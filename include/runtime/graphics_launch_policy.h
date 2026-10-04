#ifndef MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

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
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               const std::vector<std::string>& user_environment,
                               std::string* error = nullptr);

// As above, treating every variable now in the process environment as the
// user's own.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
