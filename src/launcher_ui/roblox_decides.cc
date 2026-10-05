#include "launcher_ui/roblox_decides.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "launcher/config_document.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/page_widgets.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"
#include "runtime/device_profile.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kPhysicsKey[] = "performance.physics_worker_mode";

constexpr RobloxOverrideKind kOverrideKinds[] = {
    RobloxOverrideKind::kGraphicsQuality, RobloxOverrideKind::kFrameRateTarget,
    RobloxOverrideKind::kStartMode,       RobloxOverrideKind::kTheme,
    RobloxOverrideKind::kRenderingLimits, RobloxOverrideKind::kMicrophone,
    RobloxOverrideKind::kDevice,
};

constexpr OverrideConflict kConflicts[] = {
    OverrideConflict::kHighLevelUnderPresetLimits,
    OverrideConflict::kRobloxSliderUnderPresetLimits,
};

constexpr RobloxOwnedSetting kOwnedSettings[] = {
    RobloxOwnedSetting::kGraphicsQuality,
    RobloxOwnedSetting::kFrameRate,
    RobloxOwnedSetting::kFullscreen,
    RobloxOwnedSetting::kTheme,
    RobloxOwnedSetting::kVolume,
    RobloxOwnedSetting::kOutputSwitch,
    RobloxOwnedSetting::kVoiceChat,
    RobloxOwnedSetting::kCameraSensitivity,
    RobloxOwnedSetting::kCameraMode,
    RobloxOwnedSetting::kChat,
};

// runtime_config.cc ParseGraphicsBackend: three spellings of direct Vulkan.
bool IsVulkanSpelling(std::string_view value) {
  return value == "direct-vulkan" || value == "vulkan" ||
         value == "native-vulkan";
}

// A detailed device: block, which the loader reads through device.type
// (runtime_config_file.cc). A scalar device: is never one.
bool DeviceIsMapping(const LauncherContext& context) {
  return !context.Value("device").has_value() &&
         launcher::ConfigDocument::FromBytes(context.draft().working_bytes())
             .IsMapping("device");
}

bool IsConsole(const RobloxOverride& entry) {
  const runtime::DeviceProfile* profile =
      runtime::FindDeviceProfile(entry.value);
  return profile != nullptr &&
         profile->device_class == runtime::DeviceClass::kConsole;
}

// The in-game setting an override takes over, as the overview names it.
std::string RobloxSideTitle(RobloxOverrideKind kind) {
  switch (kind) {
    case RobloxOverrideKind::kGraphicsQuality:
      return _("Graphics Quality slider");
    case RobloxOverrideKind::kRenderingLimits:
      return _("Anti-aliasing, textures and level of detail");
    case RobloxOverrideKind::kFrameRateTarget:
      return _("Maximum Frame Rate");
    case RobloxOverrideKind::kTheme:
      return _("Theme");
    case RobloxOverrideKind::kStartMode:
      return _("Fullscreen");
    case RobloxOverrideKind::kMicrophone:
      return _("Voice chat microphone");
    case RobloxOverrideKind::kDevice:
      return _("Device and interface");
  }
  return {};
}

std::string Summary(const RobloxOverride& entry) {
  switch (entry.kind) {
    case RobloxOverrideKind::kGraphicsQuality:
      return Format(_("Mocktail forces graphics quality level %d, so Roblox's "
                      "Graphics Quality slider has no effect"),
                    entry.number);
    case RobloxOverrideKind::kRenderingLimits:
      return _(
          "Mocktail's performance preset replaces Roblox's own choices "
          "for anti-aliasing, texture memory, level of detail, shadows "
          "and sound channels");
    case RobloxOverrideKind::kFrameRateTarget:
      // frame_rate_policy.cc: unlimited targets
      // kMaximumSupportedRobloxSchedulerFps.
      return entry.value == "unlimited"
                 ? Format(_("Mocktail sets Roblox's frame-rate target to %d "
                            "FPS, the most Roblox's menu offers"),
                          entry.number)
                 : Format(_("Mocktail sets Roblox's frame-rate target to %d "
                            "FPS"),
                          entry.number);
    case RobloxOverrideKind::kTheme:
      if (entry.value == "light") {
        return _("Every start saves Light as Roblox's theme");
      }
      if (entry.value == "dark") {
        return _("Every start saves Dark as Roblox's theme");
      }
      return _(
          "Every start saves your desktop's light or dark preference as "
          "Roblox's theme");
    case RobloxOverrideKind::kStartMode:
      return entry.value == "fullscreen"
                 ? std::string(_("Every start turns Roblox's Fullscreen "
                                 "setting on, to match the window"))
                 : std::string(_("Every start turns Roblox's Fullscreen "
                                 "setting off, to match the window"));
    case RobloxOverrideKind::kMicrophone:
      return _(
          "Roblox is refused the microphone, so voice chat cannot "
          "record");
    case RobloxOverrideKind::kDevice:
      // device_profile.h kRobloxConsoleAdmissionUserAgent; main.cc
      // ApplyDesktopAppPolicy for the PC class.
      return IsConsole(entry)
                 ? std::string(_("Roblox's servers treat you as a console "
                                 "player when deciding which experiences "
                                 "let you in"))
                 : std::string(_("Roblox shows its desktop interface instead "
                                 "of the layout its servers choose for "
                                 "Android"));
  }
  return {};
}

// What the override replaces and through what. `entry` picks between the
// device profiles; everything else is the same for every value.
std::string Details(const RobloxOverride& entry) {
  switch (entry.kind) {
    case RobloxOverrideKind::kGraphicsQuality:
      // performance_policy.cc MergePerformanceClientSettingsOverrides.
      return _(
          "Mocktail's performance preset sets "
          "FIntDebugFRMQualityLevelOverride, which replaces the level "
          "the Graphics Quality option in Roblox's settings would "
          "choose.");
    case RobloxOverrideKind::kRenderingLimits:
      // performance_policy.cc rendering_settings; the level is left out
      // only for manual.
      return _(
          "While the preset is on, Mocktail sets these Roblox client "
          "settings at every start, in place of what Roblox would choose "
          "itself: MSAA off (FIntDebugForceMSAASamples=1), a 128 MB "
          "texture budget (FIntRenderTextureTotalBudgetMB), a texture mip "
          "bias (FIntRenderTextureMipBias), low-end level of detail "
          "(FFlagRenderEnableLowEndLOD), one shadow-map mip "
          "(FIntTM2ShadowMapMaxMips) and at most 32 sounds at once "
          "(FIntMaxAudibleSoundChannels). Unless Graphics quality is set "
          "to “Roblox in-game slider”, it also forces the quality "
          "level.");
    case RobloxOverrideKind::kFrameRateTarget:
      // frame_rate_policy.cc sets DFIntTaskSchedulerTargetFps and
      // FFlagGameBasicSettingsFramerateCap5; which of the target and the
      // menu wins in the payload is unverified.
      return _(
          "DFIntTaskSchedulerTargetFps replaces the target that "
          "Roblox's Maximum Frame Rate menu would set. The menu stays in "
          "Roblox's settings, as Mocktail always turns it on "
          "(FFlagGameBasicSettingsFramerateCap5); whether a lower choice "
          "there still caps the game is not verified for this Roblox "
          "build.");
    case RobloxOverrideKind::kTheme:
      // legacy_runtime.cc -> platform_cache_migration.cc
      // ApplyRobloxThemeCacheOverride.
      return _(
          "Mocktail writes the theme into Roblox's local settings "
          "(AuthenticatedTheme, and DeviceLevelTheme for the signed-in "
          "account) before Roblox starts, so a theme picked in Roblox's "
          "own settings lasts only until the next start.");
    case RobloxOverrideKind::kStartMode:
      // window.cc ApplyConfiguredWindowStartMode and
      // MaybeSynchronizeRestoredFullscreenState.
      return _(
          "After the first frame Mocktail sets Roblox's own fullscreen "
          "setting to match the window, so the mode the last session "
          "ended in does not carry over. Roblox's fullscreen switch and "
          "F11 still work while you play.");
    case RobloxOverrideKind::kMicrophone:
      // roblox_permissions_bridge.cc; performance_policy.cc
      // MergeAudioCaptureClientSettingsOverrides.
      return _(
          "Mocktail answers Roblox's microphone permission request with "
          "a refusal, so voice chat cannot record even when it is on in "
          "Roblox's settings. DFFlagVoiceChatSkipPermissionCheckForTests "
          "stays off, so Roblox cannot skip that check.");
    case RobloxOverrideKind::kDevice:
      // device_profile.h, runtime_config.cc (the user agent);
      // roblox_desktop_app_policy.cc for the PC class.
      return IsConsole(entry)
                 ? std::string(_("The console profile identifies the game to "
                                 "Roblox's experience access check as a "
                                 "console (Roblox/XboxOne), without the "
                                 "desktop layout; mouse and keyboard stay "
                                 "available. It is experimental."))
                 : std::string(
                       _("The PC profile reports a Windows 11 "
                         "computer with mouse and keyboard, and "
                         "Mocktail rewrites the app policy Roblox "
                         "keeps for Android to its desktop layout "
                         "(FStringAppConfigurationOverrideAppPolicy)."));
  }
  return {};
}

// What this computer and the other settings add to the details.
std::string ExtraDetails(const RobloxOverride& entry,
                         const GameSettings& settings) {
  if (entry.kind == RobloxOverrideKind::kGraphicsQuality &&
      entry.value == "default" && settings.intel_integrated_vulkan) {
    // graphics_launch_policy.cc publishes MOCKTAIL_GRAPHICS_QUALITY=1.
    return _(
        "Level 1 is Mocktail's choice for Intel integrated graphics with "
        "Vulkan; other graphics get level 3.");
  }
  if (entry.kind == RobloxOverrideKind::kTheme && entry.value != "system") {
    // roblox_desktop_app_policy.cc ApplyDesktopAppPolicy (PC class only):
    // ForceTheme is light or dark for those modes.
    const runtime::DeviceProfile* profile = runtime::FindDeviceProfile(
        settings.device.empty() ? runtime::kDefaultDeviceProfileName
                                : std::string_view(settings.device));
    if (profile != nullptr &&
        profile->device_class == runtime::DeviceClass::kPc) {
      return _(
          "With the PC device profile, Light and Dark are also set as "
          "ForceTheme in the app policy Mocktail gives Roblox.");
    }
  }
  return {};
}

std::string GiveBack(const RobloxOverride& entry) {
  switch (entry.kind) {
    case RobloxOverrideKind::kGraphicsQuality:
      return _("choose “Roblox in-game slider” under Graphics quality.");
    case RobloxOverrideKind::kRenderingLimits:
      // performance_policy.cc: latency never merges the preset; auto only
      // with multithreaded rendering.
      return entry.key == kPhysicsKey
                 ? std::string(_("set Physics workers to Low latency, or to "
                                 "Automatic with Multithreaded rendering "
                                 "off."))
                 : std::string(_("turn Multithreaded rendering off, or set "
                                 "Physics workers to Low latency."));
    case RobloxOverrideKind::kFrameRateTarget:
      return _("choose “Set in Roblox” under Frame rate limit.");
    case RobloxOverrideKind::kTheme:
      return _("choose “From your Roblox account” under Theme.");
    case RobloxOverrideKind::kStartMode:
      return _(
          "choose “Last used mode” under Start in: every start then "
          "reopens the mode the last session ended in, Roblox's own "
          "switch included.");
    case RobloxOverrideKind::kMicrophone:
      return _("choose System default or a microphone under Microphone.");
    case RobloxOverrideKind::kDevice:
      return IsConsole(entry)
                 ? std::string(_("choose PC (Windows 11) or Phone (Google "
                                 "Pixel 7) under Device profile."))
                 : std::string(_("choose Phone (Google Pixel 7) under Device "
                                 "profile, the profile Roblox for Android "
                                 "expects; it suits a touchscreen."));
  }
  return {};
}

RobloxOverrideNote NoteFor(const RobloxOverride& entry,
                           const GameSettings& settings) {
  RobloxOverrideNote note;
  note.summary = Summary(entry);
  note.details = Details(entry);
  const std::string extra = ExtraDetails(entry, settings);
  if (!extra.empty()) note.details += " " + extra;
  note.give_back = GiveBack(entry);
  return note;
}

std::optional<RobloxOverride> FindKind(const GameSettings& settings,
                                       RobloxOverrideKind kind) {
  for (const RobloxOverride& entry : ResolveRobloxOverrides(settings)) {
    if (entry.kind == kind) return entry;
  }
  return std::nullopt;
}

// The first bound row of `key` (the combo before a number row of the same
// key), nullptr when there is none.
const RowRecord* BoundRow(const LauncherContext& context,
                          std::string_view key) {
  for (const RowRecord& record : context.rows()) {
    if (record.row != nullptr && record.key == key &&
        record.kind != RowKind::kAction) {
      return &record;
    }
  }
  return nullptr;
}

std::string OwnedTitle(RobloxOwnedSetting setting) {
  switch (setting) {
    case RobloxOwnedSetting::kVolume:
      return _("Volume");
    case RobloxOwnedSetting::kCameraSensitivity:
      return _("Camera sensitivity");
    case RobloxOwnedSetting::kCameraMode:
      return _("Camera mode");
    case RobloxOwnedSetting::kChat:
      return _("Chat");
    case RobloxOwnedSetting::kOutputSwitch:
      return _("Sound output while playing");
    case RobloxOwnedSetting::kGraphicsQuality:
      return _("Graphics Quality slider");
    case RobloxOwnedSetting::kFrameRate:
      return _("Maximum Frame Rate");
    case RobloxOwnedSetting::kTheme:
      return _("Theme");
    case RobloxOwnedSetting::kFullscreen:
      return _("Fullscreen");
    case RobloxOwnedSetting::kVoiceChat:
      return _("Voice chat microphone");
  }
  return {};
}

std::string OwnedSubtitle(RobloxOwnedSetting setting,
                          const GameSettings& settings) {
  switch (setting) {
    case RobloxOwnedSetting::kVolume:
      return _("Roblox's volume slider; Mocktail sets no volume");
    case RobloxOwnedSetting::kCameraSensitivity:
    case RobloxOwnedSetting::kCameraMode:
      return _("Roblox's own setting; nothing Mocktail sets changes it");
    case RobloxOwnedSetting::kChat:
      return _("Roblox's chat settings; nothing Mocktail sets changes them");
    case RobloxOwnedSetting::kOutputSwitch:
      // roblox_output_device_bridge.cc: Roblox's menu switches the SDL
      // route live; audio.output_device picks the device at start.
      return _(
          "Roblox's audio settings can switch it; Audio › Output device "
          "picks it at start");
    case RobloxOwnedSetting::kGraphicsQuality:
      return RenderingPresetActive(settings.physics_worker_mode,
                                   settings.multithreaded_rendering)
                 ? std::string(_("Roblox's slider picks the level; the "
                                 "performance preset's other limits stay"))
                 : std::string(_("Roblox's slider picks the level"));
    case RobloxOwnedSetting::kFrameRate:
      return _("Roblox's menu sets the cap; Mocktail sets no target");
    case RobloxOwnedSetting::kFullscreen:
      return _(
          "Roblox's switch; every start reopens the mode the last "
          "session ended in");
    case RobloxOwnedSetting::kTheme:
      return _("The theme saved in your Roblox settings");
    case RobloxOwnedSetting::kVoiceChat:
      return _("Roblox's voice chat settings decide whether it records");
  }
  return {};
}

std::string FlagTitle(AlwaysOnFlag flag) {
  switch (flag) {
    case AlwaysOnFlag::kFrameRateMenu:
      return _("Maximum Frame Rate menu");
    case AlwaysOnFlag::kAudioDeviceMenu:
      return _("In-game audio device menu");
    case AlwaysOnFlag::kMicrophonePermissionCheck:
      return _("Microphone permission check");
    case AlwaysOnFlag::kVideoMemory:
      return _("Video memory budget");
    case AlwaysOnFlag::kWebRequests:
      return _("Web requests");
    case AlwaysOnFlag::kCrashUploads:
      return _("Crash reports");
  }
  return {};
}

std::string MebibytesText(std::uint64_t bytes) {
  return Format(_("%d MiB"), static_cast<int>(bytes / (1024U * 1024U)));
}

std::string FlagReason(AlwaysOnFlag flag, const MachineProfile& machine) {
  switch (flag) {
    case AlwaysOnFlag::kFrameRateMenu:
      // frame_rate_policy.cc, for every frame_rate_limit value.
      return _("On, so Roblox always offers its own frame-rate menu");
    case AlwaysOnFlag::kAudioDeviceMenu:
      // performance_policy.cc MergeAudioDeviceMenuClientSettingsOverrides,
      // merged by main.cc when the build profile bridges input devices.
      return _(
          "Off, so the menu lists this computer's sound devices through "
          "Mocktail's bridge; set for the Roblox versions whose audio "
          "devices Mocktail bridges");
    case AlwaysOnFlag::kMicrophonePermissionCheck:
      return _("Off, so a disabled microphone stays disabled");
    case AlwaysOnFlag::kVideoMemory: {
      // texture_memory_policy.h: Roblox sizes the budget for the Android
      // guest (64 MiB); an explicit value wins.
      if (machine.memory_bytes == 0) {
        return _(
            "An eighth of this computer's memory, kept between 256 and "
            "1536 MiB, instead of the 64 MiB Roblox for Android assumes; "
            "a value in fflags.json wins");
      }
      const std::uint64_t budget = VideoMemoryBudgetBytes(machine.memory_bytes);
      if (budget == 0) {
        return _(
            "Left to Roblox: this computer has less than 4 GiB of "
            "memory");
      }
      return Format(_("%s here: an eighth of this computer's memory, kept "
                      "between 256 and 1536 MiB, instead of the 64 MiB Roblox "
                      "for Android assumes; a value in fflags.json wins"),
                    MebibytesText(budget).c_str());
    }
    case AlwaysOnFlag::kWebRequests:
      // http_client_policy.cc.
      return _(
          "Keep Roblox's web requests on code paths that work under "
          "Mocktail");
    case AlwaysOnFlag::kCrashUploads:
      // crash_report_policy.cc.
      return _("Roblox's crash and error uploads are off");
  }
  return {};
}

// "Graphics › Graphics quality": where the row that decides it lives.
std::string RowPlace(const LauncherContext& context, std::string_view key) {
  const RowRecord* record = BoundRow(context, key);
  if (record == nullptr) return {};
  return std::string(_(GetSectionInfo(record->section).title)) + " › " +
         record->title;
}

// Shows `row` while `visible` says so, after every change.
void FollowVisibility(LauncherContext* context, GtkWidget* row,
                      std::function<bool()> visible) {
  gtk_widget_set_visible(row, visible());
  FollowContext(context, row,
                [row, visible] { gtk_widget_set_visible(row, visible()); });
}

GtkWidget* PrefixIcon(GtkWidget* row, const char* icon_name) {
  GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
  gtk_accessible_update_state(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_STATE_HIDDEN,
                              TRUE, -1);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
  return icon;
}

// The rows of the group that can be shown, in order; the link row reveals
// the first visible one. One settings window per process.
std::vector<GtkWidget*>* g_group_rows = nullptr;

GtkWidget* BuildOverrideRow(LauncherContext* context, RobloxOverrideKind kind) {
  GtkWidget* row = NewActionRow("go-next-symbolic", [context, kind] {
    const std::optional<RobloxOverride> entry =
        FindKind(CurrentGameSettings(*context), kind);
    if (!entry.has_value()) return;
    if (const RowRecord* record = BoundRow(*context, entry->key);
        record != nullptr && gtk_widget_get_visible(record->row)) {
      context->Reveal(record->row);
    }
  });
  PrefixIcon(row, "input-gaming-symbolic");
  RowSpec spec;
  spec.title = RobloxSideTitle(kind);
  spec.keywords = {"roblox",      "override", "in-game", "decides",
                   "перекрывает", "в игре",   "решает"};
  spec.hint.subtitle_for = [kind](LauncherContext& ctx, const std::string&) {
    const std::optional<RobloxOverride> entry =
        FindKind(CurrentGameSettings(ctx), kind);
    if (!entry.has_value()) return std::string();
    std::string text = Summary(*entry);
    const std::string place = RowPlace(ctx, entry->key);
    if (!place.empty()) {
      text += "\n" + Format(_("Decided by %s"), place.c_str());
    }
    return text;
  };
  RobloxOverride sample;
  sample.kind = kind;
  sample.value = std::string(runtime::kDefaultDeviceProfileName);
  spec.hint.details =
      kind == RobloxOverrideKind::kDevice
          ? std::string(
                _("The device profile decides which interface Roblox shows "
                  "and how Roblox's servers see the device: the PC profile "
                  "gets the desktop layout, the console profile a console's "
                  "access to experiences. Only the Phone profile leaves both "
                  "to Roblox for Android."))
          : Details(sample);
  spec.hint.details_for = [kind](LauncherContext& ctx) {
    const GameSettings settings = CurrentGameSettings(ctx);
    const std::optional<RobloxOverride> entry = FindKind(settings, kind);
    if (!entry.has_value()) return std::string();
    std::string text;
    if (kind == RobloxOverrideKind::kDevice) text = Details(*entry);
    const std::string extra = ExtraDetails(*entry, settings);
    if (!extra.empty()) text += (text.empty() ? "" : " ") + extra;
    if (!text.empty()) text += "\n\n";
    text +=
        Format(_("To give Roblox control back: %s"), GiveBack(*entry).c_str());
    return text;
  };
  DecorateRow(context, row, std::move(spec));
  FollowVisibility(context, row, [context, kind] {
    return FindKind(CurrentGameSettings(*context), kind).has_value();
  });
  return row;
}

GtkWidget* BuildNothingRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  PrefixIcon(row, "input-gaming-symbolic");
  RowSpec spec;
  spec.title = _("Nothing is taken over");
  spec.hint.subtitle =
      _("Roblox's own settings decide everything Mocktail could take over");
  spec.hint.details =
      _("With the current settings no row in this window replaces one of "
        "Roblox's own settings: the performance preset is off, Roblox owns "
        "the frame rate, the fullscreen setting and the theme, the "
        "microphone is enabled, and the Phone profile leaves the interface "
        "to Roblox for Android.");
  DecorateRow(context, row, std::move(spec));
  FollowVisibility(context, row, [context] {
    return ResolveRobloxOverrides(CurrentGameSettings(*context)).empty();
  });
  return row;
}

GtkWidget* BuildConflictRow(LauncherContext* context,
                            OverrideConflict conflict) {
  GtkWidget* row = NewActionRow("go-next-symbolic", [context] {
    if (const RowRecord* record = BoundRow(*context, kPhysicsKey)) {
      context->Reveal(record->row);
    }
  });
  GtkWidget* icon = PrefixIcon(row, "dialog-warning-symbolic");
  gtk_widget_add_css_class(icon, "warning");
  RowSpec spec;
  spec.title = _("Other preset limits remain");
  spec.keywords = {"conflict", "msaa",     "textures", "preset",
                   "конфликт", "текстуры", "пресет"};
  spec.hint.subtitle_for = [](LauncherContext& ctx, const std::string&) {
    return Format(_("Decided by %s"), RowPlace(ctx, kPhysicsKey).c_str());
  };
  spec.hint.warning = [conflict](LauncherContext& ctx, const std::string&) {
    return DescribeOverrideConflict(conflict, CurrentGameSettings(ctx));
  };
  spec.hint.details =
      // performance_policy.cc: the level and the rest of the preset are
      // separate; manual leaves out only the level.
      _("Graphics quality only sets the level. The rest of the performance "
        "preset (MSAA off, a 128 MB texture budget, low-end level of detail, "
        "simpler shadows) stays while Physics workers is on Throughput, or on "
        "Automatic with Multithreaded rendering, whatever level Roblox "
        "runs at.");
  DecorateRow(context, row, std::move(spec));
  FollowVisibility(context, row, [context, conflict] {
    const std::vector<OverrideConflict> conflicts =
        FindOverrideConflicts(CurrentGameSettings(*context));
    return std::find(conflicts.begin(), conflicts.end(), conflict) !=
           conflicts.end();
  });
  return row;
}

GtkWidget* BuildOwnedRow(LauncherContext* context) {
  GtkWidget* expander = adw_expander_row_new();
  for (const RobloxOwnedSetting setting : kOwnedSettings) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  OwnedTitle(setting).c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
    const auto update = [context, row, setting] {
      const GameSettings settings = CurrentGameSettings(*context);
      const std::vector<RobloxOwnedSetting> owned =
          RobloxOwnedSettings(settings);
      const bool visible =
          std::find(owned.begin(), owned.end(), setting) != owned.end();
      gtk_widget_set_visible(row, visible);
      if (visible) {
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            Markup(OwnedSubtitle(setting, settings)).c_str());
      }
    };
    update();
    FollowContext(context, row, update);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
  }
  RowSpec spec;
  spec.title = _("Left to Roblox");
  spec.keywords = {"volume",    "camera", "sensitivity",      "chat",
                   "громкость", "камера", "чувствительность", "чат"};
  spec.hint.subtitle_for = [](LauncherContext& ctx, const std::string&) {
    const int count =
        static_cast<int>(RobloxOwnedSettings(CurrentGameSettings(ctx)).size());
    return Format(ngettext("%d of Roblox's own settings stays in its hands",
                           "%d of Roblox's own settings stay in its hands",
                           static_cast<unsigned long>(count)),
                  count);
  };
  spec.hint.details =
      _("Settings in Roblox's own menus that nothing in this window "
        "replaces with the current settings: what you choose there "
        "stays.") +
      std::string("\n\n") +
      _("Volume, camera and chat are never touched. The graphics level, the "
        "frame rate, fullscreen, the theme and the voice chat microphone are "
        "listed here only while no row above takes them over.");
  return DecorateRow(context, expander, std::move(spec));
}

GtkWidget* BuildAlwaysOnRow(LauncherContext* context) {
  GtkWidget* expander = adw_expander_row_new();
  std::vector<std::string> keywords = {"flags", "fflags", "client settings",
                                       "флаги", "always"};
  for (const AlwaysOnFlagInfo& info : AlwaysOnFlags()) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  FlagTitle(info.flag).c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
    const AlwaysOnFlag flag = info.flag;
    const std::string names(info.names);
    const auto update = [context, row, flag, names] {
      std::string shown = names;
      if (flag == AlwaysOnFlag::kVideoMemory) {
        // texture_memory_policy.cc writes the budget in bytes.
        const std::uint64_t budget =
            VideoMemoryBudgetBytes(context->machine().memory_bytes);
        if (budget != 0) shown += "=" + std::to_string(budget);
      }
      adw_action_row_set_subtitle(
          ADW_ACTION_ROW(row),
          (Markup(FlagReason(flag, context->machine())) +
           "\n<span size=\"small\" font_family=\"monospace\">" + Markup(shown) +
           "</span>")
              .c_str());
    };
    update();
    FollowContext(context, row, update);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
    // The client settings' names find the row in search.
    std::size_t start = 0;
    while (start < names.size()) {
      std::size_t end = names.find(", ", start);
      if (end == std::string::npos) end = names.size();
      const std::string name = names.substr(start, end - start);
      if (!name.empty() && g_ascii_isalpha(name.front())) {
        keywords.push_back(name.substr(0, name.find('=')));
      }
      start = end + 2;
    }
  }
  RowSpec spec;
  spec.title = _("Always set by Mocktail");
  spec.keywords = std::move(keywords);
  spec.hint.subtitle =
      _("Roblox client settings Mocktail sets at every start, whatever the "
        "settings say");
  spec.hint.details =
      _("These keep Roblox for Android working on a computer. No row in this "
        "window changes them, and Roblox has no menu for them.") +
      std::string("\n\n") +
      // main.cc merges fflags.json first, then the policies:
      // frame_rate_policy.cc refuses a different value of its flag,
      // http_client_policy.cc, crash_report_policy.cc and
      // performance_policy.cc (audio menu, microphone check) overwrite theirs,
      // and texture_memory_policy.cc keeps a value that is already there.
      _("fflags.json cannot change them: Mocktail replaces its values for web "
        "requests, crash reports, the audio device menu and the microphone "
        "check, and refuses to start when it sets the frame-rate menu flag "
        "to another value. Only a video memory budget in fflags.json wins "
        "over Mocktail's.");
  return DecorateRow(context, expander, std::move(spec));
}

}  // namespace

GameSettings CurrentGameSettings(const LauncherContext& context) {
  GameSettings settings;
  // The runtime's defaults where the template leaves a key out
  // (runtime_config.cc).
  settings.graphics_quality =
      context.GameValue("engine.graphics_quality", "default");
  settings.physics_worker_mode = context.GameValue(kPhysicsKey, "throughput");
  settings.multithreaded_rendering =
      context.GameValue("performance.multithreaded_rendering", "false");
  settings.frame_rate_limit =
      context.GameValue("graphics.frame_rate_limit", "-1");
  settings.theme = context.GameValue("appearance.theme", "roblox");
  settings.start_mode = context.GameValue("display.start_mode", "remember");
  settings.input_device = context.GameValue("audio.input_device", "default");
  settings.device = context.GameValue(
      "device", std::string(runtime::kDefaultDeviceProfileName));
  // MOCKTAIL_DEVICE_PROFILE wins over a detailed block as it does over a
  // scalar (runtime_config.cc has_explicit_device).
  if (context.EffectiveOverride("device") == nullptr &&
      DeviceIsMapping(context)) {
    settings.device = context.EffectiveValue("device.type");
  }
  const MachineProfile& machine = context.machine();
  settings.intel_integrated_vulkan =
      machine.detected &&
      IsVulkanSpelling(
          context.GameValue("graphics.backend", "direct-vulkan")) &&
      machine.RendersOnIntelIntegratedGraphics(
          context.GameValue("engine.gpu", "auto"));
  return settings;
}

std::function<std::optional<RobloxOverrideNote>(LauncherContext& context)>
RobloxOverrideHint(std::string key) {
  return [key = std::move(key)](
             LauncherContext& context) -> std::optional<RobloxOverrideNote> {
    const GameSettings settings = CurrentGameSettings(context);
    const std::optional<RobloxOverride> entry = RobloxOverrideOf(settings, key);
    if (!entry.has_value()) return std::nullopt;
    return NoteFor(*entry, settings);
  };
}

std::string DescribeOverrideConflict(OverrideConflict conflict,
                                     const GameSettings& settings) {
  switch (conflict) {
    case OverrideConflict::kHighLevelUnderPresetLimits: {
      const std::optional<RobloxOverride> quality =
          RobloxOverrideOf(settings, "engine.graphics_quality");
      return Format(_("Level %d is forced, but the performance preset still "
                      "keeps MSAA off, textures at 128 MB and low-end level "
                      "of detail, so the higher level gains less than it "
                      "would. Set Physics workers to Low latency to lift "
                      "those limits."),
                    quality.has_value() ? quality->number : 0);
    }
    case OverrideConflict::kRobloxSliderUnderPresetLimits:
      return _(
          "Roblox's slider picks the level, but the performance preset "
          "still keeps MSAA off, textures at 128 MB and low-end level of "
          "detail. Set Physics workers to Low latency to lift those "
          "limits.");
  }
  return {};
}

void AddRobloxDecidesGroup(LauncherContext* context, GtkWidget* page) {
  GtkWidget* group =
      AddGroup(page, _("What Mocktail decides and what Roblox decides"),
               _("Roblox's own settings that the settings in this window take "
                 "over at the next start; select one to open the row that "
                 "decides it"));
  auto* rows = new std::vector<GtkWidget*>();
  const auto add = [group, rows](GtkWidget* row) {
    AddRow(group, row);
    rows->push_back(row);
  };
  for (const RobloxOverrideKind kind : kOverrideKinds) {
    add(BuildOverrideRow(context, kind));
  }
  add(BuildNothingRow(context));
  for (const OverrideConflict conflict : kConflicts) {
    add(BuildConflictRow(context, conflict));
  }
  add(BuildOwnedRow(context));
  add(BuildAlwaysOnRow(context));
  g_group_rows = rows;
  g_object_set_data_full(
      G_OBJECT(group), "mocktail-roblox-decides", rows, [](gpointer data) {
        auto* list = static_cast<std::vector<GtkWidget*>*>(data);
        if (g_group_rows == list) g_group_rows = nullptr;
        delete list;
      });
}

GtkWidget* BuildRobloxDecidesLinkRow(LauncherContext* context) {
  GtkWidget* row = NewActionRow("go-next-symbolic", [context] {
    if (g_group_rows == nullptr) return;
    for (GtkWidget* candidate : *g_group_rows) {
      if (gtk_widget_get_visible(candidate)) {
        context->Reveal(candidate);
        return;
      }
    }
  });
  PrefixIcon(row, "input-gaming-symbolic");
  RowSpec spec;
  spec.title = _("What Mocktail decides and what Roblox decides");
  spec.keywords = {"roblox", "in-game", "override",   "decides",
                   "в игре", "решает",  "перекрывает"};
  spec.hint.subtitle_for = [](LauncherContext& ctx, const std::string&) {
    const int count = static_cast<int>(
        ResolveRobloxOverrides(CurrentGameSettings(ctx)).size());
    if (count == 0) {
      return std::string(
          _("Roblox's own settings decide everything Mocktail "
            "could take over"));
    }
    return Format(ngettext("Mocktail takes over %d of Roblox's own settings "
                           "now; see which on the Advanced page",
                           "Mocktail takes over %d of Roblox's own settings "
                           "now; see which on the Advanced page",
                           static_cast<unsigned long>(count)),
                  count);
  };
  spec.hint.details =
      _("Opens the overview on the Advanced page: which of Roblox's own "
        "settings the current settings take over and which row does it, "
        "what stays in Roblox's hands, and the client settings Mocktail sets "
        "at every start.") +
      std::string("\n\n") +
      // performance_policy.cc: the preset follows these two rows.
      _("On this page, Physics workers and Multithreaded rendering decide "
        "whether the performance preset replaces Roblox's choices for "
        "anti-aliasing, textures, level of detail and the graphics quality "
        "level.");
  return DecorateRow(context, row, std::move(spec));
}

}  // namespace mocktail::launcher_ui
