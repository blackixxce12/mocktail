#include "runtime/graphics_launch_policy.h"

#include "runtime/frame_rate_policy.h"
#include "runtime/managed_environment.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
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

struct HostGpus {
  bool intel = false;
  bool nvidia = false;
  bool amd = false;
};

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

bool EnvIsOff(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
          std::strcmp(value, "igpu") == 0);
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

bool ParsePciVendor(const std::string& raw, unsigned int* vendor) {
  if (vendor == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw.c_str(), &end, 16);
  if (end == raw.c_str() || parsed > 0xffffUL) {
    return false;
  }
  *vendor = static_cast<unsigned int>(parsed);
  return true;
}

HostGpus DetectHostGpus() {
  HostGpus gpus;
  for (int index = 0; index < 16; ++index) {
    const std::string path = "/sys/class/drm/card" + std::to_string(index) +
                             "/device/vendor";
    std::ifstream input(path);
    std::string raw;
    unsigned int vendor = 0;
    if (!(input >> raw) || !ParsePciVendor(raw, &vendor)) {
      continue;
    }
    if (vendor == 0x8086) {
      gpus.intel = true;
    } else if (vendor == 0x10de) {
      gpus.nvidia = true;
    } else if (vendor == 0x1002) {
      gpus.amd = true;
    }
  }
  return gpus;
}

std::string FindIcdFile(const char* filename_needle) {
  if (filename_needle == nullptr || filename_needle[0] == '\0') {
    return {};
  }
  std::vector<std::filesystem::path> directories;
  for (const char* directory : kIcdDirectories) {
    directories.emplace_back(directory);
  }
  return SelectVulkanIcdManifest(directories, filename_needle);
}

std::string SelectHardwareIcd(const HostGpus& gpus) {
  const bool prefer_discrete = !EnvIsOff("DRI_PRIME") &&
                               !EnvIsOff("__NV_PRIME_RENDER_OFFLOAD");
  if (prefer_discrete && gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    if (!nvidia.empty()) {
      return nvidia;
    }
  }
  if (prefer_discrete && gpus.amd) {
    const std::string amd = FindIcdFile("radeon_icd");
    if (!amd.empty()) {
      return amd;
    }
  }
  if (gpus.intel) {
    std::string intel = FindIcdFile("intel_icd");
    if (intel.empty()) {
      intel = FindIcdFile("intel_hasvk_icd");
    }
    return intel;
  }
  if (gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    return nvidia;
  }
  if (gpus.amd) {
    return FindIcdFile("radeon_icd");
  }
  return {};
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

bool ApplyVulkanIcdPolicy(const HostGpus& gpus, std::string* error) {
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
    return true;
  }
  const std::string icd = SelectHardwareIcd(gpus);
  if (icd.empty()) {
    return true;
  }
  std::fprintf(stderr, "  [runtime] vulkan ICD=%s\n", icd.c_str());
  return SetDefault("VK_DRIVER_FILES", icd, error) &&
         SetDefault("VK_ICD_FILENAMES", icd, error);
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
    const HostGpus gpus = DetectHostGpus();
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
        !ApplyVulkanIcdPolicy(gpus, error)) {
      return false;
    }
    // Low FRM only on Intel-only machines. Hybrid NVIDIA/AMD laptops should
    // keep the desktop quality default on the discrete GPU. A configured
    // engine.graphics_quality wins: publishing "1" here would beat the YAML
    // value in every later config load, because the environment wins there.
    if (gpus.intel && !gpus.nvidia && !gpus.amd &&
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
