#ifndef MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

// An Intel, NVIDIA or AMD graphics card from /sys/class/drm.
struct HostGpu {
  unsigned int vendor = 0;  // PCI vendor ID
  unsigned int device = 0;  // PCI device ID, 0 when sysfs does not say
  std::string pci_address;  // 0000:c4:00.0, empty when sysfs does not say
  bool integrated = false;
};

// The cards under `drm_class_directory` in card-number order. NVIDIA cards
// are discrete. An Intel or AMD card on the PCI root bus is integrated
// (every Intel iGPU, at 00:02.0, and the pre-Zen AMD APUs), and so is an AMD
// card right behind the root bus's device-8 bridge, where Zen APUs put their
// graphics. Every other card is discrete. Without a PCI path, as on a fake
// or partial sysfs, Intel counts as integrated and AMD as discrete.
std::vector<HostGpu> DetectHostGpus(
    const std::filesystem::path& drm_class_directory = "/sys/class/drm");

// engine.gpu as the Vulkan driver selection applies it. discrete and
// integrated stand; auto becomes integrated when DRI_PRIME or
// __NV_PRIME_RENDER_OFFLOAD is 0, off or igpu, and discrete otherwise.
GpuPreference ResolveGpuPreference(GpuPreference configured,
                                   std::string_view dri_prime,
                                   std::string_view nv_prime_render_offload);

struct HostGpuSelection {
  // The card direct Vulkan renders on; empty when no card has a Vulkan
  // driver in `icd_directories`.
  std::optional<HostGpu> gpu;
  // VK_DRIVER_FILES for it: every installed manifest of its vendor,
  // colon-separated.
  std::string icd;
  // The card is of the preferred kind. False when the computer has none,
  // or none with a driver, and another card stands in.
  bool preferred = false;
  // VK_LOADER_DEVICE_SELECT (0xVVVV:0xDDDD) when another card shares the
  // vendor's drivers, as with an AMD APU and an AMD dGPU; empty otherwise.
  std::string loader_device_select;
};

// Picks the card for `preference` (auto counts as discrete): a card of the
// preferred kind with a Vulkan driver, NVIDIA before AMD before Intel among
// discrete cards and Intel before AMD among integrated ones, and any other
// card with a driver when there is none. A single card is always chosen.
HostGpuSelection SelectHostGpu(
    const std::vector<HostGpu>& gpus, GpuPreference preference,
    const std::vector<std::filesystem::path>& icd_directories);

// What the session log says about `selection` for engine.gpu `configured`:
// a "  [runtime] vulkan GPU=" line naming the card and the drivers pinned
// for it, then, when engine.gpu names a kind that no card with a Vulkan
// driver is of, a line saying so. Empty without a card.
std::string DescribeVulkanGpuSelection(const HostGpuSelection& selection,
                                       GpuPreference configured);

// Whether direct Vulkan runs on Intel integrated graphics, which get
// graphics quality level 1 while engine.graphics_quality is default.
// `selected` is SelectHostGpu's card, or the card VK_LOADER_DEVICE_SELECT
// names while the Vulkan drivers are already pinned; without one only an
// Intel-only computer counts.
bool RendersOnIntelIntegratedGraphics(const std::vector<HostGpu>& gpus,
                                      const std::optional<HostGpu>& selected);

// Readable ICD manifest for `vendor` that this build can actually load, or an
// empty path. Directories are searched in order; a manifest built for another
// architecture is never selected.
std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor);

// Variables with which a user picks the SDL video driver directly. While one
// of them came from the user's own environment, display.server is not
// applied: the user's choice wins.
inline constexpr std::string_view kUserVideoDriverVariables[] = {
    "SDL_VIDEODRIVER",        "SDL_VIDEO_DRIVER",
    "MOCKTAIL_FORCE_WAYLAND", "MOCKTAIL_FORCE_X11",
    "MOCKTAIL_ANGLE_FORCE_X11",
};

// True when one of kUserVideoDriverVariables is in `user_environment` (from
// CaptureUserManagedEnvironment) and still set in the process environment.
// A variable the user set but Mocktail has since removed, as when the
// settings window is told to ignore the environment, no longer counts.
bool UserSelectsVideoDriver(const std::vector<std::string>& user_environment);

// engine.nvidia_shader_mt as Roblox client settings for direct Vulkan. True
// adds nothing, so Roblox's own FStringGraphicsVulkanShaderMTDenyPattern
// decides (the value Roblox serves today names only some Imagination PowerVR
// drivers). False adds the NVIDIA deny, 4318:.*, so Roblox loads its shader
// pack on one thread there, unless `base_json` already sets that flag.
// `base_json` must be a JSON object; empty means {}.
bool MergeNvidiaShaderLoadingClientSettingsOverrides(
    bool nvidia_shader_mt, std::string_view base_json,
    std::string* merged_json, std::string* error = nullptr);

// The display server ApplyGraphicsLaunchPolicy publishes for `configured`.
// Wayland needs a Wayland session (WAYLAND_DISPLAY and XDG_RUNTIME_DIR) and
// x11 an X display (DISPLAY), the same tests the window policy makes;
// without one the setting falls back to auto, so a value saved in one
// session cannot keep the game from starting in another.
DisplayServer AvailableDisplayServer(DisplayServer configured);

// Publishes the resolved graphics backend before the managed payload updater
// starts. OpenGL is a strict system EGL/GLES path; it never silently retries
// through ANGLE/Vulkan or accepts a window without a real graphics context.
//
// display.server is published here too, so the updater's canaries, which
// inherit only the video-driver variables, open their windows the same way:
// wayland sets MOCKTAIL_FORCE_WAYLAND=1 and clears both X11 switches, x11
// sets MOCKTAIL_FORCE_X11=1 and clears MOCKTAIL_FORCE_WAYLAND, and auto
// (or a server this session lacks, see AvailableDisplayServer) leaves the
// window policy alone. Nothing is touched while the user selects the video
// driver (see UserSelectsVideoDriver).
//
// MOCKTAIL_NVIDIA_SHADER_MT is published for the canaries as well. With
// direct Vulkan and engine.nvidia_shader_mt false, the NVIDIA shader loading
// deny is merged into MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON (see
// MergeNvidiaShaderLoadingClientSettingsOverrides).
//
// The policy runs before the session log starts, so what it has to say for
// the log (DescribeVulkanGpuSelection) is appended to `*log` for the caller
// to print once the log runs; without `log` it goes to stderr at once.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               const std::vector<std::string>& user_environment,
                               std::string* error = nullptr,
                               std::string* log = nullptr);

// As above, treating every variable now in the process environment as the
// user's own.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
