#ifndef MOCKTAIL_LAUNCHER_UI_ROBLOX_OVERRIDES_H_
#define MOCKTAIL_LAUNCHER_UI_ROBLOX_OVERRIDES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher_ui/machine_profile.h"

namespace mocktail::launcher_ui {

// Which of Roblox's own settings Mocktail decides with the current
// config.yaml, and which stay with Roblox, kept free of GTK and of wording
// so they are unit-tested; roblox_decides.cc says it on the rows' badges and
// in the Advanced page's overview. Every rule names the runtime code it
// follows.

// The config.yaml values the overrides depend on, as the game will read
// them. An empty value means the key is absent and the runtime's default
// applies.
struct GameSettings {
  std::string graphics_quality;         // engine.graphics_quality
  std::string physics_worker_mode;      // performance.physics_worker_mode
  std::string multithreaded_rendering;  // performance.multithreaded_rendering
  std::string frame_rate_limit;         // graphics.frame_rate_limit
  std::string theme;                    // appearance.theme
  std::string start_mode;               // display.start_mode
  std::string input_device;             // audio.input_device
  // device, or device.type of a detailed device: block.
  std::string device;
  // Direct Vulkan renders on Intel integrated graphics, which get level 1
  // by default (MachineProfile::RendersOnIntelIntegratedGraphics).
  bool intel_integrated_vulkan = false;
};

enum class RobloxOverrideKind {
  // performance_policy.cc FIntDebugFRMQualityLevelOverride: Roblox's
  // Graphics Quality slider has no effect.
  kGraphicsQuality,
  // The rest of the rendering preset (performance_policy.cc
  // rendering_settings): MSAA off (FIntDebugForceMSAASamples=1), a 128 MB
  // texture budget (FIntRenderTextureTotalBudgetMB), a texture mip bias,
  // low-end level of detail, one shadow-map mip and at most 32 sounds at
  // once, whatever Roblox's graphics settings would choose.
  kRenderingLimits,
  // frame_rate_policy.cc DFIntTaskSchedulerTargetFps: a target Roblox's
  // Maximum Frame Rate menu otherwise sets.
  kFrameRateTarget,
  // legacy_runtime.cc calls platform_cache_migration.cc
  // ApplyRobloxThemeCacheOverride for every mode but roblox: the theme is
  // written over Roblox's saved one (AuthenticatedTheme, DeviceLevelTheme)
  // at every start.
  kTheme,
  // window.cc ApplyConfiguredWindowStartMode: the window opens in this mode
  // and, after the first frame, MaybeSynchronizeRestoredFullscreenState sets
  // Roblox's own fullscreen setting (UserGameSettings.InFullScreen, through
  // roblox_fullscreen_runtime_bridge.cc) to match.
  kStartMode,
  // roblox_permissions_bridge.cc: Roblox is refused the microphone
  // permission, so voice chat cannot record.
  kMicrophone,
  // main.cc ApplyDesktopAppPolicy (the PC profile's desktop layout over the
  // app policy Roblox's servers send) and device_profile.h (the console
  // profile's admission identity).
  kDevice,
};

struct RobloxOverride {
  RobloxOverrideKind kind = RobloxOverrideKind::kGraphicsQuality;
  // The config.yaml key whose value causes it: the row to change.
  std::string key;
  // kGraphicsQuality: the forced level. kFrameRateTarget: the target.
  int number = 0;
  // kGraphicsQuality: "default" or "level" (a level from config.yaml).
  // kFrameRateTarget: "unlimited" or "fixed". kTheme: system, light or
  // dark. kStartMode: windowed, maximized or fullscreen. kDevice: the
  // preset's canonical name (pc-windows-11, console-ps5).
  std::string value;
};

// Every in-game setting the game will override with `settings`, in the
// order of the settings window's pages.
std::vector<RobloxOverride> ResolveRobloxOverrides(
    const GameSettings& settings);

// The override that `key`'s current value causes, nullopt when it causes
// none (another key may still cause it: the rendering preset follows both
// performance keys).
std::optional<RobloxOverride> RobloxOverrideOf(const GameSettings& settings,
                                               std::string_view key);

// Roblox's own settings that stay in Roblox's hands with `settings`.
enum class RobloxOwnedSetting {
  // Nothing Mocktail sets touches these.
  kVolume,
  kCameraSensitivity,
  kCameraMode,
  kChat,
  // roblox_output_device_bridge.cc: Roblox's audio menu switches the output
  // while playing; audio.output_device only picks the device at start.
  kOutputSwitch,
  // Each of these is Roblox's unless the matching override is active.
  kGraphicsQuality,
  kFrameRate,
  kTheme,
  kFullscreen,
  kVoiceChat,
};

std::vector<RobloxOwnedSetting> RobloxOwnedSettings(
    const GameSettings& settings);

// Settings that work against each other in Roblox.
enum class OverrideConflict {
  // A level above Mocktail's default 3 while the preset keeps MSAA off and
  // textures at 128 MB: the level gains less than it would.
  kHighLevelUnderPresetLimits,
  // Roblox's slider picks the level, but the preset's other limits stay.
  kRobloxSliderUnderPresetLimits,
};

std::vector<OverrideConflict> FindOverrideConflicts(
    const GameSettings& settings);

// Client settings the game sets on every start, whatever config.yaml says.
enum class AlwaysOnFlag {
  // frame_rate_policy.cc: FFlagGameBasicSettingsFramerateCap5=True turns
  // on Roblox's Maximum Frame Rate menu.
  kFrameRateMenu,
  // main.cc MergeAudioDeviceMenuClientSettingsOverrides, for Roblox
  // versions whose audio devices Mocktail bridges:
  // FFlagDebugUseWebRtcAudioDevices and FFlagRemoteAudioDeviceSync off, so
  // the in-game device menu lists the devices Mocktail bridges.
  kAudioDeviceMenu,
  // performance_policy.cc MergeAudioCaptureClientSettingsOverrides:
  // DFFlagVoiceChatSkipPermissionCheckForTests off, so a disabled
  // microphone stays disabled.
  kMicrophonePermissionCheck,
  // texture_memory_policy.cc: FIntRenderForceVideoMemorySize from the RAM
  // (an eighth, 256 MiB to 1.5 GiB) on computers with 4 GiB or more,
  // unless fflags.json sets it.
  kVideoMemory,
  // http_client_policy.cc: FFlagUseRuntimeMutexRvHttpClient,
  // DFFlagHttpClientSkipRetryForStreamingRequests and
  // FFlagLuaAppDefaultHttpRetry keep web requests on paths that work under
  // Mocktail.
  kWebRequests,
  // crash_report_policy.cc: Roblox's crash and error uploads are off.
  kCrashUploads,
};

struct AlwaysOnFlagInfo {
  AlwaysOnFlag flag;
  // The client settings with the values the runtime sets, comma-separated,
  // for the overview ("FFlagGameBasicSettingsFramerateCap5=True").
  std::string_view names;
};

const std::vector<AlwaysOnFlagInfo>& AlwaysOnFlags();

// Roblox's video memory budget the game sets for this much RAM
// (CalculateTextureMemoryBudgetBytes); 0 when it sets none.
std::uint64_t VideoMemoryBudgetBytes(std::uint64_t memory_bytes);

// ---- Graphics card and shader loading (engine.gpu, engine.nvidia_shader_mt)

// Whether engine.gpu can choose `kind` here (graphics_launch_policy.cc
// SelectHostGpu, which falls back to another card).
enum class GpuChoiceState {
  kAvailable,
  // Detection has not finished, or Mocktail does not pick the card (the
  // user's VK_DRIVER_FILES, no driver Mocktail recognizes): nothing to say.
  kUnknown,
  // The computer has no card of that kind.
  kNoSuchCard,
  // It has one, but no card of that kind has a Vulkan driver; another card
  // stands in.
  kNoVulkanDriver,
};

GpuChoiceState GpuChoiceAvailability(const MachineProfile& machine,
                                     runtime::GpuPreference kind);

enum class GpuRecommendationReason {
  // Only one graphics card: every value uses it.
  kSingleCard,
  // Automatic picks the discrete card, the faster one.
  kDiscreteCard,
  // Automatic follows DRI_PRIME or __NV_PRIME_RENDER_OFFLOAD set to 0 and
  // picks the integrated graphics.
  kPrimeIntegrated,
  // Mocktail does not pick the card here, or detection has not finished.
  kUnknown,
};

struct GpuRecommendation {
  std::string value;  // "auto", or empty for kUnknown
  GpuRecommendationReason reason = GpuRecommendationReason::kUnknown;
};

GpuRecommendation RecommendGpuPreference(const MachineProfile& machine);

// What engine.nvidia_shader_mt does here (graphics_launch_policy.cc
// MergeNvidiaShaderLoadingClientSettingsOverrides: the deny pattern
// 4318:.* names NVIDIA's PCI vendor and is merged for direct Vulkan only).
enum class ShaderLoadingState {
  // Detection has not finished.
  kUnknown,
  // No NVIDIA card: the restriction never applied here.
  kNoNvidia,
  // Another backend than direct Vulkan, which alone reads the flag.
  kNotVulkan,
  // Direct Vulkan renders on another card than the NVIDIA one (engine.gpu).
  kOtherCard,
  // NVIDIA with direct Vulkan: several threads (true) or one (false).
  kMultithreaded,
  kSingleThread,
};

// `backend`, `gpu_preference` and `value` are the config.yaml values of
// graphics.backend, engine.gpu and engine.nvidia_shader_mt.
ShaderLoadingState ResolveShaderLoading(const MachineProfile& machine,
                                        std::string_view backend,
                                        std::string_view gpu_preference,
                                        std::string_view value);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ROBLOX_OVERRIDES_H_
