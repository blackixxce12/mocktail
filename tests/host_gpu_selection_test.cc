#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "runtime/graphics_launch_policy.h"

#include "compat/guest_abi.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr unsigned int kIntel = 0x8086;
constexpr unsigned int kNvidia = 0x10de;
constexpr unsigned int kAmd = 0x1002;

// PCI paths as the kernel lays them out under /sys/devices.
constexpr char kRembrandtApu[] = "pci0000:00/0000:00:08.1/0000:05:00.0";
constexpr char kLaptopNvidia[] = "pci0000:00/0000:00:01.1/0000:01:00.0";
constexpr char kIntelIgpu[] = "pci0000:00/0000:00:02.0";
constexpr char kIntelArc[] =
    "pci0000:00/0000:00:01.0/0000:01:00.0/0000:02:01.0/0000:03:00.0";
constexpr char kAmdDgpu[] =
    "pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0/0000:0c:00.0";
constexpr char kKaveriApu[] = "pci0000:00/0000:00:01.0";

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_host_gpu_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

// The manifest name a package installs for this build's architecture.
std::string Native(const std::string& driver) {
  return driver + "." + std::string(compat::kGuestCpuName) + ".json";
}

std::string Hex(unsigned int id) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%04x", id);
  return text;
}

// A fake sysfs: <root>/class/drm/cardN/device links to the card's PCI
// function under <root>/devices, as /sys/class/drm does.
class FakeSysfs final {
 public:
  std::filesystem::path drm() const { return temporary_.root() / "class/drm"; }

  void AddCard(int number, const std::string& pci_path, unsigned int vendor,
               unsigned int device) {
    const std::filesystem::path function =
        temporary_.root() / "devices" / pci_path;
    WritePciFiles(function, vendor, device,
                  function.filename().string());
    const std::filesystem::path card = drm() / ("card" + std::to_string(number));
    std::filesystem::create_directories(card);
    std::filesystem::create_directory_symlink(function, card / "device");
    // Connectors and render nodes sit next to the cards.
    std::filesystem::create_directories(
        drm() / ("card" + std::to_string(number) + "-eDP-1"));
    std::filesystem::create_directories(
        drm() / ("renderD" + std::to_string(128 + number)));
  }

  // A card whose device directory is not a link into a PCI tree.
  void AddFlatCard(int number, unsigned int vendor, unsigned int device,
                   const std::string& slot) {
    WritePciFiles(drm() / ("card" + std::to_string(number)) / "device",
                  vendor, device, slot);
  }

 private:
  static void WritePciFiles(const std::filesystem::path& directory,
                            unsigned int vendor, unsigned int device,
                            const std::string& slot) {
    WriteFile(directory / "vendor", Hex(vendor) + "\n");
    WriteFile(directory / "device", Hex(device) + "\n");
    WriteFile(directory / "uevent",
              "DRIVER=test\nPCI_CLASS=30000\nPCI_SLOT_NAME=" + slot + "\n");
  }

  TemporaryDirectory temporary_;
};

class IcdDirectory final {
 public:
  explicit IcdDirectory(const std::vector<std::string>& names) {
    for (const std::string& name : names) {
      WriteFile(temporary_.root() / name, R"({"ICD":{}})");
    }
  }

  std::vector<std::filesystem::path> directories() const {
    return {temporary_.root()};
  }

  std::string Path(const std::string& name) const {
    return (temporary_.root() / name).string();
  }

 private:
  TemporaryDirectory temporary_;
};

HostGpu Gpu(unsigned int vendor, unsigned int device, bool integrated) {
  HostGpu gpu;
  gpu.vendor = vendor;
  gpu.device = device;
  gpu.integrated = integrated;
  return gpu;
}

const HostGpu kApu = Gpu(kAmd, 0x1681, true);
const HostGpu kNvidiaDgpu = Gpu(kNvidia, 0x2520, false);
const HostGpu kIntelGraphics = Gpu(kIntel, 0x9a49, true);
const HostGpu kArc = Gpu(kIntel, 0x5690, false);
const HostGpu kRadeon = Gpu(kAmd, 0x73ff, false);

TEST(HostGpuDetectionTest, ClassifiesCardsByVendorAndPciTopology) {
  FakeSysfs sysfs;
  sysfs.AddCard(0, kRembrandtApu, kAmd, 0x1681);
  sysfs.AddCard(1, kLaptopNvidia, kNvidia, 0x2520);
  sysfs.AddCard(2, kIntelIgpu, kIntel, 0x9a49);
  sysfs.AddCard(3, kIntelArc, kIntel, 0x5690);
  sysfs.AddCard(4, kAmdDgpu, kAmd, 0x73ff);
  sysfs.AddCard(5, kKaveriApu, kAmd, 0x130f);
  // An AMD card directly behind a root port that is not the APU bridge.
  sysfs.AddCard(6, "pci0000:00/0000:00:03.1/0000:0e:00.0", kAmd, 0x744c);
  // A second PCI domain keeps its own root bus.
  sysfs.AddCard(7, "pci0000:40/0000:40:08.1/0000:43:00.0", kAmd, 0x164e);

  const std::vector<HostGpu> gpus = DetectHostGpus(sysfs.drm());
  ASSERT_EQ(gpus.size(), 8U);
  const struct {
    unsigned int vendor;
    unsigned int device;
    const char* address;
    bool integrated;
  } expected[] = {
      {kAmd, 0x1681, "0000:05:00.0", true},
      {kNvidia, 0x2520, "0000:01:00.0", false},
      {kIntel, 0x9a49, "0000:00:02.0", true},
      {kIntel, 0x5690, "0000:03:00.0", false},
      {kAmd, 0x73ff, "0000:0c:00.0", false},
      {kAmd, 0x130f, "0000:00:01.0", true},
      {kAmd, 0x744c, "0000:0e:00.0", false},
      {kAmd, 0x164e, "0000:43:00.0", true},
  };
  for (std::size_t index = 0; index < gpus.size(); ++index) {
    EXPECT_EQ(gpus[index].vendor, expected[index].vendor) << index;
    EXPECT_EQ(gpus[index].device, expected[index].device) << index;
    EXPECT_EQ(gpus[index].pci_address, expected[index].address) << index;
    EXPECT_EQ(gpus[index].integrated, expected[index].integrated) << index;
  }
}

TEST(HostGpuDetectionTest, OrdersCardsByNumberAndSkipsOtherDevices) {
  FakeSysfs sysfs;
  sysfs.AddCard(10, kLaptopNvidia, kNvidia, 0x2520);
  sysfs.AddCard(2, kRembrandtApu, kAmd, 0x1681);
  // A virtual GPU, a platform framebuffer without PCI ids and an unreadable
  // vendor are not cards Mocktail has a Vulkan driver for.
  sysfs.AddCard(0, "pci0000:00/0000:00:01.0", 0x1af4, 0x1050);
  std::filesystem::create_directories(sysfs.drm() / "card1/device");
  WriteFile(sysfs.drm() / "card3/device/vendor", "nonsense\n");
  WriteFile(sysfs.drm() / "version", "drm 1.1.0\n");

  const std::vector<HostGpu> gpus = DetectHostGpus(sysfs.drm());
  ASSERT_EQ(gpus.size(), 2U);
  EXPECT_EQ(gpus[0].vendor, kAmd);
  EXPECT_EQ(gpus[1].vendor, kNvidia);
}

TEST(HostGpuDetectionTest, KeepsTheOldAssumptionsWithoutAPciPath) {
  FakeSysfs sysfs;
  sysfs.AddFlatCard(0, kIntel, 0x5690, "0000:03:00.0");
  sysfs.AddFlatCard(1, kAmd, 0x1681, "0000:05:00.0");
  sysfs.AddFlatCard(2, kNvidia, 0x2520, "0000:01:00.0");

  const std::vector<HostGpu> gpus = DetectHostGpus(sysfs.drm());
  ASSERT_EQ(gpus.size(), 3U);
  EXPECT_TRUE(gpus[0].integrated);
  EXPECT_FALSE(gpus[1].integrated);
  EXPECT_FALSE(gpus[2].integrated);
  EXPECT_EQ(gpus[0].pci_address, "0000:03:00.0");
  EXPECT_EQ(gpus[1].pci_address, "0000:05:00.0");
}

TEST(HostGpuDetectionTest, IgnoresAMissingDirectory) {
  TemporaryDirectory temporary;
  EXPECT_TRUE(DetectHostGpus(temporary.root() / "absent").empty());
}

TEST(GpuPreferenceTest, AutoFollowsThePrimeOffloadVariables) {
  for (const char* off : {"0", "off", "igpu"}) {
    EXPECT_EQ(ResolveGpuPreference(GpuPreference::kAuto, off, ""),
              GpuPreference::kIntegrated)
        << off;
    EXPECT_EQ(ResolveGpuPreference(GpuPreference::kAuto, "", off),
              GpuPreference::kIntegrated)
        << off;
  }
  for (const char* other : {"", "1", "on", "pci-0000_01_00_0", "10de:2520"}) {
    EXPECT_EQ(ResolveGpuPreference(GpuPreference::kAuto, other, other),
              GpuPreference::kDiscrete)
        << other;
  }
}

TEST(GpuPreferenceTest, AConfiguredCardBeatsThePrimeOffloadVariables) {
  EXPECT_EQ(ResolveGpuPreference(GpuPreference::kDiscrete, "0", "0"),
            GpuPreference::kDiscrete);
  EXPECT_EQ(ResolveGpuPreference(GpuPreference::kIntegrated, "1", "1"),
            GpuPreference::kIntegrated);
}

TEST(HostGpuSelectionTest, IntegratedPicksTheAmdApuNextToAnNvidiaCard) {
  // The hybrid laptop case: Ryzen APU + GeForce. "Integrated" used to land
  // on NVIDIA because AMD graphics only counted as discrete.
  const IcdDirectory icds({"nvidia_icd.json", Native("radeon_icd")});
  const std::vector<HostGpu> gpus = {kApu, kNvidiaDgpu};

  const HostGpuSelection integrated =
      SelectHostGpu(gpus, GpuPreference::kIntegrated, icds.directories());
  ASSERT_TRUE(integrated.gpu.has_value());
  EXPECT_EQ(integrated.gpu->vendor, kAmd);
  EXPECT_EQ(integrated.icd, icds.Path(Native("radeon_icd")));
  EXPECT_TRUE(integrated.preferred);
  EXPECT_TRUE(integrated.loader_device_select.empty());

  for (const GpuPreference preference :
       {GpuPreference::kDiscrete, GpuPreference::kAuto}) {
    const HostGpuSelection discrete =
        SelectHostGpu(gpus, preference, icds.directories());
    ASSERT_TRUE(discrete.gpu.has_value());
    EXPECT_EQ(discrete.gpu->vendor, kNvidia);
    EXPECT_EQ(discrete.icd, icds.Path("nvidia_icd.json"));
    EXPECT_TRUE(discrete.preferred);
    EXPECT_TRUE(discrete.loader_device_select.empty());
  }
}

TEST(HostGpuSelectionTest, IntelAndNvidiaLaptopsKeepTheirChoices) {
  const IcdDirectory icds({"nvidia_icd.json", Native("intel_icd"),
                           Native("intel_hasvk_icd")});
  const std::vector<HostGpu> gpus = {kIntelGraphics, kNvidiaDgpu};

  HostGpuSelection selection =
      SelectHostGpu(gpus, GpuPreference::kDiscrete, icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kNvidia);

  selection =
      SelectHostGpu(gpus, GpuPreference::kIntegrated, icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kIntel);
  // ANV and HasVK each report only the generations they support.
  EXPECT_EQ(selection.icd, icds.Path(Native("intel_icd")) + ":" +
                               icds.Path(Native("intel_hasvk_icd")));
}

TEST(HostGpuSelectionTest, ASingleCardIsAlwaysUsed) {
  // A MUX laptop in dGPU-only mode shows the NVIDIA card alone.
  const IcdDirectory icds({"nvidia_icd.json", Native("radeon_icd")});
  for (const GpuPreference preference :
       {GpuPreference::kAuto, GpuPreference::kDiscrete,
        GpuPreference::kIntegrated}) {
    const HostGpuSelection selection =
        SelectHostGpu({kNvidiaDgpu}, preference, icds.directories());
    ASSERT_TRUE(selection.gpu.has_value());
    EXPECT_EQ(selection.gpu->vendor, kNvidia);
    EXPECT_EQ(selection.icd, icds.Path("nvidia_icd.json"));
    EXPECT_EQ(selection.preferred, preference != GpuPreference::kIntegrated);
    EXPECT_TRUE(selection.loader_device_select.empty());
  }
  const IcdDirectory intel({Native("intel_icd")});
  for (const GpuPreference preference :
       {GpuPreference::kAuto, GpuPreference::kIntegrated}) {
    const HostGpuSelection selection =
        SelectHostGpu({kIntelGraphics}, preference, intel.directories());
    ASSERT_TRUE(selection.gpu.has_value());
    EXPECT_EQ(selection.icd, intel.Path(Native("intel_icd")));
  }
}

TEST(HostGpuSelectionTest, TellsTheLoaderWhichCardOfOneDriverComesFirst) {
  const IcdDirectory icds({Native("radeon_icd"), Native("intel_icd")});

  HostGpuSelection selection = SelectHostGpu(
      {kApu, kRadeon}, GpuPreference::kDiscrete, icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->device, 0x73ffU);
  EXPECT_EQ(selection.icd, icds.Path(Native("radeon_icd")));
  EXPECT_EQ(selection.loader_device_select, "0x1002:0x73ff");

  selection = SelectHostGpu({kApu, kRadeon}, GpuPreference::kIntegrated,
                            icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->device, 0x1681U);
  EXPECT_EQ(selection.loader_device_select, "0x1002:0x1681");

  selection = SelectHostGpu({kIntelGraphics, kArc}, GpuPreference::kAuto,
                            icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->device, 0x5690U);
  EXPECT_EQ(selection.loader_device_select, "0x8086:0x5690");
}

TEST(HostGpuSelectionTest, PrefersNvidiaThenAmdAmongDiscreteCards) {
  const IcdDirectory icds({"nvidia_icd.json", Native("radeon_icd"),
                           Native("intel_icd")});
  HostGpuSelection selection = SelectHostGpu(
      {kArc, kRadeon, kNvidiaDgpu}, GpuPreference::kDiscrete,
      icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kNvidia);

  selection = SelectHostGpu({kArc, kRadeon}, GpuPreference::kDiscrete,
                            icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kAmd);

  // No integrated card: the discrete ones stand in, NVIDIA first as before.
  selection = SelectHostGpu({kRadeon, kNvidiaDgpu}, GpuPreference::kIntegrated,
                            icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kNvidia);
  EXPECT_FALSE(selection.preferred);
}

TEST(HostGpuSelectionTest, SkipsCardsWithoutAVulkanDriver) {
  const IcdDirectory icds({Native("intel_icd")});
  const HostGpuSelection selection =
      SelectHostGpu({kIntelGraphics, kNvidiaDgpu}, GpuPreference::kDiscrete,
                    icds.directories());
  ASSERT_TRUE(selection.gpu.has_value());
  EXPECT_EQ(selection.gpu->vendor, kIntel);
  EXPECT_FALSE(selection.preferred);

  const IcdDirectory none({Native("lvp_icd")});
  EXPECT_FALSE(SelectHostGpu({kIntelGraphics, kNvidiaDgpu},
                             GpuPreference::kDiscrete, none.directories())
                   .gpu.has_value());
  EXPECT_FALSE(SelectHostGpu({}, GpuPreference::kDiscrete, icds.directories())
                   .gpu.has_value());
}

TEST(HostGpuSelectionTest, PinsBothNvidiaDriversWhenBothAreInstalled) {
  // NVIDIA's driver runs the card while the nvidia kernel module drives it,
  // NVK while nouveau does; each ignores the other's cards.
  const IcdDirectory icds({Native("nouveau_icd"), "nvidia_icd.json"});
  const HostGpuSelection selection =
      SelectHostGpu({kNvidiaDgpu}, GpuPreference::kAuto, icds.directories());
  EXPECT_EQ(selection.icd, icds.Path("nvidia_icd.json") + ":" +
                               icds.Path(Native("nouveau_icd")));
}

TEST(HostGpuSelectionTest, LowersQualityOnlyOnIntelIntegratedGraphics) {
  EXPECT_TRUE(RendersOnIntelIntegratedGraphics({kIntelGraphics, kNvidiaDgpu},
                                               kIntelGraphics));
  EXPECT_FALSE(RendersOnIntelIntegratedGraphics({kIntelGraphics, kNvidiaDgpu},
                                                kNvidiaDgpu));
  EXPECT_FALSE(RendersOnIntelIntegratedGraphics({kIntelGraphics, kArc}, kArc));
  EXPECT_FALSE(RendersOnIntelIntegratedGraphics({kApu}, kApu));
  // Without a selected card, only an Intel-only computer counts.
  EXPECT_TRUE(RendersOnIntelIntegratedGraphics({kIntelGraphics}, std::nullopt));
  EXPECT_FALSE(RendersOnIntelIntegratedGraphics({kIntelGraphics, kNvidiaDgpu},
                                                std::nullopt));
  EXPECT_FALSE(RendersOnIntelIntegratedGraphics({}, std::nullopt));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
