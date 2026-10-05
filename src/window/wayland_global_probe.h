#ifndef MOCKTAIL_WINDOW_WAYLAND_GLOBAL_PROBE_H_
#define MOCKTAIL_WINDOW_WAYLAND_GLOBAL_PROBE_H_

#include <chrono>

namespace mocktail {
namespace window {

struct WaylandGlobals {
  // The compositor listed its globals completely (a wl_display.sync after
  // the registry request came back) before the timeout.
  bool listed = false;
  // wp_linux_drm_syncobj_manager_v1: explicit sync, which NVIDIA's WSI uses
  // on driver 555 and newer.
  bool drm_syncobj = false;
  // wp_pointer_warp_v1: without it SDL commits the surface to warp.
  bool pointer_warp = false;
  // A global from Hyprland's own protocols (hyprland_*, hyprland-protocols):
  // the compositor is Hyprland. Of the eight compositors in the sandbox
  // bench (Hyprland, KWin, GNOME, niri, cosmic-comp, Wayfire, river, labwc)
  // only Hyprland lists one; it lists eight, hyprland_surface_manager_v1
  // among them (sandbox-bench/tools/globals, 2026-10-04).
  bool hyprland = false;
};

// libwayland-client.so.0 loaded with everything the probe needs.
bool WaylandClientAvailable();

// Lists the session compositor's globals over a short-lived connection of
// its own, through libwayland-client loaded with dlopen(), before SDL opens
// the game's connection. Never waits longer than `timeout`. Returns an
// unlisted result when the library, WAYLAND_DISPLAY or the compositor is
// unavailable, and leaves WAYLAND_SOCKET (a socket meant for SDL) alone.
WaylandGlobals ProbeWaylandGlobals(std::chrono::milliseconds timeout);

// The same over an already connected socket, which it takes over and closes.
WaylandGlobals ProbeWaylandGlobalsOnSocket(int fd,
                                           std::chrono::milliseconds timeout);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WAYLAND_GLOBAL_PROBE_H_
