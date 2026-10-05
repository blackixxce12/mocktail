#include "launcher_ui/roblox_overrides.h"

#include "launcher_ui/recommendations.h"
#include "runtime/device_profile.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/texture_memory_policy.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kQualityKey[] = "engine.graphics_quality";
constexpr char kPhysicsKey[] = "performance.physics_worker_mode";
constexpr char kMultithreadedKey[] = "performance.multithreaded_rendering";
constexpr char kFrameRateKey[] = "graphics.frame_rate_limit";
constexpr char kThemeKey[] = "appearance.theme";
constexpr char kStartModeKey[] = "display.start_mode";
constexpr char kInputKey[] = "audio.input_device";
constexpr char kDeviceKey[] = "device";

// Mocktail's level when engine.graphics_quality is default
// (performance_policy.cc uses "3" while MOCKTAIL_GRAPHICS_QUALITY is
// unset).
constexpr int kDefaultQualityLevel = 3;

bool PresetActive(const GameSettings& settings) {
  return RenderingPresetActive(settings.physics_worker_mode,
                               settings.multithreaded_rendering);
}

QualityEffect Quality(const GameSettings& settings) {
  // runtime_config.cc ParseGraphicsQualityVariable: the environment's auto
  // and 0 mean manual, as performance_policy.cc reads them.
  const std::string_view quality =
      settings.graphics_quality == "auto" || settings.graphics_quality == "0"
          ? std::string_view("manual")
          : std::string_view(settings.graphics_quality);
  return ResolveGraphicsQuality(quality, PresetActive(settings),
                                settings.intel_integrated_vulkan);
}

std::optional<RobloxOverride> QualityOverride(const GameSettings& settings) {
  const QualityEffect effect = Quality(settings);
  if (effect.source == QualitySource::kRobloxSetting) return std::nullopt;
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kGraphicsQuality;
  entry.key = kQualityKey;
  entry.number = effect.level;
  entry.value =
      effect.source == QualitySource::kConfiguredLevel ? "level" : "default";
  return entry;
}

std::optional<RobloxOverride> PresetOverride(const GameSettings& settings) {
  if (!PresetActive(settings)) return std::nullopt;
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kRenderingLimits;
  // runtime_config.cc: physics workers default to throughput, which turns
  // the preset on by itself; on auto, multithreaded rendering does.
  const std::string_view physics =
      settings.physics_worker_mode.empty()
          ? std::string_view("throughput")
          : std::string_view(settings.physics_worker_mode);
  entry.key = physics == "throughput" ? kPhysicsKey : kMultithreadedKey;
  return entry;
}

std::optional<RobloxOverride> FrameRateOverride(const GameSettings& settings) {
  // frame_rate_policy.cc: -1 and display add no target; unlimited targets
  // kMaximumSupportedRobloxSchedulerFps; a number is passed on verbatim.
  const runtime::FrameRatePolicy policy =
      runtime::ParseFrameRatePolicy(settings.frame_rate_limit);
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kFrameRateTarget;
  entry.key = kFrameRateKey;
  if (policy.mode == runtime::FrameRateLimitMode::kFixed) {
    entry.number = policy.fixed_fps;
    entry.value = "fixed";
    return entry;
  }
  if (policy.mode == runtime::FrameRateLimitMode::kUnlimited) {
    entry.number = runtime::kMaximumSupportedRobloxSchedulerFps;
    entry.value = "unlimited";
    return entry;
  }
  return std::nullopt;
}

std::optional<RobloxOverride> ThemeOverride(const GameSettings& settings) {
  // runtime_config.h: roblox is the default; legacy_runtime.cc writes every
  // other mode over Roblox's saved theme.
  const std::string& theme = settings.theme;
  if (theme != "system" && theme != "light" && theme != "dark") {
    return std::nullopt;
  }
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kTheme;
  entry.key = kThemeKey;
  entry.value = theme;
  return entry;
}

std::optional<RobloxOverride> StartModeOverride(const GameSettings& settings) {
  // runtime_config.h ParseWindowStartMode: remember (the default) keeps the
  // mode the last session ended in, including Roblox's own switch.
  const std::string& mode = settings.start_mode;
  if (mode != "windowed" && mode != "maximized" && mode != "fullscreen") {
    return std::nullopt;
  }
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kStartMode;
  entry.key = kStartModeKey;
  entry.value = mode;
  return entry;
}

std::optional<RobloxOverride> MicrophoneOverride(const GameSettings& settings) {
  // runtime_config.h microphone_enabled().
  if (settings.input_device != "disabled") return std::nullopt;
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kMicrophone;
  entry.key = kInputKey;
  return entry;
}

std::optional<RobloxOverride> DeviceOverride(const GameSettings& settings) {
  const runtime::DeviceProfile* profile = runtime::FindDeviceProfile(
      settings.device.empty() ? runtime::kDefaultDeviceProfileName
                              : std::string_view(settings.device));
  // The phone profile is what Roblox for Android expects: its own layout
  // and admission. The PC profile replaces the layout (desktop_playability,
  // main.cc ApplyDesktopAppPolicy) and the console one the admission
  // identity (kRobloxConsoleAdmissionUserAgent).
  if (profile == nullptr ||
      profile->device_class == runtime::DeviceClass::kMobile) {
    return std::nullopt;
  }
  RobloxOverride entry;
  entry.kind = RobloxOverrideKind::kDevice;
  entry.key = kDeviceKey;
  entry.value = profile->name;
  return entry;
}

}  // namespace

std::vector<RobloxOverride> ResolveRobloxOverrides(
    const GameSettings& settings) {
  std::vector<RobloxOverride> overrides;
  for (const std::optional<RobloxOverride>& entry :
       {QualityOverride(settings), FrameRateOverride(settings),
        StartModeOverride(settings), ThemeOverride(settings),
        PresetOverride(settings), MicrophoneOverride(settings),
        DeviceOverride(settings)}) {
    if (entry.has_value()) overrides.push_back(*entry);
  }
  return overrides;
}

std::optional<RobloxOverride> RobloxOverrideOf(const GameSettings& settings,
                                               std::string_view key) {
  for (const RobloxOverride& entry : ResolveRobloxOverrides(settings)) {
    if (entry.key == key) return entry;
  }
  return std::nullopt;
}

std::vector<RobloxOwnedSetting> RobloxOwnedSettings(
    const GameSettings& settings) {
  std::vector<RobloxOwnedSetting> owned;
  if (!QualityOverride(settings).has_value()) {
    owned.push_back(RobloxOwnedSetting::kGraphicsQuality);
  }
  if (!FrameRateOverride(settings).has_value()) {
    owned.push_back(RobloxOwnedSetting::kFrameRate);
  }
  if (!StartModeOverride(settings).has_value()) {
    owned.push_back(RobloxOwnedSetting::kFullscreen);
  }
  if (!ThemeOverride(settings).has_value()) {
    owned.push_back(RobloxOwnedSetting::kTheme);
  }
  owned.push_back(RobloxOwnedSetting::kVolume);
  owned.push_back(RobloxOwnedSetting::kOutputSwitch);
  if (!MicrophoneOverride(settings).has_value()) {
    owned.push_back(RobloxOwnedSetting::kVoiceChat);
  }
  owned.push_back(RobloxOwnedSetting::kCameraSensitivity);
  owned.push_back(RobloxOwnedSetting::kCameraMode);
  owned.push_back(RobloxOwnedSetting::kChat);
  return owned;
}

std::vector<OverrideConflict> FindOverrideConflicts(
    const GameSettings& settings) {
  std::vector<OverrideConflict> conflicts;
  if (!PresetActive(settings)) return conflicts;
  const QualityEffect effect = Quality(settings);
  if (effect.source == QualitySource::kConfiguredLevel &&
      effect.level > kDefaultQualityLevel) {
    conflicts.push_back(OverrideConflict::kHighLevelUnderPresetLimits);
  }
  if (effect.source == QualitySource::kRobloxSetting) {
    conflicts.push_back(OverrideConflict::kRobloxSliderUnderPresetLimits);
  }
  return conflicts;
}

const std::vector<AlwaysOnFlagInfo>& AlwaysOnFlags() {
  // The values as the runtime sets them; the video memory budget depends on
  // the RAM (VideoMemoryBudgetBytes), and the crash report policy sets 78
  // client settings, of which the first two are named.
  static const std::vector<AlwaysOnFlagInfo> kFlags = {
      {AlwaysOnFlag::kFrameRateMenu,
       "FFlagGameBasicSettingsFramerateCap5=True"},
      {AlwaysOnFlag::kAudioDeviceMenu,
       "FFlagDebugUseWebRtcAudioDevices=False, "
       "FFlagRemoteAudioDeviceSync=False"},
      {AlwaysOnFlag::kMicrophonePermissionCheck,
       "DFFlagVoiceChatSkipPermissionCheckForTests=False"},
      {AlwaysOnFlag::kVideoMemory, "FIntRenderForceVideoMemorySize"},
      {AlwaysOnFlag::kWebRequests,
       "FFlagUseRuntimeMutexRvHttpClient=False, "
       "DFFlagHttpClientSkipRetryForStreamingRequests=True, "
       "FFlagLuaAppDefaultHttpRetry=False"},
      {AlwaysOnFlag::kCrashUploads,
       "DFFlagUseCrashpad=False, FFlagUseCrashpad=False, …"},
  };
  return kFlags;
}

std::uint64_t VideoMemoryBudgetBytes(std::uint64_t memory_bytes) {
  return runtime::CalculateTextureMemoryBudgetBytes(memory_bytes);
}

}  // namespace mocktail::launcher_ui
