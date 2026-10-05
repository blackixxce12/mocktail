#include "launcher_ui/machine_profile.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/graphics_launch_policy.h"
#include "runtime/performance_policy.h"
#include "window/video_driver_policy.h"
#include "window/wayland_global_probe.h"
#include "window/wayland_surface_commit_guard.h"

namespace mocktail::launcher_ui {
namespace {

// graphics_launch_policy.cc kIcdDirectories.
constexpr const char* kIcdDirectories[] = {
    "/usr/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
};

constexpr const char* kLibraryDirectories[] = {
    "/usr/lib",
    "/usr/lib64",
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/usr/lib/aarch64-linux-gnu",
    "/usr/local/lib",
    "/app/lib",
};

// window.cc FindInstalledAngleLibraries, after the bundled directory.
constexpr const char* kAngleDirectories[] = {
    "/usr/lib64/chromium",
    "/usr/lib64/chromium-browser",
    "/usr/lib/chromium",
    "/usr/lib/chromium-browser",
    "/usr/lib64/electron43",
    "/usr/lib/electron43",
    "/usr/lib/electron42",
    "/usr/lib/electron41",
    "/usr/lib/electron40",
    "/usr/lib/electron39",
    "/usr/lib/cef",
    "/opt/google/chrome",
    "/opt/google/chrome-beta",
    "/opt/google/chrome-unstable",
};

constexpr std::string_view kAnglePlatformExtension = "EGL_ANGLE_platform_angle";
// libGLESv2.so of a browser is 5-15 MiB; stop well past that.
constexpr std::uintmax_t kMaximumScannedBytes = 64U * 1024U * 1024U;

std::filesystem::path Under(const std::filesystem::path& root,
                            const std::filesystem::path& absolute) {
  if (root.empty() || root == "/") {
    return absolute;
  }
  return root / absolute.relative_path();
}

bool NonEmpty(const runtime::Environment& environment, std::string_view name) {
  return environment.HasNonEmpty(name);
}

bool Exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

bool ParseHex(const std::string& text, unsigned int* value) {
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
  if (end == text.c_str() || *end != '\0' || parsed > 0xffffUL) return false;
  *value = static_cast<unsigned int>(parsed);
  return true;
}

GpuSummary DetectGpus(const std::filesystem::path& root) {
  GpuSummary gpus;
  gpus.cards = runtime::DetectHostGpus(Under(root, "/sys/class/drm"));
  for (const runtime::HostGpu& card : gpus.cards) {
    if (card.vendor == kNvidiaPciVendor) {
      gpus.nvidia = true;
    } else if (card.vendor == kAmdPciVendor) {
      gpus.amd = true;
      gpus.amd_discrete = gpus.amd_discrete || !card.integrated;
    } else if (card.vendor == kIntelPciVendor) {
      gpus.intel = true;
      gpus.intel_discrete = gpus.intel_discrete || !card.integrated;
    }
  }
  // DetectHostGpus leaves out cards no Vulkan driver of Mocktail's runs.
  for (int index = 0; index < 16; ++index) {
    std::ifstream input(Under(
        root,
        "/sys/class/drm/card" + std::to_string(index) + "/device/vendor"));
    std::string raw;
    unsigned int vendor = 0;
    if ((input >> raw) && ParseHex(raw, &vendor) &&
        vendor != kNvidiaPciVendor && vendor != kAmdPciVendor &&
        vendor != kIntelPciVendor) {
      gpus.other = true;
    }
  }
  gpus.nvidia_kernel_driver =
      window::HasNvidiaKernelDriver(Under(root, "/proc/driver/nvidia/version"),
                                    Under(root, "/sys/bus/pci/drivers/nvidia"));
  if (gpus.nvidia_kernel_driver) {
    gpus.nvidia_driver_version =
        window::NvidiaDriverVersion(Under(root, "/proc/driver/nvidia/version"),
                                    Under(root, "/sys/module/nvidia/version"));
  }
  return gpus;
}

std::uint64_t DetectMemory(const std::filesystem::path& root) {
  std::ifstream input(Under(root, "/proc/meminfo"));
  std::string key;
  std::uint64_t kilobytes = 0;
  std::string unit;
  while (input >> key >> kilobytes >> unit) {
    if (key == "MemTotal:") {
      return kilobytes * 1024U;
    }
  }
  return 0;
}

// graphics_launch_policy.cc FindLoaderSelectedGpu: the detected card
// VK_LOADER_DEVICE_SELECT ("0xVVVV:0xDDDD") names, if any.
std::optional<runtime::HostGpu> FindLoaderSelectedGpu(
    const std::vector<runtime::HostGpu>& cards, std::string_view select) {
  const std::size_t colon = select.find(':');
  unsigned int vendor = 0;
  unsigned int device = 0;
  if (colon == std::string_view::npos ||
      !ParseHex(std::string(select.substr(0, colon)), &vendor) ||
      !ParseHex(std::string(select.substr(colon + 1)), &device)) {
    return std::nullopt;
  }
  for (const runtime::HostGpu& card : cards) {
    if (card.vendor == vendor && card.device == device) return card;
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> SplitPaths(std::string_view list) {
  std::vector<std::filesystem::path> paths;
  while (!list.empty()) {
    const std::size_t colon = list.find(':');
    const std::string_view entry = list.substr(0, colon);
    if (!entry.empty() && entry.front() == '/') paths.emplace_back(entry);
    if (colon == std::string_view::npos) break;
    list.remove_prefix(colon + 1);
  }
  return paths;
}

// Where the Vulkan loader looks for driver manifests on Linux, in its order
// (Vulkan-Loader, LoaderDriverInterface.md), plus VK_ADD_DRIVER_FILES.
std::vector<std::filesystem::path> LoaderIcdLocations(
    const runtime::Environment& environment) {
  std::vector<std::filesystem::path> locations =
      SplitPaths(environment.GetOr("VK_ADD_DRIVER_FILES", ""));
  const std::string home = environment.GetOr("HOME", "");
  const auto add = [&locations](const std::filesystem::path& base) {
    locations.push_back(base / "vulkan/icd.d");
  };
  const auto add_home = [&](const char* variable, const char* fallback) {
    const std::vector<std::filesystem::path> own =
        SplitPaths(environment.GetOr(variable, ""));
    if (!own.empty()) {
      add(own.front());
    } else if (!home.empty() && home.front() == '/') {
      add(std::filesystem::path(home) / fallback);
    }
  };
  const auto add_list = [&](const char* variable, const char* fallback) {
    std::vector<std::filesystem::path> list =
        SplitPaths(environment.GetOr(variable, ""));
    if (list.empty()) list = SplitPaths(fallback);
    for (const std::filesystem::path& base : list) add(base);
  };
  add_home("XDG_CONFIG_HOME", ".config");
  add_list("XDG_CONFIG_DIRS", "/etc/xdg");
  add("/etc");
  add_home("XDG_DATA_HOME", ".local/share");
  add_list("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
  return locations;
}

// A manifest of a driver for real hardware: Mocktail disables Mesa's
// software and translation drivers for the game
// (VK_LOADER_DRIVERS_DISABLE=lvp_icd:dzn_icd:virtio_icd), SwiftShader is
// software too, and 32-bit manifests are for 32-bit programs.
bool IsHardwareManifest(const std::string& name) {
  if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) {
    return false;
  }
  for (const char* skipped : {"lvp_icd", "dzn_icd", "virtio_icd",
                              "swiftshader", "i686", "i386", "icd32"}) {
    if (name.find(skipped) != std::string::npos) return false;
  }
  return true;
}

// The first hardware driver manifest the loader would find; empty when
// there is none.
std::string FindLoaderIcd(const std::vector<std::filesystem::path>& locations,
                          const std::filesystem::path& root) {
  for (const std::filesystem::path& location : locations) {
    const std::filesystem::path path = Under(root, location);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
      if (IsHardwareManifest(path.filename().string())) return path.string();
      continue;
    }
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator iterator(path, error), end;
         !error && iterator != end; iterator.increment(error)) {
      const std::string name = iterator->path().filename().string();
      if (iterator->is_regular_file(error) && IsHardwareManifest(name)) {
        names.push_back(name);
      }
    }
    if (!names.empty()) {
      std::sort(names.begin(), names.end());
      return (path / names.front()).string();
    }
  }
  return {};
}

bool FileMentions(const std::filesystem::path& path, std::string_view needle) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumScannedBytes) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::vector<char> buffer(1U << 20);
  std::string carry;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0) break;
    std::string chunk = carry;
    chunk.append(buffer.data(), static_cast<std::size_t>(count));
    if (chunk.find(needle) != std::string::npos) {
      return true;
    }
    carry = chunk.size() >= needle.size()
                ? chunk.substr(chunk.size() - (needle.size() - 1))
                : chunk;
  }
  return false;
}

std::optional<AngleLibraries> FindAngle(const runtime::Environment& environment,
                                        const std::filesystem::path& root) {
  const std::optional<std::string> runtime_directory =
      environment.Get("MOCKTAIL_RUNTIME_LIBRARY_DIR");
  const std::optional<std::string> override_directory =
      environment.Get("MOCKTAIL_ANGLE_LIB_DIR");
  for (const std::filesystem::path& directory :
       AngleSearchDirectories(environment)) {
    const bool from_environment = override_directory.has_value() &&
                                  !override_directory->empty() &&
                                  directory == *override_directory;
    const std::filesystem::path egl = Under(root, directory / "libEGL.so");
    const std::filesystem::path gles = Under(root, directory / "libGLESv2.so");
    if (!Exists(egl) || !Exists(gles)) {
      // window.cc ResolveGraphicsLibraries uses MOCKTAIL_ANGLE_LIB_DIR as
      // it is set, without searching further.
      if (from_environment) return std::nullopt;
      continue;
    }
    AngleLibraries angle;
    angle.directory = directory;
    angle.from_environment = from_environment;
    angle.bundled =
        runtime_directory.has_value() && !runtime_directory->empty() &&
        directory == std::filesystem::path(*runtime_directory) / "angle";
    angle.label = AngleLibraryLabel(directory);
    angle.looks_like_angle = FileMentions(gles, kAnglePlatformExtension);
    // window.cc FindInstalledAngleLibraries skips a pair that does not
    // load as ANGLE and tries the next directory.
    if (!angle.looks_like_angle && !from_environment) continue;
    return angle;
  }
  return std::nullopt;
}

bool HasGameModeLibrary(const std::vector<std::filesystem::path>& directories,
                        const std::filesystem::path& root) {
  for (const std::filesystem::path& directory : directories) {
    if (Exists(Under(root, directory / "libgamemode.so.0"))) {
      return true;
    }
  }
  return false;
}

bool IsVulkanBackend(std::string_view backend) {
  // ShouldUseNativeVulkanBackend (window.cc) accepts the aliases too.
  return backend == "direct-vulkan" || backend == "vulkan" ||
         backend == "native-vulkan";
}

bool ContainsToken(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const std::size_t colon = list.find(':');
    const std::string_view entry = list.substr(0, colon);
    if (entry.size() == token.size() &&
        std::equal(entry.begin(), entry.end(), token.begin(),
                   [](char a, char b) {
                     return std::tolower(static_cast<unsigned char>(a)) ==
                            std::tolower(static_cast<unsigned char>(b));
                   })) {
      return true;
    }
    if (colon == std::string_view::npos) break;
    list.remove_prefix(colon + 1);
  }
  return false;
}

}  // namespace

int MonitorInfo::RefreshHz() const {
  return refresh_millihertz <= 0
             ? 0
             : static_cast<int>(std::lround(refresh_millihertz / 1000.0));
}

int MonitorInfo::PixelWidth() const {
  return static_cast<int>(std::lround(width * scale));
}

int MonitorInfo::PixelHeight() const {
  return static_cast<int>(std::lround(height * scale));
}

std::string VulkanDriverSelection::FileNames() const {
  std::string names;
  std::string_view list = icd;
  while (!list.empty()) {
    const std::size_t colon = list.find(':');
    const std::string_view entry = list.substr(0, colon);
    if (!entry.empty()) {
      if (!names.empty()) names += ", ";
      names += std::filesystem::path(entry).filename().string();
    }
    if (colon == std::string_view::npos) break;
    list.remove_prefix(colon + 1);
  }
  return names;
}

runtime::GpuPreference MachineProfile::ResolvedGpuPreference(
    std::string_view gpu_preference) const {
  const std::optional<runtime::GpuPreference> configured =
      runtime::ParseGpuPreference(gpu_preference);
  if (!configured.has_value() || *configured == runtime::GpuPreference::kAuto) {
    return automatic_gpu;
  }
  return *configured;
}

VulkanDriverSelection MachineProfile::VulkanDriver(
    std::string_view gpu_preference) const {
  VulkanDriverSelection driver;
  switch (vulkan_source) {
    case VulkanDriverSource::kUser:
      driver.icd = user_vulkan_drivers;
      // A memory-limit re-exec inherits the pinned drivers and names the
      // card in VK_LOADER_DEVICE_SELECT (graphics_launch_policy.cc).
      driver.gpu = FindLoaderSelectedGpu(gpu.cards, loader_device_select);
      break;
    case VulkanDriverSource::kPinned: {
      const runtime::HostGpuSelection& selection =
          ResolvedGpuPreference(gpu_preference) ==
                  runtime::GpuPreference::kIntegrated
              ? integrated_selection
              : discrete_selection;
      driver.icd = selection.icd;
      driver.gpu = selection.gpu;
      driver.preferred = selection.preferred;
      break;
    }
    case VulkanDriverSource::kLoader:
      driver.icd = loader_icd;
      break;
    case VulkanDriverSource::kNone:
    case VulkanDriverSource::kUnknown:
      break;
  }
  return driver;
}

bool MachineProfile::RendersOnIntelIntegratedGraphics(
    std::string_view gpu_preference) const {
  return runtime::RendersOnIntelIntegratedGraphics(
      gpu.cards, VulkanDriver(gpu_preference).gpu);
}

std::string GpuCardName(const runtime::HostGpu& card) {
  switch (card.vendor) {
    case kNvidiaPciVendor:
      return "NVIDIA";
    case kAmdPciVendor:
      return "AMD";
    case kIntelPciVendor:
      return card.integrated ? "Intel" : "Intel Arc";
    default:
      break;
  }
  return {};
}

window::VideoDriverPolicyInput MachineProfile::VideoDriverInput(
    std::string_view backend, std::string_view gpu_preference) const {
  window::VideoDriverPolicyInput input;
  input.prefer_wayland = prefer_wayland;
  input.has_wayland_session = wayland_available;
  input.has_x11_display = x11_available;
  input.uses_direct_vulkan = IsVulkanBackend(backend);
  input.has_nvidia_kernel_driver = gpu.nvidia_kernel_driver;
  // window.cc reads the drivers the policy pinned, or the user's own list;
  // VK_DRIVER_FILES stays unset while the loader chooses.
  if (vulkan_source == VulkanDriverSource::kUser ||
      vulkan_source == VulkanDriverSource::kPinned) {
    input.vulkan_drivers_exclude_nvidia =
        window::VulkanDriverFilesExcludeNvidia(
            VulkanDriver(gpu_preference).icd);
  }
  // window.cc ResolveNvidiaWaylandEvidence.
  input.nvidia_driver_major =
      window::NvidiaDriverMajorVersion(gpu.nvidia_driver_version);
  for (const runtime::HostGpu& card : gpu.cards) {
    if (card.vendor == kNvidiaPciVendor) {
      ++input.nvidia_gpu_count;
    } else {
      ++input.other_gpu_count;
    }
  }
  input.surface_commit_guard = surface_commit_guard;
  input.wayland_explicit_sync = wayland_explicit_sync;
  return input;
}

std::string MachineProfile::AutomaticDisplayServer(
    std::string_view backend, std::string_view gpu_preference) const {
  switch (window::ResolveVideoDriverChoice(
      VideoDriverInput(backend, gpu_preference))) {
    case window::VideoDriverChoice::kWayland:
    case window::VideoDriverChoice::kNvidiaDirectVulkanWayland:
      return "wayland";
    case window::VideoDriverChoice::kX11:
    case window::VideoDriverChoice::kNvidiaDirectVulkanX11:
      return "x11";
    case window::VideoDriverChoice::kSdlDefault:
      break;
  }
  // SDL's own order prefers Wayland.
  if (wayland_available) return "wayland";
  if (x11_available) return "x11";
  return {};
}

bool MachineProfile::NvidiaRuleApplies(std::string_view backend,
                                       std::string_view gpu_preference) const {
  return window::NvidiaDirectVulkanRuleApplies(
      VideoDriverInput(backend, gpu_preference));
}

window::NvidiaWaylandBlocker MachineProfile::NvidiaNativeWaylandBlocker(
    bool wayland_chosen) const {
  window::VideoDriverPolicyInput input =
      VideoDriverInput("direct-vulkan", "auto");
  if (wayland_chosen) input.prefer_wayland = true;
  return window::NvidiaNativeWaylandBlocker(input);
}

bool MachineProfile::NvidiaDirectVulkanUsesX11(
    std::string_view gpu_preference) const {
  return window::ResolveVideoDriverChoice(
             VideoDriverInput("direct-vulkan", gpu_preference)) ==
         window::VideoDriverChoice::kNvidiaDirectVulkanX11;
}

std::string MachineProfile::DiscreteGpuLabel() const {
  std::vector<std::string> names;
  if (gpu.nvidia) names.emplace_back("NVIDIA");
  if (gpu.amd_discrete) names.emplace_back("AMD");
  if (gpu.intel_discrete) names.emplace_back("Intel Arc");
  std::string label;
  for (const std::string& name : names) {
    if (!label.empty()) label += " + ";
    label += name;
  }
  return label;
}

std::string MachineProfile::GpuVendorsLabel() const {
  std::vector<std::string> names;
  if (gpu.nvidia) names.emplace_back("NVIDIA");
  if (gpu.amd) names.emplace_back("AMD");
  if (gpu.intel) names.emplace_back("Intel");
  std::string label;
  for (const std::string& name : names) {
    if (!label.empty()) label += " + ";
    label += name;
  }
  return label;
}

MachineProbe DefaultMachineProbe() {
  MachineProbe probe;
  for (const char* directory : kIcdDirectories) {
    probe.icd_directories.emplace_back(directory);
  }
  for (const char* directory : kLibraryDirectories) {
    probe.library_directories.emplace_back(directory);
  }
  probe.physical_cores = [] {
    return runtime::DetectAvailablePhysicalCoreCount();
  };
  probe.logical_cpus = [] {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<int>(count) : 0;
  };
  probe.wayland_globals = [] {
    // window.cc kWaylandProbeTimeout.
    return window::ProbeWaylandGlobals(std::chrono::milliseconds(500));
  };
  return probe;
}

std::vector<std::filesystem::path> AngleSearchDirectories(
    const runtime::Environment& environment) {
  std::vector<std::filesystem::path> directories;
  const std::optional<std::string> override_directory =
      environment.Get("MOCKTAIL_ANGLE_LIB_DIR");
  if (override_directory.has_value() && !override_directory->empty()) {
    directories.emplace_back(*override_directory);
  }
  const std::optional<std::string> runtime_directory =
      environment.Get("MOCKTAIL_RUNTIME_LIBRARY_DIR");
  if (runtime_directory.has_value() && !runtime_directory->empty()) {
    directories.push_back(std::filesystem::path(*runtime_directory) / "angle");
  }
  for (const char* directory : kAngleDirectories) {
    directories.emplace_back(directory);
  }
  return directories;
}

std::string AngleLibraryLabel(const std::filesystem::path& directory) {
  const std::string name = directory.filename().string();
  if (name.rfind("electron", 0) == 0 && name.size() > 8 &&
      std::all_of(name.begin() + 8, name.end(),
                  [](char c) { return c >= '0' && c <= '9'; })) {
    return "Electron " + name.substr(8);
  }
  if (name == "chromium" || name == "chromium-browser") return "Chromium";
  if (name == "chrome") return "Google Chrome";
  if (name == "chrome-beta") return "Google Chrome Beta";
  if (name == "chrome-unstable") return "Google Chrome Dev";
  if (name == "cef") return "CEF";
  return directory.string();
}

std::string DescribeDesktop(const runtime::Environment& environment) {
  struct Known {
    std::string_view token;
    std::string_view name;
  };
  constexpr Known kKnown[] = {
      {"Hyprland", "Hyprland"}, {"KDE", "KDE Plasma"},      {"GNOME", "GNOME"},
      {"COSMIC", "COSMIC"},     {"niri", "niri"},           {"sway", "Sway"},
      {"river", "river"},       {"Wayfire", "Wayfire"},     {"labwc", "labwc"},
      {"XFCE", "Xfce"},         {"X-Cinnamon", "Cinnamon"}, {"MATE", "MATE"},
      {"LXQt", "LXQt"},         {"Budgie", "Budgie"},
  };
  for (const char* variable : {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP"}) {
    const std::string list = environment.GetOr(variable, "");
    for (const Known& known : kKnown) {
      if (ContainsToken(list, known.token)) {
        return std::string(known.name);
      }
    }
  }
  // Compositors that do not always set XDG_CURRENT_DESKTOP.
  if (NonEmpty(environment, "HYPRLAND_INSTANCE_SIGNATURE")) return "Hyprland";
  if (NonEmpty(environment, "KDE_FULL_SESSION")) return "KDE Plasma";
  if (NonEmpty(environment, "GNOME_SETUP_DISPLAY")) return "GNOME";
  if (NonEmpty(environment, "NIRI_SOCKET")) return "niri";
  if (NonEmpty(environment, "SWAYSOCK")) return "Sway";
  if (NonEmpty(environment, "WAYFIRE_SOCKET")) return "Wayfire";
  if (NonEmpty(environment, "LABWC_PID")) return "labwc";
  const std::string current = environment.GetOr("XDG_CURRENT_DESKTOP", "");
  return current.substr(0, current.find(':'));
}

MachineProfile DetectMachineProfile(const runtime::Environment& environment,
                                    const MachineProbe& probe) {
  MachineProfile profile;
  profile.gpu = DetectGpus(probe.root);
  profile.desktop = DescribeDesktop(environment);
  profile.wayland_available = NonEmpty(environment, "WAYLAND_DISPLAY") &&
                              NonEmpty(environment, "XDG_RUNTIME_DIR");
  profile.x11_available = NonEmpty(environment, "DISPLAY");
  const std::string session = environment.GetOr("XDG_SESSION_TYPE", "");
  if (session == "wayland") {
    profile.session = SessionType::kWayland;
  } else if (session == "x11") {
    profile.session = SessionType::kX11;
  } else if (NonEmpty(environment, "WAYLAND_DISPLAY")) {
    profile.session = SessionType::kWayland;
  } else if (profile.x11_available) {
    profile.session = SessionType::kX11;
  }
  const std::string prefer = environment.GetOr("MOCKTAIL_PREFER_WAYLAND", "1");
  profile.prefer_wayland = prefer != "0" && prefer != "false";
  const std::optional<std::string> guard =
      environment.Get("MOCKTAIL_WAYLAND_COMMIT_GUARD");
  profile.surface_commit_guard = window::SurfaceCommitGuardAllowed(
      guard.has_value() ? guard->c_str() : nullptr);
  profile.flatpak = NonEmpty(environment, "FLATPAK_ID") ||
                    Exists(Under(probe.root, "/.flatpak-info"));
  profile.physical_cores = probe.physical_cores ? probe.physical_cores() : 0;
  profile.logical_cpus = probe.logical_cpus ? probe.logical_cpus() : 0;
  profile.memory_bytes = DetectMemory(probe.root);
  profile.gamemode_library =
      HasGameModeLibrary(probe.library_directories, probe.root);
  profile.angle = FindAngle(environment, probe.root);
  // graphics_launch_policy.cc ApplyVulkanIcdPolicy: a driver list the user
  // set is kept; otherwise Mocktail pins the drivers of the card engine.gpu
  // asks for, or leaves the choice to the loader when no card has one.
  const std::string user_files = environment.GetOr("VK_DRIVER_FILES", "");
  const std::string user_icds = environment.GetOr("VK_ICD_FILENAMES", "");
  profile.user_vulkan_drivers = user_files.empty() ? user_icds : user_files;
  profile.loader_device_select =
      environment.GetOr("VK_LOADER_DEVICE_SELECT", "");
  profile.automatic_gpu = runtime::ResolveGpuPreference(
      runtime::GpuPreference::kAuto, environment.GetOr("DRI_PRIME", ""),
      environment.GetOr("__NV_PRIME_RENDER_OFFLOAD", ""));
  std::vector<std::filesystem::path> icd_directories;
  for (const std::filesystem::path& directory : probe.icd_directories) {
    icd_directories.push_back(Under(probe.root, directory));
  }
  profile.discrete_selection = runtime::SelectHostGpu(
      profile.gpu.cards, runtime::GpuPreference::kDiscrete, icd_directories);
  profile.integrated_selection = runtime::SelectHostGpu(
      profile.gpu.cards, runtime::GpuPreference::kIntegrated, icd_directories);
  if (!profile.user_vulkan_drivers.empty()) {
    profile.vulkan_source = VulkanDriverSource::kUser;
  } else if (profile.discrete_selection.gpu.has_value()) {
    profile.vulkan_source = VulkanDriverSource::kPinned;
  } else if (std::string found =
                 FindLoaderIcd(LoaderIcdLocations(environment), probe.root);
             !found.empty()) {
    profile.loader_icd = std::move(found);
    profile.vulkan_source = VulkanDriverSource::kLoader;
  } else {
    profile.vulkan_source = profile.flatpak ? VulkanDriverSource::kUnknown
                                            : VulkanDriverSource::kNone;
  }
  // window.cc asks the compositor only when its answer is all the NVIDIA
  // rule still needs; so does the settings window, for direct Vulkan on the
  // NVIDIA card whatever graphics.backend and engine.gpu say now, and with
  // the Wayland preference on, as display.server: wayland would have it.
  window::VideoDriverPolicyInput nvidia =
      profile.VideoDriverInput("direct-vulkan", "auto");
  nvidia.vulkan_drivers_exclude_nvidia = false;
  nvidia.prefer_wayland = true;
  if (window::NeedsWaylandExplicitSyncProbe(nvidia) && probe.wayland_globals) {
    const window::WaylandGlobals globals = probe.wayland_globals();
    profile.wayland_explicit_sync =
        !globals.listed       ? window::WaylandExplicitSync::kUnknown
        : globals.drm_syncobj ? window::WaylandExplicitSync::kOffered
                              : window::WaylandExplicitSync::kAbsent;
  }
  profile.detected = true;
  return profile;
}

}  // namespace mocktail::launcher_ui
