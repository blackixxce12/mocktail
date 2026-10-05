#ifndef MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_
#define MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace window {

enum class VideoDriverChoice {
  kSdlDefault,
  kWayland,
  kX11,
  kNvidiaDirectVulkanX11,
  kNvidiaDirectVulkanWayland,
};

// NVIDIA added explicit sync to its Vulkan WSI in driver 555.
inline constexpr int kNvidiaExplicitSyncDriverMajor = 555;

// Whether the compositor offers wp_linux_drm_syncobj_manager_v1.
enum class WaylandExplicitSync {
  kUnknown,  // not asked yet, or the registry probe failed
  kAbsent,
  kOffered,
};

// Why the automatic choice keeps NVIDIA's direct Vulkan on XWayland.
enum class NvidiaWaylandBlocker {
  kNone,
  kWaylandNotPreferred,
  kCommitGuardOff,
  kDriverVersionUnknown,
  kDriverWithoutExplicitSync,
  kOtherGpu,
  kNoNvidiaGpuListed,
  kExplicitSyncUnknown,
  kCompositorWithoutExplicitSync,
};

struct VideoDriverPolicyInput {
  bool has_explicit_sdl_driver = false;
  bool force_wayland = false;
  bool force_x11 = false;
  bool prefer_wayland = true;
  bool has_wayland_session = false;
  bool has_x11_display = false;
  bool uses_direct_vulkan = false;
  bool has_nvidia_kernel_driver = false;
  // The pinned Vulkan drivers cannot reach an NVIDIA card, as when
  // engine.gpu puts the game on the integrated GPU of a hybrid laptop.
  bool vulkan_drivers_exclude_nvidia = false;

  // Evidence for running NVIDIA's direct Vulkan on native Wayland. Only
  // read while the NVIDIA rule applies (NvidiaDirectVulkanRuleApplies).
  // Major version of the NVIDIA kernel driver; 0 when unknown.
  int nvidia_driver_major = 0;
  // Graphics cards under /sys/class/drm: NVIDIA ones, and Intel or AMD ones.
  int nvidia_gpu_count = 0;
  int other_gpu_count = 0;
  // The game window serializes SDL's surface commits with the host present
  // (SurfaceCommitGuard), which explicit sync on Wayland needs.
  bool surface_commit_guard = false;
  WaylandExplicitSync wayland_explicit_sync = WaylandExplicitSync::kUnknown;
};

// Resolves the SDL video backend before SDL_Init. An explicit SDL driver is
// always authoritative, then MOCKTAIL_FORCE_WAYLAND and MOCKTAIL_FORCE_X11
// (display.server). NVIDIA's direct Vulkan WSI on a Wayland session that also
// has XWayland gets the NVIDIA rule: native Wayland
// (kNvidiaDirectVulkanWayland) only when NvidiaNativeWaylandBlocker() finds
// nothing against it, X11/XWayland (kNvidiaDirectVulkanX11) otherwise. A
// loaded NVIDIA kernel driver does not count while the pinned Vulkan drivers
// exclude NVIDIA: the game then presents through another vendor's WSI.
VideoDriverChoice ResolveVideoDriverChoice(const VideoDriverPolicyInput& input);

// True when the automatic choice is NVIDIA's to make: direct Vulkan on an
// NVIDIA card, a Wayland session with XWayland, and nothing explicit.
bool NvidiaDirectVulkanRuleApplies(const VideoDriverPolicyInput& input);

// What keeps NVIDIA's direct Vulkan off native Wayland, checked in this
// order: the Wayland preference is off, the surface-commit guard is off, the
// driver version is unknown or older than 555 (no explicit sync in its
// WSI), the computer has an Intel or AMD card too (PRIME presentation is
// untested), no NVIDIA card is listed, the compositor could not be asked or
// does not offer wp_linux_drm_syncobj_manager_v1. Without explicit sync,
// NVIDIA's Wayland presentation is not reliable; with it, the guard keeps
// SDL's main-thread commits from breaking the present (wl_surface.commit
// between the timeline points and the buffer is a fatal protocol error).
NvidiaWaylandBlocker NvidiaNativeWaylandBlocker(
    const VideoDriverPolicyInput& input);

// True when the compositor's globals are the last thing the NVIDIA rule
// needs to know, so the registry probe is worth its connection.
bool NeedsWaylandExplicitSyncProbe(const VideoDriverPolicyInput& input);

// True when `driver_files` (VK_DRIVER_FILES or VK_ICD_FILENAMES, a
// colon-separated manifest list) names only .json manifests whose paths do
// not mention nvidia. Empty lists, directories and anything else that does
// not say which driver it is count as possibly NVIDIA.
bool VulkanDriverFilesExcludeNvidia(std::string_view driver_files);

const char* VideoDriverChoiceName(VideoDriverChoice choice);

// Also detects sandboxed drivers through sysfs when procfs is unavailable.
bool HasNvidiaKernelDriver(
    const std::filesystem::path& proc_version = "/proc/driver/nvidia/version",
    const std::filesystem::path& pci_driver = "/sys/bus/pci/drivers/nvidia");

// The loaded NVIDIA kernel driver's version ("615.71.09"), from the
// NVRM line of /proc/driver/nvidia/version, else /sys/module/nvidia/version
// (which Flatpak hides); empty when neither says.
std::string NvidiaDriverVersion(
    const std::filesystem::path& proc_version = "/proc/driver/nvidia/version",
    const std::filesystem::path& module_version =
        "/sys/module/nvidia/version");

// The first whitespace-separated token of `text` that looks like an NVIDIA
// driver version (three or more digits, a dot, a digit), or empty.
std::string ParseNvidiaDriverVersion(std::string_view text);

// 615 for "615.71.09"; 0 for anything that is not such a version.
int NvidiaDriverMajorVersion(std::string_view version);

// SDL accepts a comma-separated priority list. An inherited override is only
// useful when at least one requested driver exists in the linked SDL build.
bool HasAvailableVideoDriverCandidate(
    std::string_view requested,
    const std::vector<std::string_view>& available_drivers);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_
