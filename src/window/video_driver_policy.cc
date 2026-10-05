#include "window/video_driver_policy.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

namespace mocktail {
namespace window {

bool NvidiaDirectVulkanRuleApplies(const VideoDriverPolicyInput& input) {
  return !input.has_explicit_sdl_driver && !input.force_wayland &&
         !(input.force_x11 && input.has_x11_display) &&
         input.uses_direct_vulkan && input.has_nvidia_kernel_driver &&
         !input.vulkan_drivers_exclude_nvidia && input.has_wayland_session &&
         input.has_x11_display;
}

NvidiaWaylandBlocker NvidiaNativeWaylandBlocker(
    const VideoDriverPolicyInput& input) {
  if (!input.prefer_wayland) {
    return NvidiaWaylandBlocker::kWaylandNotPreferred;
  }
  if (!input.surface_commit_guard) {
    return NvidiaWaylandBlocker::kCommitGuardOff;
  }
  if (input.nvidia_driver_major <= 0) {
    return NvidiaWaylandBlocker::kDriverVersionUnknown;
  }
  if (input.nvidia_driver_major < kNvidiaExplicitSyncDriverMajor) {
    return NvidiaWaylandBlocker::kDriverWithoutExplicitSync;
  }
  if (input.nvidia_explicit_sync_disabled) {
    return NvidiaWaylandBlocker::kExplicitSyncDisabled;
  }
  if (input.other_gpu_count > 0) {
    return NvidiaWaylandBlocker::kOtherGpu;
  }
  if (input.nvidia_gpu_count <= 0) {
    return NvidiaWaylandBlocker::kNoNvidiaGpuListed;
  }
  switch (input.wayland_explicit_sync) {
    case WaylandExplicitSync::kOffered:
      break;
    case WaylandExplicitSync::kAbsent:
      return NvidiaWaylandBlocker::kCompositorWithoutExplicitSync;
    case WaylandExplicitSync::kUnknown:
      return NvidiaWaylandBlocker::kExplicitSyncUnknown;
  }
  // The same registry probe answers whether the compositor is Hyprland.
  if (!input.unthrottled_presentation && !input.hyprland_compositor) {
    return NvidiaWaylandBlocker::kVsyncOutsideHyprland;
  }
  return NvidiaWaylandBlocker::kNone;
}

bool NeedsWaylandExplicitSyncProbe(const VideoDriverPolicyInput& input) {
  return NvidiaDirectVulkanRuleApplies(input) &&
         input.wayland_explicit_sync == WaylandExplicitSync::kUnknown &&
         NvidiaNativeWaylandBlocker(input) ==
             NvidiaWaylandBlocker::kExplicitSyncUnknown;
}

VideoDriverChoice ResolveVideoDriverChoice(
    const VideoDriverPolicyInput& input) {
  if (input.has_explicit_sdl_driver) {
    return VideoDriverChoice::kSdlDefault;
  }
  if (input.force_wayland) {
    return VideoDriverChoice::kWayland;
  }
  if (input.force_x11 && input.has_x11_display) {
    return VideoDriverChoice::kX11;
  }
  if (NvidiaDirectVulkanRuleApplies(input)) {
    return NvidiaNativeWaylandBlocker(input) == NvidiaWaylandBlocker::kNone
               ? VideoDriverChoice::kNvidiaDirectVulkanWayland
               : VideoDriverChoice::kNvidiaDirectVulkanX11;
  }
  if (input.prefer_wayland && input.has_wayland_session) {
    return VideoDriverChoice::kWayland;
  }
  return VideoDriverChoice::kSdlDefault;
}

const char* VideoDriverChoiceName(VideoDriverChoice choice) {
  switch (choice) {
    case VideoDriverChoice::kWayland:
    case VideoDriverChoice::kNvidiaDirectVulkanWayland:
      return "wayland";
    case VideoDriverChoice::kX11:
    case VideoDriverChoice::kNvidiaDirectVulkanX11:
      return "x11";
    case VideoDriverChoice::kSdlDefault:
      return nullptr;
  }
  return nullptr;
}

std::string NvidiaWaylandBlockerReason(NvidiaWaylandBlocker blocker,
                                       std::string_view driver_version) {
  switch (blocker) {
    case NvidiaWaylandBlocker::kWaylandNotPreferred:
      return "MOCKTAIL_PREFER_WAYLAND=0";
    case NvidiaWaylandBlocker::kCommitGuardOff:
      return "the surface-commit guard is off "
             "(MOCKTAIL_WAYLAND_COMMIT_GUARD=0)";
    case NvidiaWaylandBlocker::kDriverVersionUnknown:
      return "NVIDIA driver version unknown";
    case NvidiaWaylandBlocker::kDriverWithoutExplicitSync:
      return "driver " +
             std::string(driver_version.empty() ? "(unknown)"
                                                : driver_version) +
             " has no explicit sync; " +
             std::to_string(kNvidiaExplicitSyncDriverMajor) +
             " or newer needed";
    case NvidiaWaylandBlocker::kExplicitSyncDisabled:
      return std::string(kNvidiaDisableExplicitSyncVariable) +
             " turns NVIDIA's explicit sync off";
    case NvidiaWaylandBlocker::kOtherGpu:
      return "NVIDIA with another GPU";
    case NvidiaWaylandBlocker::kNoNvidiaGpuListed:
      return "no NVIDIA card under /sys/class/drm";
    case NvidiaWaylandBlocker::kExplicitSyncUnknown:
      return "the Wayland registry probe failed";
    case NvidiaWaylandBlocker::kCompositorWithoutExplicitSync:
      return "the compositor has no wp_linux_drm_syncobj_manager_v1";
    case NvidiaWaylandBlocker::kVsyncOutsideHyprland:
      return "vertical sync may make the swapchain wait for the display, "
             "which can stall vkAcquireNextImageKHR while the window is "
             "hidden, and the compositor is not Hyprland; graphics.vsync: "
             "off or frame_rate_limit: unlimited present without waiting";
    case NvidiaWaylandBlocker::kNone:
      break;
  }
  return {};
}

bool NvidiaExplicitSyncDisabled(const char* value) {
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

VulkanDriverReach ClassifyVulkanDriverLibrary(std::string_view library_path) {
  const std::size_t slash = library_path.rfind('/');
  const std::string_view name =
      slash == library_path.npos ? library_path
                                 : library_path.substr(slash + 1);
  if (name.empty()) {
    return VulkanDriverReach::kUnknown;
  }
  // nvidia_icd.json names libGLX_nvidia.so.0 (nvidia-utils 615.71.09);
  // NVK's manifest names libvulkan_nouveau.so.
  if (name.find("nvidia") != name.npos || name.find("nouveau") != name.npos) {
    return VulkanDriverReach::kNvidia;
  }
  // Mesa builds its drivers as libvulkan_<name>.so (radeon_icd.json names
  // libvulkan_radeon.so in vulkan-radeon 26.2.4); AMDVLK's amd_icd64.json
  // and amd_icd32.json name amdvlk64.so and amdvlk32.so.
  constexpr std::string_view kOtherVendorLibraries[] = {
      "libvulkan_radeon.so", "libvulkan_intel.so", "libvulkan_intel_hasvk.so",
      "amdvlk64.so",         "amdvlk32.so",
  };
  return std::find(std::begin(kOtherVendorLibraries),
                   std::end(kOtherVendorLibraries),
                   name) != std::end(kOtherVendorLibraries)
             ? VulkanDriverReach::kOtherVendor
             : VulkanDriverReach::kUnknown;
}

VulkanDriverReach ReadVulkanDriverManifestReach(
    const std::filesystem::path& manifest) {
  // Driver manifests are a few hundred bytes; anything far larger is not
  // one.
  constexpr std::size_t kMaximumManifestBytes = 64 * 1024;
  std::error_code error;
  if (!std::filesystem::is_regular_file(manifest, error)) {
    return VulkanDriverReach::kUnknown;
  }
  std::ifstream input(manifest, std::ios::binary);
  std::string text(kMaximumManifestBytes + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  const std::streamsize read = input.gcount();
  if (read <= 0 || static_cast<std::size_t>(read) > kMaximumManifestBytes) {
    return VulkanDriverReach::kUnknown;
  }
  text.resize(static_cast<std::size_t>(read));
  // The Vulkan loader's manifest format: {"file_format_version": ...,
  // "ICD": {"library_path": ..., "api_version": ...}}.
  const nlohmann::json document =
      nlohmann::json::parse(text, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    return VulkanDriverReach::kUnknown;
  }
  const auto icd = document.find("ICD");
  if (icd == document.end() || !icd->is_object()) {
    return VulkanDriverReach::kUnknown;
  }
  const auto library = icd->find("library_path");
  if (library == icd->end() || !library->is_string()) {
    return VulkanDriverReach::kUnknown;
  }
  return ClassifyVulkanDriverLibrary(library->get_ref<const std::string&>());
}

bool VulkanDriverFilesExcludeNvidia(std::string_view driver_files) {
  bool named = false;
  std::size_t begin = 0;
  while (begin <= driver_files.size()) {
    const std::size_t end = driver_files.find(':', begin);
    const std::string_view entry = driver_files.substr(
        begin, end == driver_files.npos ? driver_files.npos : end - begin);
    if (!entry.empty()) {
      if (ReadVulkanDriverManifestReach(std::filesystem::path(entry)) !=
          VulkanDriverReach::kOtherVendor) {
        return false;
      }
      named = true;
    }
    if (end == driver_files.npos) {
      break;
    }
    begin = end + 1;
  }
  return named;
}

bool HasNvidiaKernelDriver(const std::filesystem::path& proc_version,
                          const std::filesystem::path& pci_driver) {
  if (access(proc_version.c_str(), R_OK) == 0) {
    return true;
  }

  std::error_code error;
  return std::filesystem::is_directory(pci_driver, error);
}

std::string ParseNvidiaDriverVersion(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
      ++begin;
    }
    std::size_t end = begin;
    while (end < text.size() &&
           std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    const std::string_view token = text.substr(begin, end - begin);
    std::size_t digits = 0;
    while (digits < token.size() &&
           std::isdigit(static_cast<unsigned char>(token[digits])) != 0) {
      ++digits;
    }
    if (digits >= 3 && digits + 1 < token.size() && token[digits] == '.' &&
        std::isdigit(static_cast<unsigned char>(token[digits + 1])) != 0) {
      return std::string(token);
    }
    begin = end;
  }
  return {};
}

int NvidiaDriverMajorVersion(std::string_view version) {
  if (ParseNvidiaDriverVersion(version) != version) {
    return 0;
  }
  int major = 0;
  for (const char digit : version) {
    if (digit == '.') {
      return major;
    }
    major = major * 10 + (digit - '0');
    if (major > 99999) {
      return 0;
    }
  }
  return 0;
}

std::string NvidiaDriverVersion(const std::filesystem::path& proc_version,
                                const std::filesystem::path& module_version) {
  // Only the first line names the kernel module; the next one is the
  // compiler's version.
  std::ifstream proc(proc_version);
  std::string line;
  if (proc && std::getline(proc, line)) {
    std::string version = ParseNvidiaDriverVersion(line);
    if (!version.empty()) {
      return version;
    }
  }
  std::ifstream module(module_version);
  if (module && std::getline(module, line)) {
    return ParseNvidiaDriverVersion(line);
  }
  return {};
}

bool HasAvailableVideoDriverCandidate(
    std::string_view requested,
    const std::vector<std::string_view>& available_drivers) {
  std::size_t begin = 0;
  while (begin <= requested.size()) {
    const std::size_t end = requested.find(',', begin);
    const std::string_view candidate = requested.substr(
        begin, end == requested.npos ? requested.npos : end - begin);
    if (!candidate.empty() &&
        std::find(available_drivers.begin(), available_drivers.end(),
                  candidate) != available_drivers.end()) {
      return true;
    }
    if (end == requested.npos) {
      break;
    }
    begin = end + 1;
  }
  return false;
}

}  // namespace window
}  // namespace mocktail
