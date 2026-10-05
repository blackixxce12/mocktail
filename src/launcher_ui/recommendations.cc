#include "launcher_ui/recommendations.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail::launcher_ui {
namespace {

std::optional<int> ParsePositive(std::string_view value) {
  int number = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  if (value.empty() || parsed.ec != std::errc() ||
      parsed.ptr != value.data() + value.size() || number <= 0) {
    return std::nullopt;
  }
  return number;
}

}  // namespace

BackendRecommendation RecommendGraphicsBackend(const MachineProfile& machine) {
  if (!machine.detected) {
    // direct-vulkan is the default (runtime_config.h).
    return {"direct-vulkan", BackendRecommendationReason::kUnknown};
  }
  // ANGLE is never recommended: it adds a translation layer, is
  // experimental on NVIDIA (upstream issue #149) and depends on a browser's
  // libraries (research/graphics.md 1.3, 1.7).
  if (machine.has_vulkan_driver()) {
    return {"direct-vulkan", BackendRecommendationReason::kVulkanDriver};
  }
  if (machine.vulkan_source == VulkanDriverSource::kUnknown) {
    return {"direct-vulkan", BackendRecommendationReason::kUnknown};
  }
  return {"opengl", BackendRecommendationReason::kNoVulkanDriver};
}

// ---- Graphics, Display, Performance and Audio pages ------------------------

bool RenderingPresetActive(std::string_view physics_worker_mode,
                           std::string_view multithreaded_rendering) {
  // runtime_config.cc: MOCKTAIL_PHYSICS_WORKER_MODE defaults to throughput
  // and multithreaded rendering to off.
  const std::string_view physics =
      physics_worker_mode.empty() ? "throughput" : physics_worker_mode;
  const bool multithreaded = multithreaded_rendering == "true";
  if (physics == "latency") return false;
  if (physics == "throughput") return true;
  return physics == "auto" && multithreaded;
}

std::optional<int> ParseQualityLevel(std::string_view value) {
  // runtime_config.cc ParseGraphicsQuality: levels 1..21.
  const std::optional<int> level = ParsePositive(value);
  if (!level.has_value() || *level > 21) return std::nullopt;
  return level;
}

QualityEffect ResolveGraphicsQuality(std::string_view graphics_quality,
                                     bool preset_active,
                                     bool intel_integrated_vulkan) {
  QualityEffect effect;
  if (graphics_quality == "manual") return effect;
  const std::optional<int> level = ParseQualityLevel(graphics_quality);
  if (!preset_active) {
    // performance_policy.cc sets FIntDebugFRMQualityLevelOverride only
    // together with the rendering preset.
    effect.ignored = true;
    return effect;
  }
  if (level.has_value()) {
    effect.source = QualitySource::kConfiguredLevel;
    effect.level = *level;
    return effect;
  }
  // "default" (or absent): MOCKTAIL_GRAPHICS_QUALITY stays unset, so the
  // preset uses "3"; graphics_launch_policy.cc publishes "1" first when
  // direct Vulkan renders on Intel integrated graphics.
  effect.source = QualitySource::kMocktailDefault;
  effect.level = intel_integrated_vulkan ? 1 : 3;
  return effect;
}

QualityRecommendation RecommendGraphicsQuality(const MachineProfile& machine) {
  if (!machine.detected) return {};
  if (machine.gpu.discrete()) {
    return {"manual", QualityRecommendationReason::kCapableGraphics};
  }
  if (machine.gpu.integrated_only()) {
    return {"default", QualityRecommendationReason::kModestGraphics};
  }
  return {};
}

Presentation ResolvePresentation(std::string_view vsync,
                                 std::string_view frame_rate_limit) {
  if (vsync == "on") return Presentation::kSynchronized;
  if (vsync == "off") return Presentation::kUnthrottled;
  if (frame_rate_limit == "unlimited") return Presentation::kUnthrottled;
  if (frame_rate_limit.empty() || frame_rate_limit == "-1") {
    return Presentation::kDriverDefault;
  }
  return Presentation::kSynchronized;
}

std::optional<int> ParseFrameRate(std::string_view value) {
  return ParsePositive(value);
}

std::optional<std::string> RecommendFrameRate(const MachineProfile& machine) {
  const MonitorInfo& monitor = machine.monitor;
  const int refresh = monitor.valid ? monitor.RefreshHz() : 0;
  if (refresh <= 0) return std::nullopt;
  if (refresh <= 60) return std::string("-1");
  if (machine.detected && machine.gpu.integrated_only()) return std::nullopt;
  // frame_rate_policy.h kMaximumSupportedRobloxSchedulerFps.
  if (refresh > runtime::kMaximumSupportedRobloxSchedulerFps) {
    return std::string("unlimited");
  }
  return std::to_string(refresh);
}

std::string FrameRateFlagConflict(const launcher::FastFlagsDocument& flags,
                                  std::string_view frame_rate_limit) {
  const runtime::FrameRatePolicy frame_rate =
      runtime::ParseFrameRatePolicy(frame_rate_limit);
  if (!frame_rate.valid()) return {};
  // No preset: only the frame-rate (and always-on) policies are compared.
  const runtime::PerformancePolicy performance =
      runtime::ParsePerformancePolicy("false", "0", "auto", "auto");
  for (const launcher::FastFlagConflict& conflict :
       flags.FindManagedConflicts(frame_rate, performance)) {
    if (conflict.effect == launcher::FastFlagConflictEffect::kBlocksStart &&
        (conflict.name == "DFIntTaskSchedulerTargetFps" ||
         conflict.name == "FFlagGameBasicSettingsFramerateCap5")) {
      return conflict.name;
    }
  }
  return {};
}

std::string QualityFlagConflict(const launcher::FastFlagsDocument& flags,
                                const QualityEffect& quality) {
  constexpr const char* kName = "FIntDebugFRMQualityLevelOverride";
  if (quality.source == QualitySource::kRobloxSetting) return {};
  const launcher::FastFlagEntry* entry = flags.Find(kName);
  // performance_policy.cc refuses a different explicit value.
  if (entry == nullptr ||
      entry->RobloxValue() == std::to_string(quality.level)) {
    return {};
  }
  return kName;
}

std::vector<WindowSizePreset> WindowSizePresets(const MonitorInfo& monitor) {
  constexpr WindowSize kCommon[] = {{3840, 2160}, {2560, 1440}, {1920, 1080},
                                    {1600, 900},  {1440, 810},  {1366, 768},
                                    {1280, 720},  {1024, 576},  {960, 540}};
  constexpr std::size_t kMaximumFitting = 4;
  std::vector<WindowSizePreset> presets;
  const auto contains = [&presets](WindowSize size) {
    return std::any_of(
        presets.begin(), presets.end(),
        [size](const WindowSizePreset& preset) { return preset.size == size; });
  };
  const bool known = monitor.valid && monitor.width >= kMinimumWindowWidth &&
                     monitor.height >= kMinimumWindowHeight;
  if (known) {
    const WindowSize whole = {std::min(monitor.width, kMaximumWindowExtent),
                              std::min(monitor.height, kMaximumWindowExtent)};
    presets.push_back({whole, true, whole == kDefaultWindowSize});
    std::size_t fitting = 0;
    for (const WindowSize size : kCommon) {
      if (fitting == kMaximumFitting) break;
      if (size.width > whole.width || size.height > whole.height ||
          contains(size)) {
        continue;
      }
      presets.push_back({size, false, size == kDefaultWindowSize});
      ++fitting;
    }
  } else {
    for (const WindowSize size :
         {WindowSize{1920, 1080}, WindowSize{1600, 900}, WindowSize{1280, 720},
          WindowSize{960, 540}}) {
      presets.push_back({size, false, size == kDefaultWindowSize});
    }
  }
  if (!contains(kDefaultWindowSize)) {
    presets.push_back({kDefaultWindowSize, false, true});
  }
  std::stable_sort(presets.begin(), presets.end(),
                   [](const WindowSizePreset& a, const WindowSizePreset& b) {
                     return a.size.width * a.size.height >
                            b.size.width * b.size.height;
                   });
  return presets;
}

WindowMode StartWindowMode(std::string_view start_mode, bool remembered_found,
                           bool remembered_fullscreen,
                           bool remembered_maximized) {
  // window.cc ApplyConfiguredWindowStartMode / runtime_config.cc
  // ApplyWindowStartMode.
  if (start_mode == "windowed") return WindowMode::kWindowed;
  if (start_mode == "maximized") return WindowMode::kMaximized;
  if (start_mode == "fullscreen") return WindowMode::kFullscreen;
  if (!remembered_found) return WindowMode::kWindowed;
  // Fullscreen is applied at creation and wins over maximized
  // (window_state_store.cc PlanWindowStartupPresentation).
  if (remembered_fullscreen) return WindowMode::kFullscreen;
  if (remembered_maximized) return WindowMode::kMaximized;
  return WindowMode::kWindowed;
}

GameResolution ComputeGameResolution(const MonitorInfo& monitor,
                                     WindowSize windowed, WindowMode mode,
                                     bool high_dpi, bool wayland) {
  GameResolution resolution;
  resolution.logical = windowed;
  if (mode != WindowMode::kWindowed && monitor.valid) {
    resolution.logical = {monitor.width, monitor.height};
    resolution.approximate = mode == WindowMode::kMaximized;
  }
  resolution.pixels = resolution.logical;
  const bool scaled = monitor.valid && monitor.scale > 1.0 + 1e-6;
  if (!wayland || !scaled) return resolution;
  resolution.high_dpi_matters = true;
  if (high_dpi) {
    resolution.pixels = {
        static_cast<int>(std::lround(resolution.logical.width * monitor.scale)),
        static_cast<int>(
            std::lround(resolution.logical.height * monitor.scale))};
  } else {
    resolution.upscaled = true;
  }
  return resolution;
}

std::optional<std::string> RecommendHighDpi(const MachineProfile& machine,
                                            bool wayland) {
  if (!wayland || !machine.monitor.valid ||
      machine.monitor.scale <= 1.0 + 1e-6) {
    return std::nullopt;
  }
  if (machine.detected && machine.gpu.integrated_only()) {
    return std::string("false");
  }
  return std::string("true");
}

DisplayServerChoice ResolveDisplayServer(const MachineProfile& machine,
                                         std::string_view configured,
                                         std::string_view backend,
                                         std::string_view gpu_preference,
                                         Presentation presentation) {
  if (!machine.detected) return {};
  const bool chosen_wayland = configured == "wayland";
  const bool chosen_x11 = configured == "x11";
  if (chosen_wayland && machine.wayland_available) {
    return {"wayland", DisplayServerReason::kChosen};
  }
  if (chosen_x11 && machine.x11_available) {
    return {"x11", DisplayServerReason::kChosen};
  }
  // runtime_config.h: direct-vulkan and auto are the built-in defaults.
  if (backend.empty()) backend = "direct-vulkan";
  if (gpu_preference.empty()) gpu_preference = "auto";
  DisplayServerChoice choice;
  choice.server = machine.AutomaticDisplayServer(
      backend, gpu_preference, presentation == Presentation::kUnthrottled);
  if (chosen_wayland || chosen_x11) {
    choice.reason = DisplayServerReason::kChosenUnavailable;
  } else if (choice.server.empty()) {
    choice.reason = DisplayServerReason::kUnknown;
  } else if (machine.NvidiaRuleApplies(backend, gpu_preference)) {
    choice.reason = choice.server == "wayland"
                        ? DisplayServerReason::kNvidiaVulkanWayland
                        : DisplayServerReason::kNvidiaVulkanX11;
  } else if (choice.server == "wayland") {
    choice.reason = DisplayServerReason::kWaylandSession;
  } else {
    // Only the NVIDIA rule picks X11 beside a Wayland session
    // (video_driver_policy.cc; SDL's own order prefers Wayland).
    choice.reason = DisplayServerReason::kX11Only;
  }
  return choice;
}

std::uint64_t SuggestedMemoryLimitMiB(std::uint64_t memory_bytes) {
  constexpr std::uint64_t kStep = 512;
  constexpr std::uint64_t kMinimum = 4096;
  const std::uint64_t memory_mib = memory_bytes / (1024U * 1024U);
  std::uint64_t suggested = std::max(memory_mib * 3 / 16, kMinimum);
  // On a small computer 4 GiB would be more than it has: the watchdog
  // counts swap too, so the game would run out of memory first.
  if (memory_mib > 0) suggested = std::min(suggested, memory_mib * 3 / 4);
  return std::max(suggested / kStep * kStep, kStep);
}

AudioDeviceState ClassifyAudioDevice(std::string_view value,
                                     const std::vector<std::string>* devices,
                                     bool input) {
  if (value.empty() || value == "default") return AudioDeviceState::kDefault;
  if (input && value == "disabled") return AudioDeviceState::kDisabled;
  if (input && value.rfind("id:", 0) == 0) return AudioDeviceState::kNumericId;
  if (devices == nullptr) return AudioDeviceState::kNotListed;
  const auto count = std::count(devices->begin(), devices->end(), value);
  if (count == 0) return AudioDeviceState::kMissing;
  return count == 1 ? AudioDeviceState::kConnected
                    : AudioDeviceState::kAmbiguous;
}

}  // namespace mocktail::launcher_ui
