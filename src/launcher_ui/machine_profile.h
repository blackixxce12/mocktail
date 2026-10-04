#ifndef MOCKTAIL_LAUNCHER_UI_MACHINE_PROFILE_H_
#define MOCKTAIL_LAUNCHER_UI_MACHINE_PROFILE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"

namespace mocktail::launcher_ui {

// What the settings window knows about this computer, so hints can say what
// a choice means here ("Recommended for this computer"). Detection reads
// files only: it never loads a GPU driver, ANGLE or libgamemode into the
// GTK process. Everything but the monitor is detected off the UI thread at
// start; the monitor comes from the window once it is mapped.

enum class SessionType { kUnknown, kWayland, kX11 };

struct GpuSummary {
  // PCI vendors of /sys/class/drm/card0..15/device/vendor, the same scan
  // graphics_launch_policy.cc makes (DetectHostGpus).
  bool nvidia = false;
  bool amd = false;
  bool intel = false;
  bool other = false;
  // /proc/driver/nvidia/version or /sys/bus/pci/drivers/nvidia, as
  // video_driver_policy.cc checks (HasNvidiaKernelDriver): the proprietary
  // or open NVIDIA kernel module, not nouveau.
  bool nvidia_kernel_driver = false;

  bool any() const { return nvidia || amd || intel || other; }
  // Only an Intel GPU: Mocktail then lowers Roblox's graphics quality by
  // default (graphics_launch_policy.cc, MOCKTAIL_GRAPHICS_QUALITY=1).
  bool intel_only() const { return intel && !nvidia && !amd; }
};

struct MonitorInfo {
  bool valid = false;
  std::string connector;    // "eDP-1", "WAYLAND-1", "Virtual-0", ...
  std::string description;  // the monitor's model, may be empty
  // Logical size in desktop units (what window.width/height mean on
  // Wayland) and the surface's fractional scale.
  int width = 0;
  int height = 0;
  double scale = 1.0;
  int refresh_millihertz = 0;

  // Rounded refresh rate; 0 when unknown.
  int RefreshHz() const;
  // Pixels at the monitor's scale (a 1600x900 monitor at 1.6 is 2560x1440).
  int PixelWidth() const;
  int PixelHeight() const;
};

struct AngleLibraries {
  // Directory holding libEGL.so and libGLESv2.so.
  std::filesystem::path directory;
  // "Electron 43", "Chromium", "Google Chrome", "CEF", or the directory.
  std::string label;
  // Found through MOCKTAIL_RUNTIME_LIBRARY_DIR/angle (shipped with
  // Mocktail) or MOCKTAIL_ANGLE_LIB_DIR (chosen by the user).
  bool bundled = false;
  bool from_environment = false;
  // libGLESv2.so names the EGL_ANGLE_platform_angle extension. The game
  // still checks the pair properly (graphics/angle_probe.cc) before use.
  bool looks_like_angle = false;
};

struct MachineProfile {
  // False until the background detection finished.
  bool detected = false;

  GpuSummary gpu;
  SessionType session = SessionType::kUnknown;
  // "Hyprland", "KDE Plasma", "GNOME", "COSMIC", "niri", "Sway", ...;
  // empty when unknown.
  std::string desktop;
  // A Wayland session the game can use: WAYLAND_DISPLAY and
  // XDG_RUNTIME_DIR (AvailableDisplayServer's test).
  bool wayland_available = false;
  // An X server: DISPLAY (XWayland on a Wayland desktop).
  bool x11_available = false;
  // MOCKTAIL_PREFER_WAYLAND is not 0/false (window.cc).
  bool prefer_wayland = true;
  bool flatpak = false;

  int physical_cores = 0;
  int logical_cpus = 0;
  std::uint64_t memory_bytes = 0;

  // libgamemode.so.0 is installed (the game dlmopen()s it, game_mode.cc).
  bool gamemode_library = false;
  // The first ANGLE pair in the order window.cc searches.
  std::optional<AngleLibraries> angle;
  // The Vulkan driver manifest the game would pin for this GPU, or any
  // hardware driver manifest when the GPU vendor is unknown.
  std::string vulkan_icd;

  MonitorInfo monitor;

  bool has_vulkan_driver() const { return !vulkan_icd.empty(); }
  // What display.server: auto picks for the game window with `backend`
  // (a config.yaml graphics.backend value): "wayland" or "x11", or empty
  // when this session has neither. Mirrors ResolveVideoDriverChoice.
  std::string AutomaticDisplayServer(std::string_view backend) const;
  // display.server: auto runs direct Vulkan through XWayland because of
  // the NVIDIA kernel driver (video_driver_policy.h).
  bool NvidiaDirectVulkanUsesX11() const;
  // Vendor names, discrete first: "NVIDIA", "AMD + Intel", "" if unknown.
  std::string GpuVendorsLabel() const;
};

// Where detection looks; tests point everything at a fake tree.
struct MachineProbe {
  // Prefixed to /sys, /proc and every directory below.
  std::filesystem::path root = "/";
  std::vector<std::filesystem::path> icd_directories;
  std::vector<std::filesystem::path> library_directories;
  // Physical cores; defaults to runtime::DetectAvailablePhysicalCoreCount.
  std::function<int()> physical_cores;
  std::function<int()> logical_cpus;
};

MachineProbe DefaultMachineProbe();

MachineProfile DetectMachineProfile(const runtime::Environment& environment,
                                    const MachineProbe& probe);

// The directories the game searches for an ANGLE pair, in its order
// (window.cc FindInstalledAngleLibraries; MOCKTAIL_ANGLE_LIB_DIR first, as
// ResolveGraphicsLibraries uses it before the search).
std::vector<std::filesystem::path> AngleSearchDirectories(
    const runtime::Environment& environment);

// "Electron 43" for /usr/lib/electron43 and so on; the path otherwise.
std::string AngleLibraryLabel(const std::filesystem::path& directory);

// The desktop's name from XDG_CURRENT_DESKTOP / XDG_SESSION_DESKTOP and
// compositor-specific variables; empty when unknown.
std::string DescribeDesktop(const runtime::Environment& environment);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_MACHINE_PROFILE_H_
