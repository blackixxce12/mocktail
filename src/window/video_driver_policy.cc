#include "window/video_driver_policy.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

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
  if (input.other_gpu_count > 0) {
    return NvidiaWaylandBlocker::kOtherGpu;
  }
  if (input.nvidia_gpu_count <= 0) {
    return NvidiaWaylandBlocker::kNoNvidiaGpuListed;
  }
  switch (input.wayland_explicit_sync) {
    case WaylandExplicitSync::kOffered:
      return NvidiaWaylandBlocker::kNone;
    case WaylandExplicitSync::kAbsent:
      return NvidiaWaylandBlocker::kCompositorWithoutExplicitSync;
    case WaylandExplicitSync::kUnknown:
      break;
  }
  return NvidiaWaylandBlocker::kExplicitSyncUnknown;
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

bool VulkanDriverFilesExcludeNvidia(std::string_view driver_files) {
  constexpr std::string_view kManifestSuffix = ".json";
  bool named = false;
  std::size_t begin = 0;
  while (begin <= driver_files.size()) {
    const std::size_t end = driver_files.find(':', begin);
    const std::string_view entry = driver_files.substr(
        begin, end == driver_files.npos ? driver_files.npos : end - begin);
    if (!entry.empty()) {
      const std::size_t slash = entry.rfind('/');
      const std::string_view name =
          slash == entry.npos ? entry : entry.substr(slash + 1);
      if (name.size() <= kManifestSuffix.size() ||
          name.substr(name.size() - kManifestSuffix.size()) !=
              kManifestSuffix ||
          entry.find("nvidia") != entry.npos) {
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
