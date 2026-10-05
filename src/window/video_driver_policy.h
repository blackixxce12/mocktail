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

// NVIDIA's switch for leaving explicit sync out of its WSI. Its changelog
// (575.51.02): "Extended the __NV_DISABLE_EXPLICIT_SYNC environment
// variable, which was available to EGL applications, to also apply to GLX
// and Vulkan applications."
inline constexpr char kNvidiaDisableExplicitSyncVariable[] =
    "__NV_DISABLE_EXPLICIT_SYNC";

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
  // __NV_DISABLE_EXPLICIT_SYNC takes explicit sync out of NVIDIA's WSI.
  kExplicitSyncDisabled,
  // The game's swapchain may wait for the display (vertical sync), and the
  // compositor is not Hyprland.
  kVsyncOutsideHyprland,
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
  // __NV_DISABLE_EXPLICIT_SYNC is set (NvidiaExplicitSyncDisabled).
  bool nvidia_explicit_sync_disabled = false;
  WaylandExplicitSync wayland_explicit_sync = WaylandExplicitSync::kUnknown;
  // The game's swapchain does not wait for the display: graphics.vsync off,
  // or auto with frame_rate_limit unlimited (present_mode_policy.cc
  // kUnthrottled), for which the Vulkan adapter asks for immediate, else
  // mailbox (FilterPresentModes). Otherwise it presents in step with the
  // display, through FIFO latest ready when the driver offers it (else
  // mailbox, FIFO relaxed, FIFO), or Roblox picks.
  bool unthrottled_presentation = false;
  // The compositor lists Hyprland's own globals (WaylandGlobals::hyprland);
  // known only after the registry probe ran.
  bool hyprland_compositor = false;
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
// WSI), __NV_DISABLE_EXPLICIT_SYNC turns explicit sync off, the computer
// has an Intel or AMD card too (PRIME presentation is untested), no NVIDIA
// card is listed, the compositor could not be asked or does not offer
// wp_linux_drm_syncobj_manager_v1, and last, the swapchain may wait for the
// display on a compositor other than Hyprland. Without explicit sync,
// NVIDIA's Wayland presentation is not reliable; with it, the guard keeps
// SDL's main-thread commits from breaking the present (wl_surface.commit
// between the timeline points and the buffer is a fatal protocol error).
//
// With vertical sync, NVIDIA's Wayland WSI can block the render thread
// while the game window is hidden: Roblox calls vkAcquireNextImageKHR with
// an infinite timeout (UINT64_MAX). In a nested GNOME session with driver
// 615.71.09, a FIFO swapchain sat in vkAcquireNextImageKHR for 8 of the 10
// seconds its window was minimized (and around that, too: 320 presents in
// 15 s), while an immediate one presented throughout, 59790 times while
// minimized (sandbox-bench/tools/syncrace/results/gnome-min.out).
// Immediate and mailbox presentation do not wait for the display, and on
// Hyprland the user's 92 native Wayland sessions with driver 615 (38.7 h,
// commit 275e8f7) ran without a stall, so native Wayland on the automatic
// choice needs one of the two.
NvidiaWaylandBlocker NvidiaNativeWaylandBlocker(
    const VideoDriverPolicyInput& input);

// True when the compositor's globals (explicit sync, and whether it is
// Hyprland) are the last thing the NVIDIA rule needs to know, so the
// registry probe is worth its connection.
bool NeedsWaylandExplicitSyncProbe(const VideoDriverPolicyInput& input);

// Why the automatic choice keeps NVIDIA's direct Vulkan on XWayland, as the
// game window logs it: the setting or condition, in a few words.
// `driver_version` is the NVIDIA driver's version, or empty when unknown.
std::string NvidiaWaylandBlockerReason(NvidiaWaylandBlocker blocker,
                                       std::string_view driver_version);

// __NV_DISABLE_EXPLICIT_SYNC set to anything but 0 (`value` null when
// unset). How NVIDIA reads other values is not documented, so every other
// value counts as turning explicit sync off, which keeps XWayland.
bool NvidiaExplicitSyncDisabled(const char* value);

// Which cards a Vulkan driver reaches, from its manifest's ICD.library_path.
enum class VulkanDriverReach {
  // The manifest cannot be read, names no library, or names one not listed
  // below: it may be NVIDIA's.
  kUnknown,
  // NVIDIA's own driver (libGLX_nvidia.so.0) or Mesa's NVK
  // (libvulkan_nouveau.so), which run NVIDIA cards.
  kNvidia,
  // Mesa's RADV, ANV and HasVK (libvulkan_radeon.so, libvulkan_intel.so,
  // libvulkan_intel_hasvk.so), the drivers Mocktail pins for AMD and Intel
  // cards, or AMD's AMDVLK (amdvlk64.so, amdvlk32.so).
  kOtherVendor,
};

// By the library's file name; directories in front of it do not count.
VulkanDriverReach ClassifyVulkanDriverLibrary(std::string_view library_path);

// Reads ICD.library_path from the driver manifest at `manifest`.
VulkanDriverReach ReadVulkanDriverManifestReach(
    const std::filesystem::path& manifest);

// True when `driver_files` (VK_DRIVER_FILES or VK_ICD_FILENAMES, a
// colon-separated manifest list) names at least one manifest and every one
// reads as kOtherVendor. Empty lists, directories, unreadable manifests and
// unknown drivers count as possibly NVIDIA, as do manifests of NVIDIA's
// driver or NVK, whatever their file names.
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
