#include "runtime/graphics_launch_policy.h"

#include "runtime/frame_rate_policy.h"
#include "runtime/managed_environment.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

// Roblox matches this regular expression against
// "<VkPhysicalDeviceProperties::vendorID>:<driverVersion>", both decimal,
// and loads its Vulkan shader pack on one thread when it matches. 4318 is
// NVIDIA's PCI vendor ID, 0x10de.
constexpr char kShaderMtDenyPatternFlag[] =
    "FStringGraphicsVulkanShaderMTDenyPattern";
constexpr char kNvidiaShaderMtDenyPattern[] = "4318:.*";

constexpr const char* kIcdDirectories[] = {
    "/usr/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
};

#if defined(__aarch64__)
constexpr char kNativeArchitecture[] = "aarch64";

constexpr const char* kForeignArchitectures[] = {
    "i686",  "i586", "i486",  "i386",    "x86_64",  "x86.",
    "amd64", "armhf", "armv7", "ppc64", "riscv64", "s390x",
};
#else
constexpr char kNativeArchitecture[] = "x86_64";

constexpr const char* kForeignArchitectures[] = {
    "i686", "i586", "i486", "i386",   "x86.",   "aarch64",
    "arm64", "armhf", "armv7", "ppc64", "riscv64", "s390x",
};
#endif

constexpr unsigned int kIntelVendor = 0x8086;
constexpr unsigned int kNvidiaVendor = 0x10de;
constexpr unsigned int kAmdVendor = 0x1002;

// Zen APUs put their graphics behind the internal bridge at device 8 of the
// root bus (00:08.1 on every APU from Raven Ridge to Strix Halo); external
// PCIe slots and laptop dGPUs hang off root ports at other device numbers.
constexpr unsigned int kAmdApuBridgeDevice = 0x08;

bool ForeignArchitecture(const std::string& name) {
  for (const char* architecture : kForeignArchitectures) {
    if (name.find(architecture) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool SetValue(const char* name, const std::string& value, std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish resolved graphics setting: ") + name;
  }
  return false;
}

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  return SetValue(name, value, error);
}

bool ClearValue(const char* name, std::string* error) {
  if (unsetenv(name) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot clear graphics setting: ") + name;
  }
  return false;
}

// window.cc's video-driver policy reads these switches when it creates the
// game window; a forced driver beats its NVIDIA XWayland rule.
bool ApplyDisplayServer(DisplayServer server, std::string* error) {
  switch (server) {
    case DisplayServer::kWayland:
      return SetValue("MOCKTAIL_FORCE_WAYLAND", "1", error) &&
             ClearValue("MOCKTAIL_FORCE_X11", error) &&
             ClearValue("MOCKTAIL_ANGLE_FORCE_X11", error);
    case DisplayServer::kX11:
      return SetValue("MOCKTAIL_FORCE_X11", "1", error) &&
             ClearValue("MOCKTAIL_FORCE_WAYLAND", error);
    case DisplayServer::kAuto:
      break;
  }
  return true;
}

// "default" is engine.graphics_quality's own word for an unset level.
bool GraphicsQualityLeftToDefault() {
  const char* current = std::getenv("MOCKTAIL_GRAPHICS_QUALITY");
  return current == nullptr || current[0] == '\0' ||
         std::strcmp(current, "default") == 0;
}

bool DenyNvidiaShaderLoadingThreads(std::string* error) {
  const char* current = std::getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON");
  std::string merged;
  if (!MergeNvidiaShaderLoadingClientSettingsOverrides(
          false, current != nullptr ? current : "", &merged, error)) {
    return false;
  }
  return SetValue("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON", merged, error);
}

bool IsStrictOpenGlName(const std::string& name) {
  return name == "opengl" || name == "gles";
}

bool PrimeOffloadIsOff(std::string_view value) {
  return value == "0" || value == "off" || value == "igpu";
}

std::string_view EnvValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view() : std::string_view(value);
}

// The same decision as present_mode_policy.cc ResolvePresentModePolicy:
// Mesa's WSI replaces the present mode Mocktail's adapter chose with
// MESA_VK_WSI_PRESENT_MODE, so Vertical sync On must win over the unlimited
// frame rate here too.
bool UnthrottledPresentation(const RuntimeConfig& config) {
  if (config.vsync_mode() == "on" || config.vsync_mode() == "1") {
    return false;
  }
  if (config.vsync_mode() == "off" || config.vsync_mode() == "0") {
    return true;
  }
  return config.frame_rate().mode == FrameRateLimitMode::kUnlimited;
}

bool ParsePciId(const std::string& raw, unsigned int* id) {
  if (id == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw.c_str(), &end, 16);
  if (end == raw.c_str() || *end != '\0' || parsed > 0xffffUL) {
    return false;
  }
  *id = static_cast<unsigned int>(parsed);
  return true;
}

bool ReadPciId(const std::filesystem::path& path, unsigned int* id) {
  std::ifstream input(path);
  std::string raw;
  return static_cast<bool>(input >> raw) && ParsePciId(raw, id);
}

bool IsHexDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char character) {
           return std::isxdigit(static_cast<unsigned char>(character)) != 0;
         });
}

// domain:bus:device.function, as in 0000:c4:00.0. Domains can be longer
// than four digits (VMD), so only the ":bus:device.function" tail has a
// fixed length.
bool IsPciAddress(std::string_view name) {
  if (name.size() < 12) {
    return false;
  }
  const std::size_t domain = name.size() - 8;
  return IsHexDigits(name.substr(0, domain)) && name[domain] == ':' &&
         IsHexDigits(name.substr(domain + 1, 2)) && name[domain + 3] == ':' &&
         IsHexDigits(name.substr(domain + 4, 2)) && name[domain + 6] == '.' &&
         name[domain + 7] >= '0' && name[domain + 7] <= '7';
}

unsigned int PciDeviceNumber(std::string_view address) {
  const std::string digits(address.substr(address.size() - 4, 2));
  return static_cast<unsigned int>(std::strtoul(digits.c_str(), nullptr, 16));
}

// The PCI functions from the root bus down to the card, from the resolved
// sysfs device path (/sys/devices/pci0000:00/0000:00:08.1/0000:c4:00.0).
std::vector<std::string> PciPath(const std::filesystem::path& device) {
  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::canonical(device, error);
  std::vector<std::string> path;
  if (error) {
    return path;
  }
  for (const std::filesystem::path& component : resolved) {
    const std::string name = component.string();
    if (IsPciAddress(name)) {
      path.push_back(name);
    }
  }
  return path;
}

std::string ReadUeventValue(const std::filesystem::path& path,
                            std::string_view key) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
        line[key.size()] == '=') {
      return line.substr(key.size() + 1);
    }
  }
  return {};
}

bool IsIntegrated(unsigned int vendor, const std::vector<std::string>& path) {
  if (vendor == kNvidiaVendor) {
    return false;
  }
  if (path.empty()) {
    // No PCI topology to go by: the old assumption, Intel graphics are
    // integrated and AMD graphics are discrete.
    return vendor == kIntelVendor;
  }
  // A card on the root bus itself is part of the processor: every Intel
  // iGPU (00:02.0) and the pre-Zen AMD APUs (00:01.0). Discrete cards
  // always sit behind a root port.
  if (path.size() == 1) {
    return true;
  }
  return vendor == kAmdVendor && path.size() == 2 &&
         PciDeviceNumber(path.front()) == kAmdApuBridgeDevice;
}

bool IsCardName(const std::string& name, unsigned long* number) {
  constexpr std::string_view kPrefix = "card";
  if (name.size() <= kPrefix.size() ||
      name.compare(0, kPrefix.size(), kPrefix) != 0 ||
      !std::all_of(name.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
                   name.end(), [](char character) {
                     return std::isdigit(static_cast<unsigned char>(
                                character)) != 0;
                   })) {
    return false;
  }
  *number = std::strtoul(name.c_str() + kPrefix.size(), nullptr, 10);
  return true;
}

// Vulkan manifests of the drivers that can run a vendor's cards. Both of a
// vendor's drivers are pinned when installed: each one only reports the
// cards it supports (ANV Gen9+, HasVK Gen7/8; NVIDIA's own driver, NVK on
// nouveau), so the loader still finds the card whichever applies.
std::vector<const char*> VendorIcdNeedles(unsigned int vendor) {
  switch (vendor) {
    case kNvidiaVendor:
      return {"nvidia_icd", "nouveau_icd"};
    case kAmdVendor:
      return {"radeon_icd"};
    case kIntelVendor:
      return {"intel_icd", "intel_hasvk_icd"};
    default:
      break;
  }
  return {};
}

int VendorRank(unsigned int vendor, bool integrated) {
  // Discrete: NVIDIA, then AMD, then Intel Arc. Integrated: Intel, then AMD.
  switch (vendor) {
    case kNvidiaVendor:
      return integrated ? 2 : 0;
    case kAmdVendor:
      return 1;
    case kIntelVendor:
      return integrated ? 0 : 2;
    default:
      break;
  }
  return 3;
}

const char* VendorName(unsigned int vendor) {
  switch (vendor) {
    case kNvidiaVendor:
      return "NVIDIA";
    case kAmdVendor:
      return "AMD";
    case kIntelVendor:
      return "Intel";
    default:
      break;
  }
  return "unknown";
}

std::vector<std::filesystem::path> IcdDirectories() {
  std::vector<std::filesystem::path> directories;
  for (const char* directory : kIcdDirectories) {
    directories.emplace_back(directory);
  }
  return directories;
}

// Match Mesa ANV: 75% of RAM when the machine has more than 4GiB, else 50%.
// Forcing 50 on an 8GiB UHD 620 iGPU advertises a smaller Vk heap than ANV
// would, which increases BO eviction inside GEM_EXECBUFFER2.
const char* AnvSysMemLimitPercent() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  unsigned long kb = 0;
  std::string unit;
  while (input >> key >> kb >> unit) {
    if (key == "MemTotal:") {
      return kb > 4UL * 1024UL * 1024UL ? "75" : "50";
    }
  }
  return "50";
}

// The detected card VK_LOADER_DEVICE_SELECT (vendor:device in hex, as the
// loader reads it) names, if any.
std::optional<HostGpu> FindLoaderSelectedGpu(const std::vector<HostGpu>& gpus,
                                             std::string_view select) {
  const std::size_t colon = select.find(':');
  unsigned int vendor = 0;
  unsigned int device = 0;
  if (colon == std::string_view::npos ||
      !ParsePciId(std::string(select.substr(0, colon)), &vendor) ||
      !ParsePciId(std::string(select.substr(colon + 1)), &device)) {
    return std::nullopt;
  }
  for (const HostGpu& gpu : gpus) {
    if (gpu.vendor == vendor && gpu.device == device) {
      return gpu;
    }
  }
  return std::nullopt;
}

// Pins the Vulkan driver of the card engine.gpu (or DRI_PRIME and
// __NV_PRIME_RENDER_OFFLOAD under auto) asks for. `selected` receives that
// card, or stays empty while the user pins the drivers or no card has one.
bool ApplyVulkanIcdPolicy(const std::vector<HostGpu>& gpus,
                          GpuPreference configured,
                          std::optional<HostGpu>* selected,
                          std::string* error) {
  // Drop software/emulation ICDs even when the user already pinned a driver
  // list. Old loaders ignore this variable.
  if (!SetDefault("VK_LOADER_DRIVERS_DISABLE",
                  "lvp_icd:dzn_icd:virtio_icd", error)) {
    return false;
  }
  const char* existing_files = std::getenv("VK_DRIVER_FILES");
  const char* existing_icds = std::getenv("VK_ICD_FILENAMES");
  if ((existing_files != nullptr && existing_files[0] != '\0') ||
      (existing_icds != nullptr && existing_icds[0] != '\0')) {
    // A memory-limit re-exec inherits what the first pass pinned, including
    // the card VK_LOADER_DEVICE_SELECT names when one driver has two.
    if (selected != nullptr) {
      *selected =
          FindLoaderSelectedGpu(gpus, EnvValue("VK_LOADER_DEVICE_SELECT"));
    }
    return true;
  }
  const GpuPreference preference =
      ResolveGpuPreference(configured, EnvValue("DRI_PRIME"),
                           EnvValue("__NV_PRIME_RENDER_OFFLOAD"));
  const HostGpuSelection selection =
      SelectHostGpu(gpus, preference, IcdDirectories());
  if (!selection.gpu.has_value()) {
    return true;
  }
  const HostGpu& gpu = *selection.gpu;
  std::fprintf(stderr, "  [runtime] vulkan GPU=%s %04x:%04x %s%s%s ICD=%s\n",
               VendorName(gpu.vendor), gpu.vendor, gpu.device,
               gpu.integrated ? "integrated" : "discrete",
               gpu.pci_address.empty() ? "" : " at ", gpu.pci_address.c_str(),
               selection.icd.c_str());
  if (!selection.preferred && configured != GpuPreference::kAuto) {
    std::fprintf(stderr,
                 "  [runtime] engine.gpu=%s, but no %s graphics card has a "
                 "Vulkan driver here\n",
                 std::string(GpuPreferenceName(configured)).c_str(),
                 std::string(GpuPreferenceName(configured)).c_str());
  }
  if (!SetDefault("VK_DRIVER_FILES", selection.icd, error) ||
      !SetDefault("VK_ICD_FILENAMES", selection.icd, error)) {
    return false;
  }
  // Mocktail turns Mesa's device-select layer off (NODEVICE_SELECT=1 in
  // window.cc), so DRI_PRIME does not order one driver's cards; the loader
  // puts this one first instead. Left alone, it lists discrete cards first.
  if (!selection.loader_device_select.empty() &&
      !SetDefault("VK_LOADER_DEVICE_SELECT", selection.loader_device_select,
                  error)) {
    return false;
  }
  if (selected != nullptr) {
    *selected = gpu;
  }
  return true;
}

}  // namespace

bool MergeNvidiaShaderLoadingClientSettingsOverrides(
    bool nvidia_shader_mt, std::string_view base_json,
    std::string* merged_json, std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "NVIDIA shader loading client-settings output is required";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? std::string_view("{}") : base_json, nullptr, false,
      true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (!nvidia_shader_mt && !overrides.contains(kShaderMtDenyPatternFlag)) {
    overrides[kShaderMtDenyPatternFlag] = kNvidiaShaderMtDenyPattern;
  }
  *merged_json = overrides.dump();
  return true;
}

bool RendersOnIntelIntegratedGraphics(const std::vector<HostGpu>& gpus,
                                      const std::optional<HostGpu>& selected) {
  if (selected.has_value()) {
    return selected->vendor == kIntelVendor && selected->integrated;
  }
  // The user pins the drivers, or no card has one: Intel-only machines.
  return !gpus.empty() &&
         std::all_of(gpus.begin(), gpus.end(), [](const HostGpu& gpu) {
           return gpu.vendor == kIntelVendor;
         });
}

std::vector<HostGpu> DetectHostGpus(
    const std::filesystem::path& drm_class_directory) {
  std::vector<std::pair<unsigned long, HostGpu>> cards;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(drm_class_directory,
                                                    error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    unsigned long number = 0;
    if (!IsCardName(iterator->path().filename().string(), &number)) {
      continue;
    }
    const std::filesystem::path device = iterator->path() / "device";
    HostGpu gpu;
    if (!ReadPciId(device / "vendor", &gpu.vendor) ||
        VendorIcdNeedles(gpu.vendor).empty()) {
      continue;
    }
    (void)ReadPciId(device / "device", &gpu.device);
    const std::vector<std::string> path = PciPath(device);
    gpu.pci_address = path.empty()
                          ? ReadUeventValue(device / "uevent", "PCI_SLOT_NAME")
                          : path.back();
    gpu.integrated = IsIntegrated(gpu.vendor, path);
    const bool duplicate =
        !gpu.pci_address.empty() &&
        std::any_of(cards.begin(), cards.end(), [&](const auto& card) {
          return card.second.pci_address == gpu.pci_address;
        });
    if (!duplicate) {
      cards.emplace_back(number, std::move(gpu));
    }
  }
  std::sort(cards.begin(), cards.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });
  std::vector<HostGpu> gpus;
  gpus.reserve(cards.size());
  for (auto& card : cards) {
    gpus.push_back(std::move(card.second));
  }
  return gpus;
}

GpuPreference ResolveGpuPreference(GpuPreference configured,
                                   std::string_view dri_prime,
                                   std::string_view nv_prime_render_offload) {
  if (configured != GpuPreference::kAuto) {
    return configured;
  }
  return PrimeOffloadIsOff(dri_prime) ||
                 PrimeOffloadIsOff(nv_prime_render_offload)
             ? GpuPreference::kIntegrated
             : GpuPreference::kDiscrete;
}

HostGpuSelection SelectHostGpu(
    const std::vector<HostGpu>& gpus, GpuPreference preference,
    const std::vector<std::filesystem::path>& icd_directories) {
  const bool want_integrated = preference == GpuPreference::kIntegrated;
  std::vector<std::size_t> order(gpus.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  const auto rank = [&](std::size_t index) {
    const HostGpu& gpu = gpus[index];
    return std::make_pair(gpu.integrated == want_integrated ? 0 : 1,
                          VendorRank(gpu.vendor, gpu.integrated));
  };
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     return rank(a) < rank(b);
                   });

  HostGpuSelection selection;
  for (const std::size_t index : order) {
    const HostGpu& gpu = gpus[index];
    std::string icd;
    for (const char* needle : VendorIcdNeedles(gpu.vendor)) {
      const std::string manifest =
          SelectVulkanIcdManifest(icd_directories, needle);
      if (!manifest.empty()) {
        icd += (icd.empty() ? "" : ":") + manifest;
      }
    }
    if (icd.empty()) {
      continue;
    }
    selection.gpu = gpu;
    selection.icd = std::move(icd);
    selection.preferred = gpu.integrated == want_integrated;
    // Another card on the same drivers is reported by the same loader
    // drivers, so the loader must be told which one comes first.
    const bool shares_drivers =
        std::any_of(gpus.begin(), gpus.end(), [&](const HostGpu& other) {
          return &other != &gpu && other.vendor == gpu.vendor;
        });
    if (shares_drivers && gpu.device != 0) {
      // PCI IDs are 16-bit; the buffer also fits any unsigned int.
      char select[32];
      std::snprintf(select, sizeof(select), "0x%04x:0x%04x", gpu.vendor,
                    gpu.device);
      selection.loader_device_select = select;
    }
    break;
  }
  return selection;
}

std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor) {
  if (vendor.empty()) {
    return {};
  }
  for (const std::filesystem::path& directory : directories) {
    std::error_code error;
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error)) {
        continue;
      }
      const std::string name = iterator->path().filename().string();
      if (name.find(".json") == std::string::npos ||
          name.find(vendor) == std::string::npos || ForeignArchitecture(name)) {
        continue;
      }
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string generic;
    for (const std::string& name : names) {
      const std::string path = (directory / name).string();
      if (access(path.c_str(), R_OK) != 0) {
        continue;
      }
      if (name.find(kNativeArchitecture) != std::string::npos) {
        return path;
      }
      if (generic.empty()) {
        generic = path;
      }
    }
    if (!generic.empty()) {
      return generic;
    }
  }
  return {};
}

bool UserSelectsVideoDriver(const std::vector<std::string>& user_environment) {
  for (const std::string_view name : kUserVideoDriverVariables) {
    const std::string owned(name);
    if (std::find(user_environment.begin(), user_environment.end(), owned) !=
            user_environment.end() &&
        std::getenv(owned.c_str()) != nullptr) {
      return true;
    }
  }
  return false;
}

DisplayServer AvailableDisplayServer(DisplayServer configured) {
  const auto non_empty = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
  };
  if (configured == DisplayServer::kWayland &&
      !(non_empty("WAYLAND_DISPLAY") && non_empty("XDG_RUNTIME_DIR"))) {
    return DisplayServer::kAuto;
  }
  if (configured == DisplayServer::kX11 && !non_empty("DISPLAY")) {
    return DisplayServer::kAuto;
  }
  return configured;
}

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error) {
  return ApplyGraphicsLaunchPolicy(
      config, CaptureUserManagedEnvironment(environ), error);
}

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               const std::vector<std::string>& user_environment,
                               std::string* error) {
  if (config.graphics_backend() == GraphicsBackend::kUnknown) {
    if (error != nullptr) {
      *error = "cannot apply an unknown graphics backend";
    }
    return false;
  }
  if (!config.display().server_valid) {
    if (error != nullptr) {
      *error = "cannot apply an invalid display server";
    }
    return false;
  }
  if (!config.engine().nvidia_shader_mt_valid) {
    if (error != nullptr) {
      *error = "cannot apply an invalid NVIDIA shader loading policy";
    }
    return false;
  }
  if (!UserSelectsVideoDriver(user_environment) &&
      !ApplyDisplayServer(AvailableDisplayServer(config.display().server),
                          error)) {
    return false;
  }

  const bool direct_vulkan =
      config.graphics_backend() == GraphicsBackend::kVulkan;
  if (!SetValue("MOCKTAIL_GRAPHICS_BACKEND",
                config.graphics_backend_name(), error) ||
      !SetValue("MOCKTAIL_NVIDIA_SHADER_MT",
                config.engine().nvidia_shader_mt ? "1" : "0", error) ||
      !SetValue("MOCKTAIL_PRELOAD_VULKAN_SHIM",
                direct_vulkan ? "1" : "0", error) ||
      !SetDefault("MOCKTAIL_REQUIRE_REAL_GRAPHICS", "1", error)) {
    return false;
  }

  if (IsStrictOpenGlName(config.graphics_backend_name()) &&
      (!SetValue("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", error) ||
       !SetValue("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", error))) {
    return false;
  }

  if (direct_vulkan) {
    const char* wsi_mode =
        UnthrottledPresentation(config) ? "immediate" : "mailbox";
    const std::vector<HostGpu> gpus = DetectHostGpus();
    std::optional<HostGpu> selected;
    // Roblox's loader holds its own lock around every fseek/fread pair on
    // the shared pack FILE, so several threads are safe on every vendor;
    // engine.nvidia_shader_mt: false brings back the old NVIDIA deny.
    if ((!config.engine().nvidia_shader_mt &&
         !DenyNvidiaShaderLoadingThreads(error)) ||
        !SetDefault("ANV_SYS_MEM_LIMIT", AnvSysMemLimitPercent(), error) ||
        !SetDefault("MESA_VK_WSI_PRESENT_MODE", wsi_mode, error) ||
        // Move GEM_EXECBUFFER2 off the application thread onto Mesa's submit
        // worker so the render thread is not stuck in i915 ioctl.
        !SetDefault("MESA_VK_ENABLE_SUBMIT_THREAD", "1", error) ||
        !ApplyVulkanIcdPolicy(gpus, config.engine().gpu, &selected, error)) {
      return false;
    }
    // Low FRM only when the game renders on Intel integrated graphics. A
    // discrete card keeps the desktop quality default. A configured
    // engine.graphics_quality wins: publishing "1" here would beat the YAML
    // value in every later config load, because the environment wins there.
    if (RendersOnIntelIntegratedGraphics(gpus, selected) &&
        config.engine().graphics_quality.mode == GraphicsQualityMode::kDefault &&
        GraphicsQualityLeftToDefault() &&
        !SetValue("MOCKTAIL_GRAPHICS_QUALITY", "1", error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
