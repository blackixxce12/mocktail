#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"

namespace mocktail::launcher_ui {
namespace {

bool IsVulkanSpelling(const std::string& value) {
  return value == "direct-vulkan" || value == "vulkan" ||
         value == "native-vulkan";
}

// The worked example of a fully hinted row: every option described, the
// recommendation computed from the machine, warnings for what can go wrong
// here, and details that say what was found on this computer. Sources for
// each claim are in the comments; research/graphics.md 1 has the details.
GtkWidget* BuildGraphicsBackendRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "graphics.backend";
  spec.title = _("Graphics backend");
  // runtime_config.h: direct-vulkan is the built-in default.
  spec.fallback = "direct-vulkan";
  spec.keywords = {"vulkan", "opengl",    "gles",   "angle",      "renderer",
                   "render", "gpu",       "driver", "--graphics", "графика",
                   "рендер", "отрисовка", "движок", "драйвер",    "видеокарта"};
  spec.hint.details =
      // research/graphics.md 1.3-1.5.
      _("Chooses how Roblox draws the game. The change applies the next time "
        "Roblox starts.") +
      std::string("\n\n") +
      // bionic_vulkan_loader_adapter.cc: present modes are chosen only in
      // the Vulkan adapter; vulkan_etc2_emulation.cc decodes ETC2 when the
      // device lacks textureCompressionETC2.
      _("• Vulkan: Roblox's own Vulkan renderer talks to your graphics "
        "driver directly. It is the default, and the only backend where "
        "Vertical sync chooses how frames are presented. When the driver "
        "lacks the ETC2 texture format Android games use (common on desktop "
        "NVIDIA and AMD cards), Mocktail decodes those textures itself.") +
      "\n\n" +
      // stubs/libegl_stub.cc eglSwapInterval is a no-op; Mocktail never
      // calls SDL_GL_SetSwapInterval (research/graphics.md 1.5).
      _("• OpenGL ES: Roblox's OpenGL ES 3.0 renderer on your system's EGL "
        "driver. Use it when Vulkan is missing or crashes. Vertical sync has "
        "no effect here: frames follow the driver's default, which normally "
        "waits for the display.") +
      "\n\n" +
      // window.cc FindInstalledAngleLibraries; upstream issue #149.
      _("• ANGLE on Vulkan: the OpenGL ES renderer, translated to Vulkan by "
        "the ANGLE libraries of a Chromium or Electron installed on this "
        "computer. It adds a translation layer, is experimental on NVIDIA "
        "(upstream issue #149), and can break when that browser updates. Try "
        "it only when Vulkan shows glitches and OpenGL ES does not work.") +
      "\n\n" +
      // config/mocktail.example.yaml updates.automatic: the canaries run
      // with the selected graphics backend.
      _("Automatic Roblox updates test a new Roblox version with the backend "
        "chosen here before switching to it, so a version that fails with it "
        "is not installed.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MachineProfile& machine = ctx.machine();
    if (!machine.detected) return std::string();
    std::string text;
    text += machine.has_vulkan_driver()
                ? Format(_("Vulkan driver on this computer: %s"),
                         machine.vulkan_icd.c_str())
                : std::string(_("No Vulkan driver for your graphics card was "
                                "found in the usual places "
                                "(/usr/share/vulkan/icd.d)."));
    text += "\n\n";
    if (machine.angle.has_value()) {
      text += machine.angle->bundled
                  ? Format(_("ANGLE for “ANGLE on Vulkan”: Mocktail's own copy "
                             "in %s."),
                           machine.angle->directory.c_str())
                  : Format(_("ANGLE for “ANGLE on Vulkan”: %s in %s."),
                           machine.angle->label.c_str(),
                           machine.angle->directory.c_str());
    } else {
      text +=
          _("No ANGLE libraries were found, so “ANGLE on Vulkan” is not "
            "available.");
    }
    if (machine.NvidiaDirectVulkanUsesX11()) {
      // video_driver_policy.cc rule 4 and the reason in
      // video_driver_policy.h.
      text += "\n\n";
      text +=
          _("With Vulkan on NVIDIA, the automatic display server runs "
            "the game through XWayland, because NVIDIA's native Wayland "
            "path can hang or lose the display. Display › Display server "
            "can change that.");
    }
    return text;
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return std::optional<std::string>(RecommendGraphicsBackend(machine).value);
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    switch (RecommendGraphicsBackend(machine).reason) {
      case BackendRecommendationReason::kVulkanDriver: {
        const std::string vendors = machine.GpuVendorsLabel();
        return vendors.empty()
                   ? std::string(_("A Vulkan driver is installed, and Vulkan "
                                   "is Mocktail's default and most direct "
                                   "path."))
                   : Format(_("A Vulkan driver for your %s graphics is "
                              "installed, and Vulkan is Mocktail's default "
                              "and most direct path."),
                            vendors.c_str());
      }
      case BackendRecommendationReason::kNoVulkanDriver:
        return std::string(
            _("No Vulkan driver was found, and OpenGL ES works "
              "with any driver that supports OpenGL ES 3.0."));
      case BackendRecommendationReason::kUnknown:
        break;
    }
    return std::string();
  };
  spec.hint.warning = [](LauncherContext& ctx, const std::string& value) {
    const MachineProfile& machine = ctx.machine();
    if (value == "vulkan" || value == "native-vulkan") {
      // window.cc opens the Vulkan window only for the exact name
      // direct-vulkan (research/graphics.md 1.2).
      return std::string(
          _("This spelling starts an OpenGL window instead of "
            "the Vulkan one. Choose Vulkan to fix it."));
    }
    if (IsVulkanSpelling(value) && machine.detected &&
        !machine.has_vulkan_driver()) {
      return std::string(
          _("No Vulkan driver was found for your graphics "
            "card, so Roblox may fail to start. Install one or "
            "choose OpenGL ES."));
    }
    if (value == "angle-vulkan") {
      if (machine.detected && !machine.angle.has_value()) {
        return std::string(
            _("No ANGLE libraries were found, so Roblox cannot "
              "start with this backend."));
      }
      if (machine.gpu.nvidia) {
        return std::string(
            _("Experimental on NVIDIA: ANGLE on Vulkan has "
              "failed there before (upstream issue #149)."));
      }
      if (machine.angle.has_value() && !machine.angle->looks_like_angle) {
        return Format(_("The libraries in %s do not look like ANGLE. Roblox "
                        "checks them at start and stops if they are not."),
                      machine.angle->directory.c_str());
      }
    }
    if (value == "angle-swiftshader") {
      // window.cc: SwiftShader is ANGLE's CPU renderer.
      return std::string(
          _("Software rendering on the processor: very slow, "
            "only for diagnosing problems."));
    }
    if ((value == "opengl" || value == "gles" || value == "system" ||
         value == "auto") &&
        ctx.EffectiveValue("graphics.vsync", "auto") != "auto") {
      return std::string(
          _("Vertical sync is set, but it has no effect with "
            "OpenGL ES."));
    }
    return std::string();
  };

  ComboSpec combo;
  combo.options = {
      {"direct-vulkan",
       _("Vulkan"),
       _("Roblox's Vulkan renderer, straight to your graphics driver"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"opengl",
       _("OpenGL ES"),
       _("For a missing or broken Vulkan driver; Vertical sync has no effect"),
       // runtime_config.cc ParseGraphicsBackend: gles is the same strict
       // EGL path.
       {"gles"},
       nullptr,
       nullptr,
       false,
       false},
      {"angle-vulkan",
       _("ANGLE on Vulkan"),
       {},
       {},
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         if (machine.angle.has_value() && !machine.angle->bundled) {
           return Format(_("OpenGL ES translated to Vulkan by the ANGLE of "
                           "%s; experimental"),
                         machine.angle->label.c_str());
         }
         return std::string(
             _("OpenGL ES translated to Vulkan by ANGLE; "
               "experimental"));
       },
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         return machine.detected && !machine.angle.has_value()
                    ? std::string(_("No Chromium or Electron ANGLE libraries "
                                    "were found on this computer"))
                    : std::string();
       },
       false,
       false},
      // Listed only while config.yaml holds them.
      {"system",
       _("OpenGL ES with ANGLE retry"),
       // window.cc RetryWithAutoAngleFallback; "auto" is treated the same.
       _("OpenGL ES; if the window cannot start, one retry with ANGLE on "
         "Vulkan"),
       {"auto"},
       nullptr,
       nullptr,
       true,
       false},
      {"angle-swiftshader",
       _("ANGLE SwiftShader (software)"),
       _("Draws on the processor instead of the graphics card; very slow"),
       {},
       nullptr,
       nullptr,
       true,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

constexpr char kQualityKey[] = "engine.graphics_quality";
constexpr char kFrameRateKey[] = "graphics.frame_rate_limit";
constexpr char kVsyncKey[] = "graphics.vsync";

// What the rows of this page share: fflags.json as last saved (Mocktail
// refuses to start when it sets a flag the frame-rate or performance policy
// also sets, to another value) and the rows shown only for custom values.
// Owned by the page widget.
class GraphicsPageState {
 public:
  explicit GraphicsPageState(LauncherContext* context) : context_(context) {
    LoadFastFlags();
    listeners_.push_back(
        context_->OnSettingChanged([this](std::string_view key) {
          // Saved, discarded or reloaded: fflags.json may have changed too.
          if (key.empty()) LoadFastFlags();
          if (key.empty() || key == kFrameRateKey || key == kQualityKey) {
            UpdateCustomRows();
          }
        }));
    listeners_.push_back(
        context_->OnMachineChanged([this] { UpdateCustomRows(); }));
  }
  ~GraphicsPageState() {
    for (const LauncherContext::ListenerId id : listeners_) {
      context_->RemoveListener(id);
    }
  }
  GraphicsPageState(const GraphicsPageState&) = delete;
  GraphicsPageState& operator=(const GraphicsPageState&) = delete;

  const launcher::FastFlagsDocument* fast_flags() const {
    return fast_flags_loaded_ ? &fast_flags_ : nullptr;
  }

  // The level-number row is shown while engine.graphics_quality is a
  // number; the frame-rate number row while graphics.frame_rate_limit is a
  // number no option lists, or after "Custom…".
  void UpdateCustomRows() {
    if (quality_spin_ != nullptr) {
      gtk_widget_set_visible(
          quality_spin_,
          ParseQualityLevel(context_->EffectiveValue(kQualityKey, "default"))
              .has_value());
    }
    if (frame_rate_spin_ != nullptr) {
      const std::optional<int> fps =
          ParseFrameRate(context_->EffectiveValue(kFrameRateKey, "-1"));
      if (!fps.has_value()) custom_frame_rate_ = false;
      gtk_widget_set_visible(
          frame_rate_spin_,
          fps.has_value() && (custom_frame_rate_ || !IsListedFrameRate(*fps)));
    }
  }

  bool IsListedFrameRate(int fps) const {
    const MonitorInfo& monitor = context_->machine().monitor;
    if (monitor.valid && monitor.RefreshHz() == fps) return true;
    for (const int choice : kFrameRateChoices) {
      if (choice == fps) return true;
    }
    return false;
  }

  void set_quality_spin(GtkWidget* row) { quality_spin_ = row; }
  void set_frame_rate_spin(GtkWidget* row) { frame_rate_spin_ = row; }
  void RequestCustomFrameRate() { custom_frame_rate_ = true; }
  GtkWidget* quality_spin() const { return quality_spin_; }
  GtkWidget* frame_rate_spin() const { return frame_rate_spin_; }

 private:
  void LoadFastFlags() {
    std::string error;
    fast_flags_ = launcher::FastFlagsDocument();
    fast_flags_loaded_ = launcher::FastFlagsDocument::Load(
        context_->fast_flags_file(), &fast_flags_, &error);
  }

  LauncherContext* context_;
  std::vector<LauncherContext::ListenerId> listeners_;
  launcher::FastFlagsDocument fast_flags_;
  bool fast_flags_loaded_ = false;
  GtkWidget* quality_spin_ = nullptr;
  GtkWidget* frame_rate_spin_ = nullptr;
  bool custom_frame_rate_ = false;
};

// performance_policy.cc: the preset (and with it the forced quality level)
// follows the Performance page's physics workers and multithreading.
bool PresetActive(const LauncherContext& context) {
  return RenderingPresetActive(
      context.EffectiveValue("performance.physics_worker_mode", "throughput"),
      context.EffectiveValue("performance.multithreaded_rendering", "false"));
}

// graphics_launch_policy.cc: direct Vulkan (any spelling) on Intel-only
// graphics publishes MOCKTAIL_GRAPHICS_QUALITY=1 while the level is default.
bool IntelOnlyVulkan(const LauncherContext& context) {
  const MachineProfile& machine = context.machine();
  return machine.detected && machine.gpu.intel_only() &&
         IsVulkanSpelling(
             context.EffectiveValue("graphics.backend", "direct-vulkan"));
}

QualityEffect CurrentQuality(const LauncherContext& context,
                             const std::string& value) {
  return ResolveGraphicsQuality(value, PresetActive(context),
                                IntelOnlyVulkan(context));
}

std::vector<ComboOption> QualityOptions(LauncherContext& context) {
  std::vector<ComboOption> options;
  const int default_level = IntelOnlyVulkan(context) ? 1 : 3;
  options.push_back(
      {"default",
       Format(_("Mocktail default (level %d)"), default_level),
       {},
       {},
       [](LauncherContext& ctx) {
         if (!PresetActive(ctx)) {
           return std::string(
               _("Roblox's own setting decides while Mocktail's performance "
                 "preset is off"));
         }
         return IntelOnlyVulkan(ctx)
                    ? std::string(_("Level 1 of 21: Mocktail's choice for "
                                    "Intel graphics with Vulkan"))
                    : std::string(_("Level 3 of 21, forced by Mocktail's "
                                    "performance preset"));
       },
       nullptr,
       false,
       false});
  options.push_back(
      {"manual",
       _("Roblox in-game slider"),
       _("Roblox's Graphics Quality setting decides (automatic unless you "
         "change it)"),
       {},
       nullptr,
       nullptr,
       false,
       false});
  // A level from config.yaml is listed while it is the value; the number row
  // below changes it.
  for (int level = 1; level <= 21; ++level) {
    options.push_back({std::to_string(level),
                       Format(_("Level %d"), level),
                       {},
                       {},
                       [level](LauncherContext& ctx) {
                         return PresetActive(ctx)
                                    ? Format(_("Level %d of 21, forced by "
                                               "Mocktail"),
                                             level)
                                    : Format(_("Level %d of 21; no effect "
                                               "while the performance preset "
                                               "is off"),
                                             level);
                       },
                       nullptr,
                       true,
                       false});
  }
  options.push_back({"custom",
                     _("Custom level…"),
                     _("Pick a level from 1 (fastest) to 21 (best looking)"),
                     {},
                     nullptr,
                     nullptr,
                     false,
                     true});
  return options;
}

// engine.graphics_quality (SPEC 2; config/mocktail.example.yaml engine:).
GtkWidget* BuildGraphicsQualityRow(LauncherContext* context,
                                   GraphicsPageState* state) {
  RowSpec spec;
  spec.key = kQualityKey;
  spec.title = _("Graphics quality");
  spec.fallback = "default";
  spec.keywords = {"quality",  "level",    "detail",
                   "slider",   "frm",      "settings",
                   "качество", "графика",  "детализация",
                   "уровень",  "ползунок", "FIntDebugFRMQualityLevelOverride"};
  spec.hint.details =
      _("Roblox's graphics quality level, from 1 to 21, decides how much "
        "detail Roblox draws: view distance, shadows, effects and similar. "
        "Higher levels look better and need more of the graphics card.") +
      std::string("\n\n") +
      // performance_policy.cc: FIntDebugFRMQualityLevelOverride is merged
      // only together with the rendering preset.
      _("Mocktail forces a level only while its performance preset is on: "
        "with Physics workers on Throughput (the default), or on Automatic "
        "with Multithreaded rendering. Otherwise Roblox's own setting always "
        "decides.") +
      "\n\n" +
      // graphics_launch_policy.cc: MOCKTAIL_GRAPHICS_QUALITY=1 on Intel-only
      // machines with direct Vulkan.
      _("• Mocktail default: level 3, or level 1 with Vulkan on computers "
        "that have only Intel graphics. Roblox's in-game quality slider then "
        "has no effect.") +
      "\n\n" +
      _("• Roblox in-game slider: Mocktail forces nothing, and the Graphics "
        "Quality option in Roblox's settings decides. It is automatic unless "
        "you change it, so Roblox raises or lowers the level with the frame "
        "rate.") +
      "\n\n" + _("• Custom level: forces the level you pick.") + "\n\n" +
      // performance_policy.cc rendering_settings (MSAA, texture budgets,
      // shadow map mips, low-end LOD).
      _("The preset also lowers texture memory, turns anti-aliasing (MSAA) "
        "off and uses simpler shadows and level of detail, whatever level is "
        "chosen here.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const QualityEffect effect =
        CurrentQuality(ctx, ctx.EffectiveValue(kQualityKey, "default"));
    if (effect.source == QualitySource::kRobloxSetting) {
      return PresetActive(ctx)
                 ? std::string(_("With the current settings Roblox's own "
                                 "Graphics Quality setting decides."))
                 : std::string(_("Mocktail's performance preset is off with "
                                 "the current Performance settings, so "
                                 "Roblox's own Graphics Quality setting "
                                 "decides."));
    }
    return Format(_("With the current settings Roblox runs at level %d."),
                  effect.level);
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return std::optional<std::string>(RecommendGraphicsQuality(machine).value);
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    switch (RecommendGraphicsQuality(machine).reason) {
      case QualityRecommendationReason::kCapableGraphics:
        return Format(_("Your %s graphics can usually draw more than level 3, "
                        "and Roblox's automatic setting keeps the frame rate "
                        "up by itself."),
                      machine.GpuVendorsLabel().c_str());
      case QualityRecommendationReason::kModestGraphics:
        return std::string(
            _("With integrated or unknown graphics, Mocktail's low default "
              "keeps the frame rate up."));
      case QualityRecommendationReason::kUnknown:
        break;
    }
    return std::string();
  };
  spec.hint.warning = [state](LauncherContext& ctx, const std::string& value) {
    const QualityEffect effect = CurrentQuality(ctx, value);
    if (const launcher::FastFlagsDocument* flags = state->fast_flags()) {
      const std::string conflict = QualityFlagConflict(*flags, effect);
      if (!conflict.empty()) {
        return Format(_("fflags.json sets %s to another level, so Roblox will "
                        "not start. Remove that flag or choose “Roblox in-game "
                        "slider”."),
                      conflict.c_str());
      }
    }
    if (effect.ignored && ParseQualityLevel(value).has_value()) {
      return std::string(
          _("No effect: Mocktail's performance preset is off "
            "(Performance › Physics workers)."));
    }
    return std::string();
  };

  ComboSpec combo;
  combo.options_for = QualityOptions;
  combo.on_custom = [state](LauncherContext& ctx) {
    QualityEffect effect =
        CurrentQuality(ctx, ctx.EffectiveValue(kQualityKey, "default"));
    const int level = effect.level > 0 ? effect.level : 3;
    if (ctx.SetValue(kQualityKey, std::to_string(level),
                     launcher::ScalarKind::kInteger) &&
        state->quality_spin() != nullptr) {
      state->UpdateCustomRows();
      ctx.Reveal(state->quality_spin());
    }
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

GtkWidget* BuildQualityLevelRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kQualityKey;
  spec.title = _("Quality level");
  spec.fallback = "default";
  spec.keywords = {"quality level", "уровень качества"};
  spec.hint.subtitle = _("1 is the fastest, 21 looks best");
  spec.hint.details =
      _("The graphics quality level Mocktail forces while its performance "
        "preset is on. Level 1 draws the least and runs fastest; level 21 "
        "draws the most detail. Roblox's own quality slider has no effect "
        "while a level is set here.") +
      std::string("\n\n") +
      _("Change it in small steps and check the frame rate in a busy "
        "experience. Choose “Roblox in-game slider” above to let Roblox "
        "pick the level again.");
  SpinSpec spin;
  spin.minimum = 1;
  spin.maximum = 21;
  spin.step = 1;
  return BindSpinRow(context, std::move(spec), spin);
}

std::vector<ComboOption> FrameRateOptions(LauncherContext& context) {
  const MonitorInfo& monitor = context.machine().monitor;
  const int refresh = monitor.valid ? monitor.RefreshHz() : 0;
  std::vector<ComboOption> options;
  // frame_rate_policy.cc: -1 adds no scheduler target, and
  // FFlagGameBasicSettingsFramerateCap5 keeps Roblox's own menu enabled.
  options.push_back({"-1",
                     _("Set in Roblox"),
                     _("Roblox's own Maximum Frame Rate menu decides; Mocktail "
                       "sets no target"),
                     {},
                     nullptr,
                     nullptr,
                     false,
                     false});
  if (refresh > 0) {
    // SPEC 5: "Match display" writes the monitor's rounded refresh rate.
    options.push_back(
        {std::to_string(refresh),
         Format(_("Match display (%d Hz)"), refresh),
         Format(_("Target of %d frames per second, the refresh rate of this "
                  "screen"),
                refresh),
         {},
         nullptr,
         nullptr,
         false,
         false});
  }
  for (const int fps : kFrameRateChoices) {
    if (fps == refresh) continue;
    options.push_back({std::to_string(fps),
                       Format(_("%d FPS"), fps),
                       Format(_("Target of %d frames per second"), fps),
                       {},
                       nullptr,
                       nullptr,
                       false,
                       false});
  }
  // frame_rate_policy.h: unlimited is the payload's maximum target, 240;
  // present_mode_policy.cc presents it unthrottled under vsync auto.
  options.push_back(
      {"unlimited",
       _("240 (maximum)"),
       {},
       {},
       [](LauncherContext& ctx) {
         return ctx.EffectiveValue(kVsyncKey, "auto") == "on"
                    ? std::string(_("Roblox's highest target; Vertical sync "
                                    "On still waits for the display"))
                    : std::string(_("Roblox's highest target; Vulkan frames "
                                    "do not wait for the display"));
       },
       nullptr,
       false,
       false});
  // frame_rate_policy.cc: "display" sets no target either, but
  // present_mode_policy.cc synchronizes Vulkan frames under vsync auto.
  options.push_back({"display",
                     _("Set in Roblox, synchronized"),
                     _("Roblox's own menu decides; with Automatic vertical "
                       "sync, Vulkan frames wait for the display"),
                     {},
                     nullptr,
                     nullptr,
                     true,
                     false});
  const std::string value = context.EffectiveValue(kFrameRateKey, "-1");
  const std::optional<int> fps = ParseFrameRate(value);
  bool listed = false;
  for (const ComboOption& option : options) {
    if (option.value == value) listed = true;
  }
  if (fps.has_value() && !listed) {
    options.push_back({value,
                       Format(_("%d FPS (custom)"), *fps),
                       Format(_("Target of %d frames per second"), *fps),
                       {},
                       nullptr,
                       nullptr,
                       true,
                       false});
  }
  options.push_back({"custom",
                     _("Custom…"),
                     _("Any number of frames per second"),
                     {},
                     nullptr,
                     nullptr,
                     false,
                     true});
  return options;
}

// graphics.frame_rate_limit (frame_rate_policy.h/.cc, research/graphics.md
// 4.1).
GtkWidget* BuildFrameRateRow(LauncherContext* context,
                             GraphicsPageState* state) {
  RowSpec spec;
  spec.key = kFrameRateKey;
  spec.title = _("Frame rate limit");
  // runtime_config.cc: MOCKTAIL_FRAME_RATE_LIMIT defaults to -1; the
  // template leaves the key commented out.
  spec.fallback = "-1";
  spec.keywords = {"fps",       "frame rate",
                   "framerate", "frames",
                   "cap",       "limit",
                   "refresh",   "hz",
                   "кадры",     "фпс",
                   "частота",   "ограничение",
                   "герц",      "DFIntTaskSchedulerTargetFps"};
  spec.hint.details =
      _("How many frames per second Roblox aims for. A number here is passed "
        "to Roblox as its task scheduler target (DFIntTaskSchedulerTargetFps), "
        "so Roblox's in-game Maximum Frame Rate menu no longer decides. The "
        "change applies the next time Roblox starts.") +
      std::string("\n\n") +
      // frame_rate_policy.cc forces FFlagGameBasicSettingsFramerateCap5.
      _("• Set in Roblox: Mocktail sets no target, and Roblox's own Maximum "
        "Frame Rate menu, which Mocktail always turns on, sets the cap.") +
      "\n\n" +
      // frame_rate_policy.h: every positive value is forwarded verbatim.
      _("• A number: Roblox aims for exactly that many frames per second. "
        "“Match display” writes this screen's refresh rate as a number, so "
        "choose it again after moving to another screen.") +
      "\n\n" +
      // window.cc GetDisplayRefreshCapabilities advertises 240 Hz when
      // presentation is unthrottled.
      _("• 240 (maximum): Roblox's highest target. With Vulkan and Vertical "
        "sync on Automatic, frames are shown without waiting for the display, "
        "and Roblox is told the display runs at 240 Hz.") +
      "\n\n" +
      _("More frames make motion smoother and input quicker, at the cost of "
        "power, heat and fan noise. A target above the screen's refresh rate "
        "shows little difference, and a target the computer cannot reach "
        "changes nothing.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MonitorInfo& monitor = ctx.machine().monitor;
    if (!monitor.valid || monitor.RefreshHz() <= 0) return std::string();
    return monitor.connector.empty()
               ? Format(_("This screen refreshes at %d Hz."),
                        monitor.RefreshHz())
               : Format(_("This screen (%s) refreshes at %d Hz."),
                        monitor.connector.c_str(), monitor.RefreshHz());
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return RecommendFrameRate(machine.monitor);
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    const int refresh = machine.monitor.RefreshHz();
    if (refresh > 60) {
      return Format(_("Your screen refreshes %d times a second, so this "
                      "target gives it a new frame on every refresh."),
                    refresh);
    }
    return Format(_("Your screen refreshes %d times a second, which Roblox's "
                    "own menu already reaches."),
                  refresh);
  };
  spec.hint.warning = [state](LauncherContext&, const std::string& value) {
    if (const launcher::FastFlagsDocument* flags = state->fast_flags()) {
      const std::string conflict = FrameRateFlagConflict(*flags, value);
      if (!conflict.empty()) {
        return Format(_("fflags.json sets %s to another value, so Roblox will "
                        "not start. Remove that flag or choose “Set in "
                        "Roblox”."),
                      conflict.c_str());
      }
    }
    const std::optional<int> fps = ParseFrameRate(value);
    if (fps.has_value() && *fps > 240) {
      // frame_rate_policy.h kMaximumSupportedRobloxSchedulerFps.
      return std::string(
          _("Roblox's highest target is 240, so a larger number "
            "may not raise the frame rate."));
    }
    return std::string();
  };

  ComboSpec combo;
  combo.options_for = FrameRateOptions;
  combo.on_custom = [state](LauncherContext& ctx) {
    state->RequestCustomFrameRate();
    if (!ParseFrameRate(ctx.EffectiveValue(kFrameRateKey, "-1")).has_value()) {
      const int refresh = ctx.machine().monitor.RefreshHz();
      ctx.SetValue(kFrameRateKey, std::to_string(refresh > 0 ? refresh : 60),
                   launcher::ScalarKind::kInteger);
    }
    state->UpdateCustomRows();
    if (state->frame_rate_spin() != nullptr) {
      ctx.Reveal(state->frame_rate_spin());
    }
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

GtkWidget* BuildCustomFrameRateRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kFrameRateKey;
  spec.title = _("Custom frame rate");
  spec.fallback = "-1";
  spec.keywords = {"custom fps", "своя частота"};
  spec.hint.subtitle = _("Frames per second Roblox aims for");
  spec.hint.details =
      _("Any whole number of frames per second. Mocktail passes it to Roblox "
        "unchanged, without a list of allowed values.") +
      std::string("\n\n") +
      _("Pick the refresh rate of your screen, or a lower number to save "
        "power and heat. Roblox's own maximum is 240.");
  SpinSpec spin;
  spin.minimum = 1;
  spin.maximum = 1000;
  spin.step = 1;
  return BindSpinRow(context, std::move(spec), spin);
}

std::string VsyncAutoDescription(const LauncherContext& context) {
  switch (ResolvePresentation("auto",
                              context.EffectiveValue(kFrameRateKey, "-1"))) {
    case Presentation::kDriverDefault:
      return _("The driver's default, since Roblox owns the frame-rate cap");
    case Presentation::kSynchronized:
      return _("Synchronized, because a frame-rate target is set");
    case Presentation::kUnthrottled:
      return _(
          "Not synchronized, because the frame rate is set to the "
          "maximum");
  }
  return {};
}

// graphics.vsync (present_mode_policy.cc; research/graphics.md 1.5, 4.2).
GtkWidget* BuildVsyncRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kVsyncKey;
  spec.title = _("Vertical sync");
  // runtime_config.cc: MOCKTAIL_VSYNC defaults to auto; the template leaves
  // the key commented out.
  spec.fallback = "auto";
  spec.keywords = {"vsync",         "v-sync",       "tearing",   "latency",
                   "present",       "mailbox",      "immediate", "fifo",
                   "синхронизация", "вертикальная", "разрывы",   "задержка"};
  spec.hint.details =
      _("Decides whether Vulkan frames wait for the display's next refresh. "
        "Only the Vulkan backend uses it: with OpenGL ES and ANGLE the "
        "driver's default applies, which normally waits.") +
      std::string("\n\n") +
      _("• Automatic: synchronized when a frame-rate target is set, not "
        "synchronized with the 240 maximum, and the driver's default while "
        "Roblox owns the cap.") +
      "\n\n" +
      // present_mode_policy.cc FilterPresentModes.
      _("• On: frames wait for the display, using the newest finished frame "
        "when the driver allows it. No tearing, slightly more input "
        "latency.") +
      "\n\n" +
      // window.cc advertises 240 Hz to Roblox when unthrottled.
      _("• Off: frames are shown as soon as they are ready, for the lowest "
        "input latency. Roblox is also told the display runs at 240 Hz.") +
      "\n\n" +
      _("On Wayland a window only tears when the compositor allows it (on "
        "Hyprland, for example, allow_tearing); otherwise Off still shows "
        "whole frames, just with less waiting.");
  const auto describe = [](const LauncherContext& ctx,
                           const std::string& value) -> std::string {
    if (value == "on") {
      return _("Waits for the display: no tearing, a little more latency");
    }
    if (value == "off") {
      return _("Lowest input latency; may tear if the compositor allows it");
    }
    return VsyncAutoDescription(ctx);
  };
  spec.hint.subtitle_for = [describe](LauncherContext& ctx,
                                      const std::string& value) {
    if (!IsVulkanSpelling(
            ctx.EffectiveValue("graphics.backend", "direct-vulkan"))) {
      return std::string(
          _("No effect with this graphics backend; the driver decides"));
    }
    return describe(ctx, value);
  };
  spec.hint.warning = [](LauncherContext& ctx, const std::string& value) {
    if (value != "auto" && !IsVulkanSpelling(ctx.EffectiveValue(
                               "graphics.backend", "direct-vulkan"))) {
      return std::string(
          _("Only the Vulkan backend applies this; choose Vulkan or set "
            "Automatic."));
    }
    return std::string();
  };

  ComboSpec combo;
  combo.options = {
      {"auto",
       _("Automatic"),
       {},
       {},
       [describe](LauncherContext& ctx) { return describe(ctx, "auto"); },
       nullptr,
       false,
       false},
      // present_mode_policy.cc accepts 1/0 from the environment; config.yaml
      // only on/off.
      {"on",
       _("On"),
       {},
       {},
       [describe](LauncherContext& ctx) { return describe(ctx, "on"); },
       nullptr,
       false,
       false},
      {"off",
       _("Off"),
       {},
       {},
       [describe](LauncherContext& ctx) { return describe(ctx, "off"); },
       nullptr,
       false,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

}  // namespace

// Graphics: the backend, Roblox's quality level and the frame rate. The
// backend row is the reference for how hints are written.
GtkWidget* BuildGraphicsPage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kGraphics,
                            _("How Roblox draws the game: the graphics "
                              "backend, frame rate and quality"));
  auto* state = new GraphicsPageState(context);
  g_object_set_data_full(
      G_OBJECT(page), "mocktail-graphics-page", state,
      [](gpointer data) { delete static_cast<GraphicsPageState*>(data); });

  GtkWidget* renderer =
      AddGroup(page, _("Renderer"),
               _("Which graphics path Roblox uses on this computer"));
  AddRow(renderer, BuildGraphicsBackendRow(context));
  AddRow(renderer, BuildGraphicsQualityRow(context, state));
  GtkWidget* quality_level = BuildQualityLevelRow(context);
  state->set_quality_spin(quality_level);
  AddRow(renderer, quality_level);

  GtkWidget* frame_rate =
      AddGroup(page, _("Frame rate"),
               _("How many frames Roblox draws and when they reach the "
                 "screen"));
  AddRow(frame_rate, BuildFrameRateRow(context, state));
  GtkWidget* custom_frame_rate = BuildCustomFrameRateRow(context);
  state->set_frame_rate_spin(custom_frame_rate);
  AddRow(frame_rate, custom_frame_rate);
  AddRow(frame_rate, BuildVsyncRow(context));
  state->UpdateCustomRows();
  return page;
}

}  // namespace mocktail::launcher_ui
