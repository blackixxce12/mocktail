#include <clocale>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/env_overrides.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/network_updates_status.h"
#include "launcher_ui/page_dialogs.h"
#include "launcher_ui/page_rules.h"
#include "launcher_ui/page_widgets.h"
#include "launcher_ui/pages.h"
#include "runtime/environment.h"
#include "runtime/session_log.h"

#ifndef MOCKTAIL_PROJECT_VERSION
#define MOCKTAIL_PROJECT_VERSION "unknown"
#endif
#ifndef MOCKTAIL_BUILD_GIT_COMMIT
#define MOCKTAIL_BUILD_GIT_COMMIT ""
#endif

namespace mocktail::launcher_ui {
namespace {

// Rows showing the session header refresh on this pseudo key.
constexpr char kSystemKey[] = "@about-system";

std::string ShortCommit() {
  std::string commit = MOCKTAIL_BUILD_GIT_COMMIT;
  if (commit == "unknown") commit.clear();
  return commit.substr(0, 12);
}

// The session log's header (version, kernel, CPU, RAM, GPU names and
// drivers; session_log.cc), which the game writes at every start. Built on
// a worker: naming the GPUs reads the PCI ID database.
std::string& SessionHeader() {
  static std::string header;
  return header;
}

std::string BuildSessionHeader(const LauncherContext& context) {
  return TrimSessionHeader(runtime::SessionLog().Header(
      runtime::ProcessEnvironment(), context.paths(),
      context.EffectiveValue("graphics.backend", "direct-vulkan")));
}

void StartSessionHeader(LauncherContext* context) {
  struct Request {
    LauncherContext* context;
    runtime::RuntimePaths paths;
    std::string backend;
    std::string header;
  };
  auto* request =
      new Request{context,
                  context->paths(),
                  context->EffectiveValue("graphics.backend", "direct-vulkan"),
                  {}};
  GTask* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        auto* done = static_cast<Request*>(
            g_task_propagate_pointer(G_TASK(result), nullptr));
        if (done == nullptr) return;
        SessionHeader() = std::move(done->header);
        done->context->NotifySettingChanged(kSystemKey);
        delete done;
      },
      nullptr);
  g_task_set_task_data(task, request, nullptr);
  g_task_run_in_thread(
      task, [](GTask* running, gpointer, gpointer data, GCancellable*) {
        auto* work = static_cast<Request*>(data);
        work->header = TrimSessionHeader(runtime::SessionLog().Header(
            runtime::ProcessEnvironment(), work->paths, work->backend));
        g_task_return_pointer(running, work, nullptr);
      });
  g_object_unref(task);
}

std::string HeaderField(const char* key) {
  const std::string value = SessionHeaderField(SessionHeader(), key);
  return value == "unknown" ? std::string() : value;
}

std::string SessionLabel(SessionType session) {
  switch (session) {
    case SessionType::kWayland:
      return "Wayland";
    case SessionType::kX11:
      return "X11";
    case SessionType::kUnknown:
      break;
  }
  return {};
}

std::string MonitorText(const MonitorInfo& monitor) {
  if (!monitor.valid) return {};
  std::string text =
      Format(_("%d × %d at %d %% (%d × %d pixels)"), monitor.width,
             monitor.height, static_cast<int>(monitor.scale * 100.0 + 0.5),
             monitor.PixelWidth(), monitor.PixelHeight());
  if (monitor.RefreshHz() > 0) {
    text += " · " + Format(_("%d Hz"), monitor.RefreshHz());
  }
  if (!monitor.connector.empty()) text += " · " + monitor.connector;
  return text;
}

// ---- diagnostics ------------------------------------------------------------

// Plain English for bug reports, built only from what is listed here: no
// sign-in data, no variable values, no proxy host, paths or free texts
// (DiagnosticSettingKeys).
std::string DiagnosticText(LauncherContext& context) {
  if (SessionHeader().empty()) SessionHeader() = BuildSessionHeader(context);
  const MachineProfile& machine = context.machine();
  std::vector<std::pair<std::string, std::string>> lines;
  lines.emplace_back("Settings window", MOCKTAIL_PROJECT_VERSION);
  lines.emplace_back("Session", SessionLabel(machine.session));
  lines.emplace_back("Desktop", machine.desktop);
  lines.emplace_back("Wayland available",
                     machine.wayland_available ? "yes" : "no");
  lines.emplace_back("X11 available", machine.x11_available ? "yes" : "no");
  lines.emplace_back("Flatpak", machine.flatpak ? "yes" : "no");
  lines.emplace_back("GPU vendors", machine.GpuVendorsLabel());
  lines.emplace_back("NVIDIA kernel driver",
                     machine.gpu.nvidia_kernel_driver ? "yes" : "no");
  lines.emplace_back(
      "Vulkan driver",
      std::filesystem::path(machine.vulkan_icd).filename().string());
  lines.emplace_back("ANGLE", machine.angle.has_value()
                                  ? machine.angle->label
                                  : std::string("none found"));
  lines.emplace_back("GameMode library",
                     machine.gamemode_library ? "yes" : "no");
  lines.emplace_back(
      "Monitor",
      machine.monitor.valid
          ? Format("%dx%d logical, scale %.3g, %d Hz, %s",
                   machine.monitor.width, machine.monitor.height,
                   machine.monitor.scale, machine.monitor.RefreshHz(),
                   machine.monitor.connector.c_str())
          : std::string());
  lines.emplace_back("CPU cores / threads",
                     std::to_string(machine.physical_cores) + " / " +
                         std::to_string(machine.logical_cpus));
  const char* locale = std::setlocale(LC_MESSAGES, nullptr);
  lines.emplace_back("Language", locale != nullptr ? locale : "");
  const RobloxStatus* status = RobloxStatus::For(&context);
  if (status->installed_state() == RobloxStatus::Installed::kReady) {
    const InstalledRoblox& installed = status->installed();
    lines.emplace_back("Roblox", installed.installed
                                     ? installed.version_name + " (" +
                                           installed.version_code +
                                           "), library " + installed.build_id
                                     : std::string("not installed"));
  }
  std::string variables;
  for (const EnvOverride& entry : context.env().all()) {
    if (!variables.empty()) variables += ", ";
    variables += entry.name;
  }
  if (!variables.empty() && context.ignoring_environment()) {
    variables += " (moved into settings)";
  }
  lines.emplace_back("Overriding variables", variables);

  std::string text =
      "Mocktail diagnostic info (no sign-in data, no variable values)\n\n" +
      SessionHeader() + "\n" + FormatDiagnosticLines(lines) + "\nSettings\n";
  std::vector<std::pair<std::string, std::string>> settings;
  for (const std::string_view key : DiagnosticSettingKeys()) {
    std::string value = context.EffectiveValue(key);
    if (value.empty()) value = "(default)";
    if (const EnvOverride* env = context.EffectiveOverride(key)) {
      value += " [overridden by " + env->name + "]";
    }
    settings.emplace_back(std::string(key), value);
  }
  return text + FormatDiagnosticLines(settings);
}

void CopyDiagnostics(LauncherContext* context) {
  if (context->window() == nullptr) return;
  const std::string text = DiagnosticText(*context);
  gdk_clipboard_set_text(
      gtk_widget_get_clipboard(GTK_WIDGET(context->window())), text.c_str());
  context->Toast(_("Diagnostic info copied"));
}

// ---- rows -------------------------------------------------------------------

GtkWidget* BuildVersionRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = "Mocktail";
  spec.keywords = {"version",  "commit", "build",
                   "mocktail", "версия", "сборка"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string&) {
    const std::string commit = ShortCommit();
    return commit.empty() ? Format(_("Version %s"), MOCKTAIL_PROJECT_VERSION)
                          : Format(_("Version %s · commit %s"),
                                   MOCKTAIL_PROJECT_VERSION, commit.c_str());
  };
  spec.hint.details =
      _("The version of Mocktail and of this settings window. Mention it, "
        "together with Copy diagnostic info below, when you report a "
        "problem.") +
      std::string("\n\n") +
      // CMakeLists.txt: MOCKTAIL_BUILD_GIT_COMMIT.
      _("The commit names the exact source code this build was made from.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildCreditsRow(LauncherContext* context) {
  GtkWidget* row =
      NewActionRow("go-next-symbolic", [context] { OpenAboutDialog(context); });
  RowSpec spec;
  spec.title = _("License & credits");
  spec.keywords = {"license", "apache",   "credits", "authors",
                   "website", "лицензия", "авторы",  "сайт"};
  spec.hint.subtitle = _("Apache License 2.0 · Mocktail Project Authors");
  // README.md and the metainfo file.
  spec.hint.details =
      _("Mocktail is free software under the Apache License 2.0, made by the "
        "Mocktail Project Authors.") +
      std::string("\n\n") +
      _("It is an independent community project, not affiliated with Roblox "
        "Corporation or VinegarHQ, and it does not include the Roblox client: "
        "Roblox is downloaded on first start.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildSessionRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Desktop");
  spec.keywords = {"session",  "wayland",    "x11",
                   "xwayland", "compositor", "desktop",
                   "сессия",   "композитор", "рабочий стол"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    const MachineProfile& machine = context.machine();
    if (!machine.detected) return std::string(_("Looking…"));
    std::string text = SessionLabel(machine.session);
    if (text.empty()) text = _("Unknown session");
    if (!machine.desktop.empty()) text += " · " + machine.desktop;
    if (machine.session == SessionType::kWayland) {
      text += " · ";
      text +=
          machine.x11_available ? _("XWayland available") : _("no XWayland");
    }
    return text;
  };
  spec.hint.details =
      _("The display system and desktop this window runs in. The game window "
        "can use Wayland or, through XWayland, X11; the Display page decides "
        "which.") +
      std::string("\n\n") +
      // video_driver_policy.cc rule 4.
      _("On NVIDIA with Vulkan the automatic choice is X11 through XWayland, "
        "because NVIDIA's native Wayland path can hang.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildGraphicsRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Graphics card");
  spec.keywords = {"gpu",    "graphics card", "nvidia",     "amd",    "intel",
                   "vulkan", "driver",        "видеокарта", "драйвер"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    const MachineProfile& machine = context.machine();
    std::string text = HeaderField("gpu");
    const std::string driver = HeaderField("gpu_driver");
    if (text.empty()) text = machine.GpuVendorsLabel();
    if (text.empty()) {
      return std::string(machine.detected ? _("No graphics card found")
                                          : _("Looking…"));
    }
    if (!driver.empty()) text += " (" + driver + ")";
    if (machine.detected) {
      text += "\n";
      text += machine.has_vulkan_driver()
                  ? Format(_("Vulkan driver: %s"),
                           std::filesystem::path(machine.vulkan_icd)
                               .filename()
                               .c_str())
                  : std::string(_("No Vulkan driver found"));
    }
    return text;
  };
  spec.hint.details =
      // session_log.cc DetectGraphicsHardware; machine_profile.cc.
      _("The graphics cards in /sys/class/drm with their kernel drivers, "
        "named the way Mocktail's session log names them, and the Vulkan "
        "driver Roblox would use.") +
      std::string("\n\n") +
      _("The Graphics page recommends a graphics backend from this.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildDisplayRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Monitor");
  spec.keywords = {"monitor", "screen", "resolution", "refresh", "scale",
                   "монитор", "экран",  "разрешение", "масштаб"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    const std::string text = MonitorText(context.machine().monitor);
    return text.empty() ? std::string(_("Not known yet")) : text;
  };
  spec.hint.details =
      _("The monitor this window is on, in desktop units and in pixels, with "
        "its scale and refresh rate.") +
      std::string("\n\n") +
      _("The Display page uses it to explain the window size and native "
        "resolution, and the Graphics page for “match display” frame rates.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildProcessorRow(LauncherContext* context) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Processor and memory");
  spec.keywords = {"cpu",    "processor", "cores", "threads", "ram",
                   "memory", "процессор", "ядра",  "память"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    const MachineProfile& machine = context.machine();
    std::string text = HeaderField("cpu");
    if (machine.detected && machine.physical_cores > 0) {
      if (!text.empty()) text += " · ";
      text += Format(_("%d cores, %d threads"), machine.physical_cores,
                     machine.logical_cpus);
    }
    if (machine.detected && machine.memory_bytes > 0) {
      if (!text.empty()) text += " · ";
      const double gib = static_cast<double>(machine.memory_bytes) /
                         (1024.0 * 1024.0 * 1024.0);
      text += Format(_("%.1f GiB of memory"), gib);
    }
    return text.empty() ? std::string(_("Looking…")) : text;
  };
  spec.hint.details =
      // performance_policy.cc: worker counts from physical cores.
      _("The Performance page sizes Roblox's worker threads from the physical "
        "cores when multithreaded rendering or throughput mode is on.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildCopyRow(LauncherContext* context) {
  GtkWidget* row = NewActionRow("edit-copy-symbolic",
                                [context] { CopyDiagnostics(context); });
  RowSpec spec;
  spec.title = _("Copy diagnostic info");
  spec.keywords = {"diagnostics", "debug",       "bug",    "report",
                   "support",     "диагностика", "ошибка", "отчёт"};
  spec.hint.subtitle = _("Versions, this computer and your main settings");
  spec.hint.details =
      _("Copies a plain-text summary for a bug report: Mocktail's and "
        "Roblox's versions, the graphics card, display and desktop, and the "
        "main settings.") +
      std::string("\n\n") +
      _("It holds no sign-in data, no values of environment variables, no "
        "proxy address and no Discord texts. Paste it into your report as "
        "it is.");
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildLogsRow(LauncherContext* context) {
  GtkWidget* row = NewActionRow("folder-open-symbolic", [context] {
    std::error_code error;
    std::filesystem::create_directories(context->paths().logs_root(), error);
    context->OpenPath(context->paths().logs_root());
  });
  RowSpec spec;
  spec.title = _("Open logs folder");
  spec.keywords = {"logs", "log", "latest.log", "логи", "журнал"};
  spec.hint.subtitle = _("latest.log is the log of the last start");
  // FAQ.md; session_log.cc UpdateLatestLink.
  spec.hint.details =
      _("Each start of Roblox writes a log in sessions/, and latest.log "
        "points at the newest one. Attach it when you report a problem.");
  return DecorateRow(context, row, std::move(spec));
}

}  // namespace

void OpenAboutDialog(LauncherContext* context) {
  if (context->window() == nullptr) return;
  AdwDialog* dialog = adw_about_dialog_new();
  AdwAboutDialog* about = ADW_ABOUT_DIALOG(dialog);
  adw_about_dialog_set_application_name(about, "Mocktail");
  GtkIconTheme* theme = gtk_icon_theme_get_for_display(
      gtk_widget_get_display(GTK_WIDGET(context->window())));
  if (gtk_icon_theme_has_icon(theme, "space.bigrat.mocktail")) {
    adw_about_dialog_set_application_icon(about, "space.bigrat.mocktail");
  }
  // packaging/space.bigrat.mocktail.metainfo.xml and README.md.
  adw_about_dialog_set_developer_name(about, "Mocktail Project Authors");
  adw_about_dialog_set_version(about, MOCKTAIL_PROJECT_VERSION);
  adw_about_dialog_set_comments(
      about, _("Play Roblox on Linux. Mocktail is an independent community "
               "project. It is not affiliated with Roblox Corporation or "
               "VinegarHQ, and the Roblox client is not included."));
  adw_about_dialog_set_website(about,
                               "https://github.com/komaruworld/mocktail");
  adw_about_dialog_set_issue_url(
      about, "https://github.com/komaruworld/mocktail/issues");
  adw_about_dialog_set_copyright(about, "© 2026 Mocktail Project Authors");
  adw_about_dialog_set_license_type(about, GTK_LICENSE_APACHE_2_0);
  const std::string debug = DiagnosticText(*context);
  adw_about_dialog_set_debug_info(about, debug.c_str());
  adw_about_dialog_set_debug_info_filename(about, "mocktail-diagnostics.txt");
  adw_dialog_present(dialog, GTK_WIDGET(context->window()));
}

// About (research/ux.md 4.3 ABOUT, SPEC 5 About).
GtkWidget* BuildAboutPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kAbout,
              _("Versions, this computer, and help when something goes wrong"));
  StartSessionHeader(context);

  GtkWidget* mocktail = AddGroup(page, "Mocktail", _("This build of Mocktail"));
  AddRow(mocktail, BuildVersionRow(context));
  AddRow(mocktail, BuildCreditsRow(context));

  GtkWidget* roblox =
      AddGroup(page, "Roblox", _("The Roblox for Android build Mocktail runs"));
  AddRow(roblox, BuildInstalledRobloxRow(context, true));
  AddRow(roblox, BuildLatestRobloxRow(context));

  GtkWidget* system =
      AddGroup(page, _("This computer"),
               _("What Mocktail found here; other pages base advice on it"));
  AddRow(system, BuildSessionRow(context));
  AddRow(system, BuildGraphicsRow(context));
  AddRow(system, BuildDisplayRow(context));
  AddRow(system, BuildProcessorRow(context));

  GtkWidget* diagnostics =
      AddGroup(page, _("Diagnostics"),
               _("For reporting a problem to Mocktail's authors"));
  AddRow(diagnostics, BuildCopyRow(context));
  AddRow(diagnostics, BuildLogsRow(context));
  return page;
}

}  // namespace mocktail::launcher_ui
