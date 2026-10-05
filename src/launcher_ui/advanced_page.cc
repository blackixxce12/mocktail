#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launcher/config_document.h"
#include "launcher/desktop_entry_cleanup.h"
#include "launcher_ui/advanced_fast_flags.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/env_overrides.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/page_rules.h"
#include "launcher_ui/page_widgets.h"
#include "launcher_ui/pages.h"
#include "runtime/environment.h"

namespace mocktail::launcher_ui {
namespace {

// The detailed `device:` block the template documents; this window keeps
// it as it is (SPEC 5, research/config.md 1 "device").
bool DeviceIsMapping(const LauncherContext& context) {
  return launcher::ConfigDocument::FromBytes(context.draft().working_bytes())
      .IsMapping("device");
}

std::filesystem::path Home() {
  return runtime::ProcessEnvironment().GetOr("HOME", "");
}

// ---- device -----------------------------------------------------------------

GtkWidget* BuildDeviceRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "device";
  spec.title = _("Device profile");
  // runtime_config.h / device_profile.h: pc-windows-11 is the default.
  spec.fallback = "pc-windows-11";
  spec.keywords = {"device",  "pc",      "phone",     "mobile",
                   "console", "touch",   "gamepad",   "устройство",
                   "телефон", "консоль", "компьютер", "сенсорный"};
  spec.hint.recommend = [](const MachineProfile&) {
    return std::optional<std::string>("pc-windows-11");
  };
  spec.hint.recommend_reason = [](const MachineProfile&) {
    return std::string(
        _("A computer with a mouse and keyboard gets Roblox's "
          "full desktop interface."));
  };
  spec.hint.warning = [](LauncherContext&, const std::string& value) {
    const std::string preset = CanonicalDevicePreset(value);
    if (preset == "mobile-pixel-7") {
      return std::string(
          _("Mouse and keyboard are not reported to Roblox; "
            "without a touchscreen this is hard to use"));
    }
    if (preset == "console-ps5") {
      return std::string(_("Experimental in Mocktail"));
    }
    return std::string();
  };
  spec.hint.details =
      // research/graphics.md 6.1; device_profile.cc; runtime_config.cc
      // (user agent); main.cc SetPlatformIdentity.
      _("Roblox for Android adapts to the device it runs on. This profile "
        "decides which inputs Roblox expects (touch, mouse, keyboard), the "
        "device name and model it reports, and how its web requests "
        "identify the device.") +
      std::string("\n\n") +
      // main.cc ApplyDesktopAppPolicy runs for the PC class only.
      _("• PC (Windows 11): mouse and keyboard and Roblox's desktop layout. "
        "Mocktail's default.") +
      "\n\n" +
      _("• Phone (Google Pixel 7): Roblox's touch interface with on-screen "
        "buttons. Mouse and keyboard are not reported, so it suits a "
        "touchscreen.") +
      "\n\n" +
      _("• Console (PlayStation 5): Roblox treats you as a console player "
        "when deciding which experiences let you in, without the desktop "
        "layout; mouse and keyboard stay available. Experimental.") +
      "\n\n" +
      // platform_cache_migration.h: device changes drop PlayerHydrationBlob,
      // PlayerHydrationSignature and AppConfiguration from app storage.
      _("Switching clears the Roblox data that depends on the device at the "
        "next start; your sign-in stays.");
  ComboSpec combo;
  combo.options = {
      {"pc-windows-11",
       _("PC (Windows 11)"),
       _("Mouse and keyboard, Roblox's desktop layout"),
       {"pc"},
       nullptr,
       nullptr,
       false,
       false},
      {"mobile-pixel-7",
       _("Phone (Google Pixel 7)"),
       _("Touch interface; mouse and keyboard are not reported"),
       {"mobile"},
       nullptr,
       nullptr,
       false,
       false},
      {"console-ps5",
       _("Console (PlayStation 5)"),
       _("Console identity for experience access; experimental"),
       {"console", "console-xbox-series-x"},
       nullptr,
       nullptr,
       false,
       false},
  };
  GtkWidget* row = BindComboRow(context, std::move(spec), std::move(combo));
  gtk_widget_set_visible(row, !DeviceIsMapping(*context));
  FollowContext(context, row, [context, row] {
    gtk_widget_set_visible(row, !DeviceIsMapping(*context));
  });
  return row;
}

GtkWidget* BuildCustomDeviceRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                            NewRowButton(_("Open config.yaml"), [context] {
                              context->OpenPath(context->config_file());
                            }));
  RowSpec spec;
  spec.title = _("Device profile");
  spec.keywords = {"device", "custom device", "устройство"};
  spec.hint.subtitle = _("A custom profile, edited in config.yaml");
  spec.hint.details =
      _("config.yaml describes a device of its own (the detailed device: "
        "block with a name, model and inputs). This window does not edit it "
        "and keeps it exactly as it is.") +
      std::string("\n\n") +
      _("To use a preset again, replace the block with a single line such as "
        "device: pc-windows-11.");
  gtk_widget_set_visible(row, DeviceIsMapping(*context));
  FollowContext(context, row, [context, row] {
    gtk_widget_set_visible(row, DeviceIsMapping(*context));
  });
  return DecorateRow(context, row, std::move(spec));
}

// ---- launcher ---------------------------------------------------------------

std::shared_ptr<launcher::DesktopEntryInspection>& Inspection();
void InspectShortcut();

GtkWidget* BuildShowOnStartRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "launcher.show_on_start";
  spec.title = _("Show this window on start");
  spec.fallback = "true";
  spec.keywords = {"launcher",   "start",   "startup", "skip",         "--play",
                   "--launcher", "лаунчер", "запуск",  "окно настроек"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("Opens before Roblox when you start Mocktail "
                               "from its icon"))
               : std::string(_("Roblox starts directly; run mocktail "
                               "--launcher, or use the “Mocktail Settings” "
                               "shortcut action where your launcher offers "
                               "it, to come back"));
  };
  spec.hint.details =
      _("When on, starting Mocktail from its icon or with the mocktail "
        "command opens this window first, and Play starts Roblox.") +
      std::string("\n\n") +
      // launcher_policy.h DecideLauncher.
      _("Joining a game from the Roblox website never opens it, and neither "
        "do Mocktail's own update test runs.") +
      "\n\n" +
      // packaging/space.bigrat.mocktail.desktop actions; command_line.cc.
      // Tiling desktops and launchers such as rofi or fuzzel rarely list
      // desktop actions.
      _("When off, run mocktail --launcher to come back here. Desktops that "
        "show shortcut actions (right-click in GNOME, KDE and most docks) "
        "also offer “Mocktail Settings”, unless your own copy of the "
        "shortcut predates it. “Play now” (mocktail --play) always skips "
        "this window.") +
      "\n\n" +
      // launcher_policy.h ReadLauncherShowOnStart: a broken file gives true.
      _("If config.yaml has an error, the window opens anyway so you can fix "
        "it.");
  // A copy of the shortcut in ~/.local/share/applications hides the
  // packaged one, actions included.
  spec.hint.warning = [](LauncherContext&, const std::string& value) {
    if (value == "true") return std::string();
    if (Inspection() == nullptr) InspectShortcut();
    const std::shared_ptr<launcher::DesktopEntryInspection>& inspection =
        Inspection();
    if (inspection == nullptr || !inspection->found ||
        inspection->bytes.empty() ||
        inspection->bytes.find("[Desktop Action settings]") !=
            std::string::npos) {
      return std::string();
    }
    return Format(_("Your own copy of the Mocktail shortcut (%s) has no "
                    "“Mocktail Settings” action; run mocktail --launcher to "
                    "come back here."),
                  inspection->path.c_str());
  };
  return BindSwitchRow(context, std::move(spec));
}

// ---- environment variables --------------------------------------------------

std::string OverrideNames(const LauncherContext& context) {
  std::string names;
  for (const EnvOverride& entry : context.env().all()) {
    if (!names.empty()) names += ", ";
    names += entry.name;
  }
  return names;
}

GtkWidget* BuildVariablesRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  GtkWidget* details = NewRowButton(
      _("Details…"), [context] { context->ShowEnvironmentDialog(); });
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), details);
  gtk_widget_set_sensitive(details, !context->env().empty());
  RowSpec spec;
  spec.title = _("Overriding variables");
  spec.keywords = {"environment", "variables", "env",   "shortcut", "terminal",
                   "переменные",  "окружение", "ярлык", "терминал"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    if (context.env().empty()) {
      return std::string(_("None: every setting comes from config.yaml"));
    }
    if (context.ignoring_environment()) {
      return std::string(_("Moved into settings and left out of this launch"));
    }
    return Format(ngettext("%d variable overrides settings: %s",
                           "%d variables override settings: %s",
                           static_cast<int>(context.env().all().size())),
                  static_cast<int>(context.env().all().size()),
                  OverrideNames(context).c_str());
  };
  spec.hint.details =
      // runtime_config_file.h: the environment wins over YAML.
      _("Environment variables such as SDL_VIDEODRIVER or MOCKTAIL_VSYNC win "
        "over config.yaml, so a value set in a desktop shortcut or a terminal "
        "silently replaces the setting shown here. Rows they affect carry an "
        "ENV badge.") +
      std::string("\n\n") +
      _("Details lists each variable, its value (secrets hidden) and where it "
        "probably comes from.") +
      "\n\n" +
      _("To stop them for good, remove them where they are set: Clean Up "
        "below for the desktop shortcut, or your shell's profile.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildMoveRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                            NewRowButton(_("Move"), [context] {
                              context->MoveEnvironmentIntoSettings();
                            }));
  const auto visible = [context] {
    return !context->env().empty() && !context->ignoring_environment();
  };
  gtk_widget_set_visible(row, visible());
  FollowContext(context, row,
                [row, visible] { gtk_widget_set_visible(row, visible()); });
  RowSpec spec;
  spec.title = _("Move into settings");
  spec.keywords = {"import", "move", "environment", "перенести", "переменные"};
  spec.hint.subtitle =
      _("Keep their values in config.yaml and ignore them for this launch");
  spec.hint.details =
      // LauncherContext::MoveEnvironmentIntoSettings and the managed
      // environment importers; mocktail drops them on "play ignore-env".
      // LauncherContext::MoveEnvironmentIntoSettings ignores the variables
      // even when one cannot be imported.
      _("Copies each variable's value into the matching setting, so the "
        "settings show and control it, and leaves the variables out of this "
        "launch. A value that has no matching setting is only left out "
        "(Details lists them), so that setting falls back to config.yaml. "
        "The variables come back on the next start until you remove them "
        "from the shortcut or terminal.") +
      std::string("\n\n") + _("Save to keep the moved values.");
  spec.unavailable = [](LauncherContext& context) {
    return context.read_only() ? std::string(_("Fix config.yaml first"))
                               : std::string();
  };
  return DecorateRow(context, row, std::move(spec));
}

// The user's copy of the desktop entry, read again after a cleanup.
std::shared_ptr<launcher::DesktopEntryInspection>& Inspection() {
  static std::shared_ptr<launcher::DesktopEntryInspection> inspection;
  return inspection;
}

void InspectShortcut() {
  auto inspection = std::make_shared<launcher::DesktopEntryInspection>();
  std::string error;
  launcher::InspectDesktopEntry(
      launcher::DefaultDesktopEntryPaths(runtime::ProcessEnvironment()),
      inspection.get(), &error);
  Inspection() = std::move(inspection);
}

std::string ManagedAssignments(
    const launcher::DesktopEntryInspection& inspection) {
  std::string text;
  for (const launcher::EnvAssignment& assignment : inspection.env_assignments) {
    if (!assignment.managed) continue;
    if (!text.empty()) text += ", ";
    text += assignment.name + "=" +
            (assignment.sensitive ? std::string("***")
                                  : RedactEnvironmentValue(assignment.value));
  }
  return text;
}

void ConfirmShortcutCleanup(LauncherContext* context) {
  const std::shared_ptr<launcher::DesktopEntryInspection> inspection =
      Inspection();
  if (inspection == nullptr || context->window() == nullptr ||
      inspection->plan == launcher::DesktopEntryCleanupPlan::kNone) {
    return;
  }
  AdwDialog* dialog =
      adw_alert_dialog_new(_("Clean up the desktop shortcut?"), nullptr);
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  const std::string body =
      (inspection->plan == launcher::DesktopEntryCleanupPlan::kDeleteFile
           ? Format(_("Your copy of the Mocktail shortcut (%s) only adds these "
                      "variables. It will be removed so the installed shortcut "
                      "is used again. A backup is kept next to it."),
                    inspection->path.c_str())
           : Format(_("The variables that override settings are removed from "
                      "the command of %s. Everything else in it stays, and a "
                      "backup is kept next to it."),
                    inspection->path.c_str())) +
      "\n\n" + ManagedAssignments(*inspection) + "\n\n" +
      _("Move their values into settings first if you want to keep them.");
  adw_alert_dialog_set_body(alert, body.c_str());
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "clean",
                                 _("Clean _Up"), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "clean",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_signal_connect(
      dialog, "response::clean",
      G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
        auto* context = static_cast<LauncherContext*>(data);
        const std::shared_ptr<launcher::DesktopEntryInspection> inspection =
            Inspection();
        if (context->selftest() || inspection == nullptr) return;
        launcher::DesktopEntryApplyResult result;
        std::string error;
        if (!launcher::ApplyDesktopEntryCleanup(*inspection, {}, &result,
                                                &error)) {
          context->Toast(
              Format(_("The shortcut was not changed: %s"), error.c_str()));
        } else {
          context->Toast(
              _("Shortcut cleaned up; it applies from the next "
                "start"));
        }
        InspectShortcut();
        context->NotifySettingChanged("@desktop-shortcut");
      }),
      context);
  adw_dialog_present(dialog, GTK_WIDGET(context->window()));
}

GtkWidget* BuildShortcutRow(LauncherContext* context) {
  InspectShortcut();
  GtkWidget* row = adw_action_row_new();
  GtkWidget* clean = NewRowButton(
      _("Clean Up…"), [context] { ConfirmShortcutCleanup(context); });
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), clean);
  const auto update = [clean] {
    const auto& inspection = Inspection();
    gtk_widget_set_visible(
        clean,
        inspection != nullptr && inspection->found &&
            inspection->plan != launcher::DesktopEntryCleanupPlan::kNone);
  };
  update();
  FollowContext(context, row, update);
  RowSpec spec;
  spec.title = _("Desktop shortcut");
  spec.keywords = {"shortcut", ".desktop", "desktop entry", "exec",
                   "clean up", "ярлык",    "очистить"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string&) {
    const auto& inspection = Inspection();
    if (inspection == nullptr || !inspection->found) {
      return std::string(
          _("You use the installed shortcut; it sets no variables"));
    }
    if (inspection->plan != launcher::DesktopEntryCleanupPlan::kNone) {
      return Format(_("Your own copy sets %s"),
                    ManagedAssignments(*inspection).c_str());
    }
    if (!inspection->note.empty()) {
      return Format(_("Your own copy cannot be cleaned up here: %s"),
                    inspection->note.c_str());
    }
    return std::string(
        _("Your own copy sets no variables that override settings"));
  };
  spec.hint.details =
      // desktop_entry_cleanup.h.
      _("Desktop environments prefer your own copy of a shortcut "
        "(~/.local/share/applications/space.bigrat.mocktail.desktop) over the "
        "installed one. A copy made to add a variable such as "
        "SDL_VIDEODRIVER=wayland forces it on every start, website joins "
        "included, because the same shortcut opens roblox: links.") +
      std::string("\n\n") +
      _("Clean Up removes only the variables that override settings this "
        "window manages; others, such as a GPU switch or MANGOHUD, stay. When "
        "nothing else differs from the installed shortcut, your copy is "
        "deleted instead. A backup is kept next to it.") +
      "\n\n" +
      _("Move the values into settings first, or those settings fall back to "
        "what config.yaml says.");
  spec.hint.details_for = [](LauncherContext&) {
    const auto& inspection = Inspection();
    if (inspection == nullptr || !inspection->found) return std::string();
    return Format(_("Your copy: %s"), inspection->path.c_str());
  };
  return DecorateRow(context, row, std::move(spec));
}

// ---- files ------------------------------------------------------------------

GtkWidget* BuildFileRow(LauncherContext* context, const char* icon,
                        std::string title, std::filesystem::path path,
                        bool create_folder, std::string details,
                        std::vector<std::string> keywords) {
  GtkWidget* row = NewActionRow(icon, [context, path, create_folder] {
    if (create_folder) {
      std::error_code error;
      std::filesystem::create_directories(path, error);
    }
    context->OpenPath(path);
  });
  RowSpec spec;
  spec.title = std::move(title);
  spec.keywords = std::move(keywords);
  spec.hint.subtitle = DisplayPath(path, Home());
  spec.hint.details = std::move(details);
  return DecorateRow(context, row, std::move(spec));
}

}  // namespace

// Advanced (research/ux.md 4.3 ADVANCED, SPEC 5).
GtkWidget* BuildAdvancedPage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kAdvanced,
                            _("The device Roblox sees, Fast Flags, this window "
                              "and Mocktail's files"));

  GtkWidget* device = AddGroup(page, _("Device"),
                               _("Which kind of device Roblox thinks it runs "
                                 "on"));
  AddRow(device, BuildDeviceRow(context));
  AddRow(device, BuildCustomDeviceRow(context));

  GtkWidget* flags =
      AddGroup(page, _("Fast Flags"),
               _("Roblox engine switches for experiments; rarely needed"));
  AddRow(flags, BuildFastFlagsRow(context));

  GtkWidget* launcher =
      AddGroup(page, _("Settings window"), _("When this window opens"));
  AddRow(launcher, BuildShowOnStartRow(context));

  GtkWidget* environment = AddGroup(
      page, _("Environment variables"),
      _("Values from the shortcut or terminal that win over settings"));
  AddRow(environment, BuildVariablesRow(context));
  AddRow(environment, BuildMoveRow(context));
  AddRow(environment, BuildShortcutRow(context));

  const runtime::RuntimePaths& paths = context->paths();
  GtkWidget* files = AddGroup(page, _("Files"),
                              _("Where Mocktail keeps settings, Roblox and "
                                "logs"));
  AddRow(files,
         BuildFileRow(
             context, "document-open-symbolic", _("Settings file"),
             context->config_file(), false,
             // ConfigDocument: line edits keep comments; the one-time
             // .launcher-backup; LauncherContext's file monitor.
             _("config.yaml holds every setting in this window. Saving keeps "
               "your comments and layout, and before its first save the "
               "window keeps a copy as config.yaml.launcher-backup.") +
                 std::string("\n\n") +
                 _("If you edit it while this window is open, the window "
                   "notices and offers to reload it."),
             {"config", "config.yaml", "yaml", "конфиг", "файл настроек"}));
  AddRow(files,
         BuildFileRow(
             context, "folder-open-symbolic", _("Data folder"),
             paths.data_root(), true,
             // runtime_paths.cc: payloads, app data and auth live here; the
             // Android cache, downloads and shader caches under cache_root.
             Format(_("Roblox itself (the installed versions), its app data "
                      "and settings, and your saved sign-ins. Caches and "
                      "downloads are kept in %s."),
                    paths.cache_root().c_str()) +
                 std::string("\n\n") +
                 _("Do not share this folder: it holds your sign-in "
                   "sessions."),
             {"data", "folder", "данные", "папка"}));
  AddRow(files,
         BuildFileRow(context, "folder-open-symbolic", _("Logs folder"),
                      paths.logs_root(), true,
                      // FAQ.md; session_log.cc: latest.log and sessions/.
                      _("latest.log and one log per start in sessions/. Attach "
                        "latest.log when you report a problem."),
                      {"logs", "log", "latest.log", "логи", "журнал"}));
  return page;
}

}  // namespace mocktail::launcher_ui
