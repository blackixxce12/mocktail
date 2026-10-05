#ifndef MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_
#define MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/fast_flags_document.h"
#include "launcher_ui/machine_profile.h"

namespace mocktail::launcher_ui {

// Machine-aware recommendations, kept free of GTK and of wording so they
// are unit-tested; the pages turn the reason into a translated sentence.

enum class BackendRecommendationReason {
  // A Vulkan driver for this GPU is installed: direct Vulkan.
  kVulkanDriver,
  // No hardware Vulkan driver was found anywhere the loader looks: OpenGL
  // ES works with any OpenGL ES 3.0 driver.
  kNoVulkanDriver,
  // Detection has not finished, or the drivers are out of sight (Flatpak):
  // no recommendation.
  kUnknown,
};

struct BackendRecommendation {
  // A canonical graphics.backend value: direct-vulkan or opengl.
  std::string value;
  BackendRecommendationReason reason = BackendRecommendationReason::kUnknown;
};

BackendRecommendation RecommendGraphicsBackend(const MachineProfile& machine);

// ---- Graphics, Display, Performance and Audio pages ------------------------
// Values are config.yaml spellings; an empty value means the key is absent
// and the runtime's default applies.

// Mocktail's rendering preset (performance_policy.cc
// MergePerformanceClientSettingsOverrides) is merged when physics workers
// are "throughput" (the runtime default), or "auto" with multithreaded
// rendering on; "latency" never merges it. The preset also forces Roblox's
// graphics quality level (FIntDebugFRMQualityLevelOverride).
bool RenderingPresetActive(std::string_view physics_worker_mode,
                           std::string_view multithreaded_rendering);

// engine.graphics_quality: 1..21, nullopt for default/manual/anything else.
std::optional<int> ParseQualityLevel(std::string_view value);

enum class QualitySource {
  // Nothing is forced: Roblox's own graphics setting decides.
  kRobloxSetting,
  // "default" with the preset active: level 3, or 1 when direct Vulkan
  // renders on Intel integrated graphics (graphics_launch_policy.cc).
  kMocktailDefault,
  // A level from config.yaml with the preset active.
  kConfiguredLevel,
};

struct QualityEffect {
  QualitySource source = QualitySource::kRobloxSetting;
  int level = 0;  // the forced level; 0 for kRobloxSetting
  // A level is configured (or default) but the preset is off, so it does
  // nothing.
  bool ignored = false;
};

// `intel_integrated_vulkan`: direct Vulkan renders on Intel integrated
// graphics (MachineProfile::RendersOnIntelIntegratedGraphics).
QualityEffect ResolveGraphicsQuality(std::string_view graphics_quality,
                                     bool preset_active,
                                     bool intel_integrated_vulkan);

enum class QualityRecommendationReason {
  // A discrete graphics card (GpuSummary::discrete): Roblox's own setting
  // can go above level 3.
  kCapableGraphics,
  // Only integrated AMD or Intel graphics: Mocktail's low default.
  kModestGraphics,
  // Detection has not finished, or no known GPU was found: no
  // recommendation.
  kUnknown,
};

struct QualityRecommendation {
  std::string value;  // "manual", "default", or empty for kUnknown
  QualityRecommendationReason reason = QualityRecommendationReason::kUnknown;
};

QualityRecommendation RecommendGraphicsQuality(const MachineProfile& machine);

// How the Vulkan swapchain presents frames: present_mode_policy.cc
// ResolvePresentModePolicy for config.yaml values (vsync auto, on, off;
// frame_rate_limit -1, display, unlimited or a number).
enum class Presentation {
  kDriverDefault,  // auto with Roblox owning the cap (-1)
  kSynchronized,   // on, or auto with display / a fixed target
  kUnthrottled,    // off, or auto with unlimited
};
Presentation ResolvePresentation(std::string_view vsync,
                                 std::string_view frame_rate_limit);

// Fixed frame-rate choices besides "Set in Roblox", the display's own rate
// and the maximum (frame_rate_policy.h kMaximumSupportedRobloxSchedulerFps).
inline constexpr int kFrameRateChoices[] = {60, 120, 144};

// graphics.frame_rate_limit as a positive integer, nullopt otherwise.
std::optional<int> ParseFrameRate(std::string_view value);

// The display's refresh rate when it is above 60 Hz (each refresh then
// shows a new frame), "unlimited" (240, the highest rate Roblox's own menu
// offers) above 240 Hz, "-1" (Roblox's own menu) for a 60 Hz or slower
// display. nullopt while the monitor is unknown, and above 60 Hz with only
// integrated graphics, which rarely keep up with a fast screen.
std::optional<std::string> RecommendFrameRate(const MachineProfile& machine);

// The fflags.json entry that makes Mocktail refuse to start with this
// frame rate (frame_rate_policy.cc rejects a different explicit value of a
// flag it sets), empty when there is none.
std::string FrameRateFlagConflict(const launcher::FastFlagsDocument& flags,
                                  std::string_view frame_rate_limit);
// The same for the quality level the preset forces.
std::string QualityFlagConflict(const launcher::FastFlagsDocument& flags,
                                const QualityEffect& quality);

struct WindowSize {
  int width = 0;
  int height = 0;
  bool operator==(const WindowSize& other) const {
    return width == other.width && height == other.height;
  }
  bool operator!=(const WindowSize& other) const { return !(*this == other); }
};

// window.width/height defaults (runtime_config.h) and the limits the
// remembered window state accepts (window_state_store.h/.cc).
inline constexpr WindowSize kDefaultWindowSize = {1280, 720};
inline constexpr int kMinimumWindowWidth = 160;
inline constexpr int kMinimumWindowHeight = 120;
inline constexpr int kMaximumWindowExtent = 16384;

struct WindowSizePreset {
  WindowSize size;
  bool whole_screen = false;
  bool runtime_default = false;
};

// Window sizes to offer, largest first: the monitor's whole logical size,
// then the common 16:9 sizes that fit inside it (at most four), and always
// Mocktail's 1280 x 720 default. Without a monitor: common sizes.
std::vector<WindowSizePreset> WindowSizePresets(const MonitorInfo& monitor);

enum class WindowMode { kWindowed, kMaximized, kFullscreen };

// The window mode the game starts in: display.start_mode, where
// "remember" (and an absent key) uses the remembered state's flags.
WindowMode StartWindowMode(std::string_view start_mode, bool remembered_found,
                           bool remembered_fullscreen,
                           bool remembered_maximized);

struct GameResolution {
  // Window size in desktop units (the whole monitor for fullscreen and
  // maximized).
  WindowSize logical;
  // What Roblox renders: logical x scale with High-DPI on Wayland, else
  // the logical size.
  WindowSize pixels;
  // The compositor scales the picture up (scale > 1 without High-DPI).
  bool upscaled = false;
  // High-DPI changes something here (Wayland and a scale above 1).
  bool high_dpi_matters = false;
  // Maximized: the screen minus panels, which the launcher cannot see.
  bool approximate = false;
};

// SDL 3 on Wayland renders HIGH_PIXEL_DENSITY windows at the output scale
// and others at 1.0 (research/graphics.md 2.2); X11 gets no density.
GameResolution ComputeGameResolution(const MonitorInfo& monitor,
                                     WindowSize windowed, WindowMode mode,
                                     bool high_dpi, bool wayland);

// The High-DPI recommendation: on for a scaled Wayland screen, off with
// only integrated graphics there (it renders scale^2 as many pixels),
// nullopt when it changes nothing (scale 1, X11, unknown monitor).
std::optional<std::string> RecommendHighDpi(const MachineProfile& machine,
                                            bool wayland);

enum class DisplayServerReason {
  // display.server names a server this session has.
  kChosen,
  // It names one this session lacks; the runtime falls back to automatic
  // (graphics_launch_policy.cc AvailableDisplayServer).
  kChosenUnavailable,
  // Automatic: Wayland, the session's own.
  kWaylandSession,
  // Automatic, decided by the NVIDIA rule (direct Vulkan on the NVIDIA
  // card, both servers present; video_driver_policy.h): native Wayland,
  // as nothing in NvidiaNativeWaylandBlocker stands against it,
  kNvidiaVulkanWayland,
  // or XWayland, as something does.
  kNvidiaVulkanX11,
  // Automatic: only an X server.
  kX11Only,
  // Detection has not finished, or there is no display at all.
  kUnknown,
};

struct DisplayServerChoice {
  std::string server;  // "wayland", "x11", or empty when unknown
  DisplayServerReason reason = DisplayServerReason::kUnknown;
};

// `configured`, `backend` and `gpu_preference` are the config.yaml values
// of display.server, graphics.backend and engine.gpu; `presentation` is
// ResolvePresentation of graphics.vsync and frame_rate_limit, which the
// NVIDIA rule reads too.
DisplayServerChoice ResolveDisplayServer(const MachineProfile& machine,
                                         std::string_view configured,
                                         std::string_view backend,
                                         std::string_view gpu_preference,
                                         Presentation presentation);

// Mocktail's suggestion when the memory limit is switched on: 3/16 of the
// RAM (6 GiB of 32, as config/mocktail.example.yaml suggests), at least
// 4 GiB but never more than 3/4 of the RAM, in whole 512 MiB steps.
std::uint64_t SuggestedMemoryLimitMiB(std::uint64_t memory_bytes);

enum class AudioDeviceState {
  kDefault,    // "default" or absent
  kDisabled,   // "disabled" (microphone only)
  kConnected,  // exactly one device has this name
  kAmbiguous,  // several devices have this name: Roblox refuses it
  kMissing,    // no device has this name now: Roblox refuses to start
  kNumericId,  // "id:N" (microphone only): numbers change between starts
  kNotListed,  // devices are not enumerated (yet)
};

// What an audio.output_device / audio.input_device value means against the
// SDL device names (sdl_audio_sink.cc / sdl_audio_capture.cc resolve names
// exactly). `devices` is nullptr while enumeration has not finished.
AudioDeviceState ClassifyAudioDevice(std::string_view value,
                                     const std::vector<std::string>* devices,
                                     bool input);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_
