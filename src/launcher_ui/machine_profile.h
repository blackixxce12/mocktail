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
#include "runtime/graphics_launch_policy.h"
#include "runtime/runtime_config.h"
#include "window/video_driver_policy.h"
#include "window/wayland_global_probe.h"

namespace mocktail::launcher_ui {

// What the settings window knows about this computer, so hints can say what
// a choice means here ("Recommended for this computer"). Detection reads
// files only: it never loads a GPU driver, ANGLE or libgamemode into the
// GTK process. Everything but the monitor is detected off the UI thread at
// start; the monitor comes from the window once it is mapped.

enum class SessionType { kUnknown, kWayland, kX11 };

// PCI vendor IDs (graphics_launch_policy.cc).
inline constexpr unsigned int kIntelPciVendor = 0x8086;
inline constexpr unsigned int kNvidiaPciVendor = 0x10de;
inline constexpr unsigned int kAmdPciVendor = 0x1002;

struct GpuSummary {
  // The Intel, NVIDIA and AMD cards under /sys/class/drm, listed and
  // classified as the game's Vulkan driver choice does
  // (graphics_launch_policy.h DetectHostGpus): NVIDIA cards are discrete,
  // a card on the PCI root bus is integrated (Intel's iGPU, older AMD
  // APUs), and so is an AMD card right behind the root bus's device-8
  // bridge (Zen APUs).
  std::vector<runtime::HostGpu> cards;
  // The vendors among `cards`; `other` is a card of another vendor
  // (virtio, ...), which the game never picks.
  bool nvidia = false;
  bool amd = false;
  bool intel = false;
  bool other = false;
  // /proc/driver/nvidia/version or /sys/bus/pci/drivers/nvidia, as
  // video_driver_policy.cc checks (HasNvidiaKernelDriver): the proprietary
  // or open NVIDIA kernel module, not nouveau.
  bool nvidia_kernel_driver = false;
  // Its version, "615.71.09", from the NVRM line of
  // /proc/driver/nvidia/version, else /sys/module/nvidia/version
  // (video_driver_policy.h NvidiaDriverVersion); empty when unknown.
  std::string nvidia_driver_version;
  // Discrete AMD and Intel cards among `cards`: a Radeon dGPU, an Arc card.
  bool amd_discrete = false;
  bool intel_discrete = false;

  bool any() const { return nvidia || amd || intel || other; }
  // A graphics card with its own memory (every NVIDIA GPU counts).
  bool discrete() const { return nvidia || amd_discrete || intel_discrete; }
  // Only integrated AMD or Intel graphics: an APU, Intel UHD, Iris or Xe.
  bool integrated_only() const { return (amd || intel) && !discrete(); }
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

// Where the Vulkan driver the game would use comes from. The same for every
// engine.gpu value: SelectHostGpu falls back to any card with a driver.
enum class VulkanDriverSource {
  // No hardware driver manifest anywhere the Vulkan loader looks.
  kNone,
  // None of the manifests Mocktail recognizes, inside Flatpak: the
  // runtime's GL extensions hold the drivers, out of the launcher's sight.
  kUnknown,
  // The manifests Mocktail pins for the card it picks
  // (graphics_launch_policy.cc ApplyVulkanIcdPolicy, SelectHostGpu).
  kPinned,
  // VK_DRIVER_FILES or VK_ICD_FILENAMES, which Mocktail keeps as they are.
  kUser,
  // Mocktail pins nothing, and the loader finds a hardware driver itself
  // (AMDVLK's amd_icd64.json, an unknown GPU's driver, ...).
  kLoader,
};

// The Vulkan driver the game would use with one engine.gpu value.
struct VulkanDriverSelection {
  // The manifests Mocktail pins (every one of the card's vendor,
  // colon-separated), the user's list, or the manifest the loader finds
  // itself; empty for kNone and kUnknown.
  std::string icd;
  // The card the game renders on: the one Mocktail picks, or the one
  // VK_LOADER_DEVICE_SELECT names beside the user's list; empty otherwise.
  std::optional<runtime::HostGpu> gpu;
  // `gpu` is of the kind engine.gpu asks for. False when the computer has
  // no card of that kind with a Vulkan driver and another one stands in.
  bool preferred = false;

  // The manifests' file names, comma-separated: "nvidia_icd.json".
  std::string FileNames() const;
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
  // MOCKTAIL_WAYLAND_COMMIT_GUARD does not turn the surface-commit guard
  // off (wayland_surface_commit_guard.h SurfaceCommitGuardAllowed).
  bool surface_commit_guard = true;
  // Whether the compositor offers wp_linux_drm_syncobj_manager_v1 (explicit
  // sync). Asked, as the game asks it, only when the answer is the last
  // thing the NVIDIA rule needs (NeedsWaylandExplicitSyncProbe); unknown
  // otherwise and when the probe fails.
  window::WaylandExplicitSync wayland_explicit_sync =
      window::WaylandExplicitSync::kUnknown;
  bool flatpak = false;

  int physical_cores = 0;
  int logical_cpus = 0;
  std::uint64_t memory_bytes = 0;

  // libgamemode.so.0 is installed (the game dlmopen()s it, game_mode.cc).
  bool gamemode_library = false;
  // The ANGLE pair the game would load: MOCKTAIL_ANGLE_LIB_DIR as it is
  // set, else the first pair in window.cc's search order that looks like
  // ANGLE (the game skips pairs that do not load).
  std::optional<AngleLibraries> angle;
  VulkanDriverSource vulkan_source = VulkanDriverSource::kNone;
  // What graphics_launch_policy.cc ApplyVulkanIcdPolicy works from, so
  // VulkanDriver() can answer for any engine.gpu value: the user's
  // VK_DRIVER_FILES (else VK_ICD_FILENAMES), VK_LOADER_DEVICE_SELECT, what
  // engine.gpu: auto means through DRI_PRIME and __NV_PRIME_RENDER_OFFLOAD,
  // the card SelectHostGpu picks for each kind, and the manifest the loader
  // finds when Mocktail pins none.
  std::string user_vulkan_drivers;
  std::string loader_device_select;
  runtime::GpuPreference automatic_gpu = runtime::GpuPreference::kDiscrete;
  runtime::HostGpuSelection discrete_selection;
  runtime::HostGpuSelection integrated_selection;
  std::string loader_icd;

  MonitorInfo monitor;

  bool has_vulkan_driver() const {
    return vulkan_source == VulkanDriverSource::kPinned ||
           vulkan_source == VulkanDriverSource::kUser ||
           vulkan_source == VulkanDriverSource::kLoader;
  }
  // engine.gpu as graphics_launch_policy.cc applies `gpu_preference` (a
  // config.yaml value): discrete or integrated, auto resolved through
  // DRI_PRIME and __NV_PRIME_RENDER_OFFLOAD. Anything else reads as auto;
  // the loader refuses it before the game starts.
  runtime::GpuPreference ResolvedGpuPreference(
      std::string_view gpu_preference) const;
  // The Vulkan driver and card for direct Vulkan with `gpu_preference`.
  VulkanDriverSelection VulkanDriver(std::string_view gpu_preference) const;
  // Direct Vulkan with `gpu_preference` renders on Intel integrated
  // graphics, which then get graphics quality level 1 by default
  // (graphics_launch_policy.h RendersOnIntelIntegratedGraphics).
  bool RendersOnIntelIntegratedGraphics(std::string_view gpu_preference) const;
  // What the game window's video driver policy reads with `backend` and
  // `gpu_preference` (config.yaml graphics.backend and engine.gpu values),
  // as window.cc ResolveConfiguredVideoDriverChoice gathers it, before the
  // user's SDL_VIDEODRIVER or display.server, which the Display page
  // handles itself.
  window::VideoDriverPolicyInput VideoDriverInput(
      std::string_view backend, std::string_view gpu_preference) const;
  // What display.server: auto picks for the game window: "wayland" or
  // "x11", or empty when this session has neither. Mirrors
  // ResolveVideoDriverChoice.
  std::string AutomaticDisplayServer(std::string_view backend,
                                     std::string_view gpu_preference) const;
  // display.server: auto is the NVIDIA rule's to decide: direct Vulkan on
  // the NVIDIA card with Wayland and XWayland both there
  // (NvidiaDirectVulkanRuleApplies).
  bool NvidiaRuleApplies(std::string_view backend,
                         std::string_view gpu_preference) const;
  // What keeps NVIDIA's direct Vulkan off native Wayland here
  // (NvidiaNativeWaylandBlocker), kNone when nothing does. With
  // `wayland_chosen`, display.server: wayland asks for Wayland, so the
  // Wayland preference does not count.
  window::NvidiaWaylandBlocker NvidiaNativeWaylandBlocker(
      bool wayland_chosen = false) const;
  // The NVIDIA rule keeps direct Vulkan on XWayland under display.server:
  // auto.
  bool NvidiaDirectVulkanUsesX11(std::string_view gpu_preference) const;
  // Vendor names, discrete first: "NVIDIA", "AMD + Intel", "" if unknown.
  std::string GpuVendorsLabel() const;
  // The vendors of the discrete cards only: "NVIDIA", "Intel Arc".
  std::string DiscreteGpuLabel() const;
};

// "NVIDIA", "AMD" or "Intel" ("Intel Arc" for a discrete card), as the
// Graphics page and the About page name a card; empty for another vendor.
std::string GpuCardName(const runtime::HostGpu& card);

// Where detection looks; tests point everything at a fake tree.
struct MachineProbe {
  // Prefixed to /sys, /proc and every directory below.
  std::filesystem::path root = "/";
  // Where Mocktail looks for the manifest it pins (kIcdDirectories). The
  // loader's own search (XDG_CONFIG_DIRS, XDG_DATA_DIRS, ...) is derived
  // from the environment.
  std::vector<std::filesystem::path> icd_directories;
  std::vector<std::filesystem::path> library_directories;
  // Physical cores; defaults to runtime::DetectAvailablePhysicalCoreCount.
  std::function<int()> physical_cores;
  std::function<int()> logical_cpus;
  // Lists the compositor's globals (window.cc's registry probe); unset, the
  // answer stays unknown.
  std::function<window::WaylandGlobals()> wayland_globals;
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
