#include "launcher_ui/network_updates_status.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/page_widgets.h"
#include "runtime/environment.h"

namespace mocktail::launcher_ui {
namespace {

// `status` only reads two small files under the store lock; check-latest
// asks APKPure over the network (update/main.cc).
constexpr guint kStatusTimeoutSeconds = 20;
constexpr guint kLatestTimeoutSeconds = 90;

std::unique_ptr<RobloxStatus>& Instance() {
  static std::unique_ptr<RobloxStatus> instance;
  return instance;
}

bool IsExecutableFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

std::filesystem::path OwnExecutable() {
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error);
}

std::string FormatDate(std::int64_t seconds) {
  if (seconds <= 0) return {};
  GDateTime* time = g_date_time_new_from_unix_local(seconds);
  if (time == nullptr) return {};
  gchar* text = g_date_time_format(time, "%e %B %Y");
  g_date_time_unref(time);
  std::string result = text != nullptr ? text : "";
  g_free(text);
  while (!result.empty() && result.front() == ' ') result.erase(0, 1);
  return result;
}

std::string VersionLabel(const std::string& name, const std::string& code) {
  return code.empty() ? name : name + " (" + code + ")";
}

bool AutomaticUpdates(const LauncherContext& context) {
  return context.EffectiveValue("updates.automatic", "true") == "true";
}

}  // namespace

RobloxStatus* RobloxStatus::For(LauncherContext* context) {
  std::unique_ptr<RobloxStatus>& instance = Instance();
  if (instance == nullptr) {
    instance.reset(new RobloxStatus(context));
    instance->ReadInstalled();
  }
  return instance.get();
}

RobloxStatus::RobloxStatus(LauncherContext* context) : context_(context) {}

RobloxStatus::~RobloxStatus() {
  // Nothing outlives the window: a check still running is stopped.
  for (GSubprocess* process : {status_process_, latest_process_}) {
    if (process != nullptr) {
      g_subprocess_force_exit(process);
      g_object_unref(process);
    }
  }
  for (const guint source : {status_timeout_, latest_timeout_}) {
    if (source != 0) g_source_remove(source);
  }
}

bool RobloxStatus::Spawn(const char* command, GSubprocess** process,
                         std::string* error) {
  const std::filesystem::path updater = ResolveUpdaterHelper(
      runtime::ProcessEnvironment(), OwnExecutable(), IsExecutableFile);
  if (updater.empty()) {
    *error = _("mocktail_updater was not found next to this window");
    return false;
  }
  GError* spawn_error = nullptr;
  *process = g_subprocess_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE),
      &spawn_error, updater.c_str(), command, nullptr);
  if (*process == nullptr) {
    *error = spawn_error != nullptr ? spawn_error->message : "";
    g_clear_error(&spawn_error);
    return false;
  }
  return true;
}

void RobloxStatus::ReadInstalled() {
  installed_state_ = Installed::kReading;
  std::string error;
  if (context_->selftest() || !Spawn("status", &status_process_, &error)) {
    // The self-test never starts helpers; without the updater the manifest
    // itself still says what is installed.
    ReadManifestFile();
    return;
  }
  status_timeout_ = g_timeout_add_seconds(
      kStatusTimeoutSeconds,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<RobloxStatus*>(data);
        self->status_timeout_ = 0;
        if (self->status_process_ != nullptr) {
          g_subprocess_force_exit(self->status_process_);
        }
        return G_SOURCE_REMOVE;
      },
      this);
  g_subprocess_communicate_utf8_async(status_process_, nullptr, nullptr,
                                      OnStatusDone, this);
}

void RobloxStatus::ReadManifestFile() {
  const std::filesystem::path path =
      context_->paths().active_payload_manifest();
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    installed_ = InstalledRoblox();
    installed_state_ = Installed::kReady;
    FinishInstalled();
    return;
  }
  std::ifstream input(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  if (ParseActivePayloadManifest(bytes, &installed_)) {
    installed_state_ = Installed::kReady;
  } else {
    installed_state_ = Installed::kFailed;
    installed_error_ = _("current.json is not a valid Roblox record");
  }
  FinishInstalled();
}

void RobloxStatus::OnStatusDone(GObject* source, GAsyncResult* result,
                                gpointer data) {
  auto* self = static_cast<RobloxStatus*>(data);
  gchar* output = nullptr;
  gchar* errors = nullptr;
  GError* error = nullptr;
  const bool communicated = g_subprocess_communicate_utf8_finish(
      G_SUBPROCESS(source), result, &output, &errors, &error);
  const bool exited_cleanly =
      communicated && g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
      g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;
  if (self->status_timeout_ != 0) {
    g_source_remove(self->status_timeout_);
    self->status_timeout_ = 0;
  }
  g_clear_object(&self->status_process_);
  if (exited_cleanly &&
      ParseUpdaterStatus(output != nullptr ? output : "", &self->installed_)) {
    self->installed_state_ = Installed::kReady;
  } else {
    self->installed_state_ = Installed::kFailed;
    self->installed_error_ =
        UpdaterErrorMessage(errors != nullptr ? errors : "");
    if (self->installed_error_.empty()) {
      self->installed_error_ =
          error != nullptr ? std::string(error->message)
                           : std::string(_("the updater gave no answer"));
    }
  }
  g_clear_error(&error);
  g_free(output);
  g_free(errors);
  self->FinishInstalled();
}

void RobloxStatus::FinishInstalled() {
  if (installed_state_ == Installed::kReady && installed_.installed) {
    context_->SetIdleStatus(
        Format(_("Roblox %s"), installed_.version_name.c_str()));
  }
  Changed();
}

void RobloxStatus::CheckLatest() {
  if (latest_state_ == Latest::kChecking) return;
  if (context_->selftest()) {
    // No network during the self-test.
    context_->Toast(_("Update checks are skipped during the self-test"));
    return;
  }
  std::string error;
  if (!Spawn("check-latest", &latest_process_, &error)) {
    latest_state_ = Latest::kFailed;
    latest_error_ = error;
    context_->Toast(
        Format(_("Could not check for Roblox updates: %s"), error.c_str()));
    Changed();
    return;
  }
  latest_state_ = Latest::kChecking;
  latest_timeout_ = g_timeout_add_seconds(
      kLatestTimeoutSeconds,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<RobloxStatus*>(data);
        self->latest_timeout_ = 0;
        if (self->latest_process_ != nullptr) {
          g_subprocess_force_exit(self->latest_process_);
        }
        return G_SOURCE_REMOVE;
      },
      this);
  g_subprocess_communicate_utf8_async(latest_process_, nullptr, nullptr,
                                      OnLatestDone, this);
  Changed();
}

void RobloxStatus::OnLatestDone(GObject* source, GAsyncResult* result,
                                gpointer data) {
  auto* self = static_cast<RobloxStatus*>(data);
  gchar* output = nullptr;
  gchar* errors = nullptr;
  GError* error = nullptr;
  const bool communicated = g_subprocess_communicate_utf8_finish(
      G_SUBPROCESS(source), result, &output, &errors, &error);
  const bool timed_out = self->latest_timeout_ == 0;
  const bool exited_cleanly =
      communicated && g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
      g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;
  if (self->latest_timeout_ != 0) {
    g_source_remove(self->latest_timeout_);
    self->latest_timeout_ = 0;
  }
  g_clear_object(&self->latest_process_);
  if (exited_cleanly &&
      ParseCheckLatest(output != nullptr ? output : "", &self->latest_)) {
    self->latest_state_ = Latest::kReady;
    switch (CompareWithLatest(self->installed_, self->latest_)) {
      case UpdateComparison::kNewerAvailable:
        self->context_->Toast(Format(_("Roblox %s is available"),
                                     self->latest_.version_name.c_str()));
        break;
      case UpdateComparison::kUpToDate:
      case UpdateComparison::kInstalledNewer:
        self->context_->Toast(_("Roblox is up to date"));
        break;
      case UpdateComparison::kNotInstalled:
        self->context_->Toast(Format(_("The newest Roblox is %s"),
                                     self->latest_.version_name.c_str()));
        break;
    }
  } else {
    self->latest_state_ = Latest::kFailed;
    self->latest_error_ =
        timed_out ? std::string(_("no answer in time"))
                  : UpdaterErrorMessage(errors != nullptr ? errors : "");
    if (self->latest_error_.empty()) {
      self->latest_error_ = error != nullptr
                                ? std::string(error->message)
                                : std::string(_("the updater gave no answer"));
    }
    self->context_->Toast(Format(_("Could not check for Roblox updates: %s"),
                                 self->latest_error_.c_str()));
  }
  g_clear_error(&error);
  g_free(output);
  g_free(errors);
  self->Changed();
}

void RobloxStatus::Changed() {
  context_->NotifySettingChanged(kRobloxStatusKey);
}

// ---- rows -------------------------------------------------------------------

GtkWidget* BuildInstalledRobloxRow(LauncherContext* context, bool detailed) {
  RobloxStatus* status = RobloxStatus::For(context);
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Installed Roblox");
  spec.keywords = {"roblox",  "version", "installed",     "payload", "build",
                   "current", "версия",  "установленный", "сборка"};
  spec.hint.subtitle_for = [status, detailed](LauncherContext& context,
                                              const std::string&) {
    switch (status->installed_state()) {
      case RobloxStatus::Installed::kReading:
        return std::string(_("Reading…"));
      case RobloxStatus::Installed::kFailed:
        return Format(_("Could not read it: %s"),
                      status->installed_error().c_str());
      case RobloxStatus::Installed::kReady:
        break;
    }
    const InstalledRoblox& installed = status->installed();
    if (!installed.installed) {
      // update_coordinator.cc: without a payload and with automatic
      // updates off, the startup preflight fails.
      return AutomaticUpdates(context)
                 ? std::string(_("Not installed yet; Mocktail downloads it "
                                 "when you press Play"))
                 : std::string(_("Not installed, and automatic updates are "
                                 "off, so Roblox cannot start"));
    }
    std::string text =
        VersionLabel(installed.version_name, installed.version_code);
    if (detailed) {
      if (!installed.build_id.empty()) {
        text += "\n" + Format(_("Library ID %s"),
                              installed.build_id.substr(0, 12).c_str());
      }
      const std::string date = FormatDate(installed.activated_at);
      if (!date.empty()) text += "\n" + Format(_("Installed %s"), date.c_str());
    }
    return text;
  };
  spec.hint.details =
      // payload_store.cc: current.json and previous_good.json.
      _("The version of Roblox for Android that Mocktail runs, as recorded in "
        "current.json in Mocktail's data folder. Mocktail also keeps the "
        "previous working version, so a failed update never leaves you "
        "without one.") +
      std::string("\n\n") +
      // README "How it works"; payload_store.cc ExactSupported.
      _("Before Roblox runs, Mocktail checks the package's signature and that "
        "its library matches a known compatibility profile; the library ID "
        "names that exact build.");
  spec.hint.details_for = [status](LauncherContext& context) {
    std::string text = Format(
        _("Record: %s"), context.paths().active_payload_manifest().c_str());
    if (status->installed_state() == RobloxStatus::Installed::kReady &&
        !status->installed().build_id.empty()) {
      text += "\n\n" +
              Format(_("Library ID: %s"), status->installed().build_id.c_str());
    }
    return text;
  };
  return DecorateRow(context, row, std::move(spec));
}

GtkWidget* BuildLatestRobloxRow(LauncherContext* context) {
  RobloxStatus* status = RobloxStatus::For(context);
  GtkWidget* row = adw_action_row_new();
  GtkWidget* spinner = adw_spinner_new();
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(spinner, FALSE);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), spinner);
  GtkWidget* button =
      NewRowButton(_("Check"), [status] { status->CheckLatest(); });
  gtk_widget_set_tooltip_text(button, _("Check for Roblox updates"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Check for Roblox updates"), -1);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
  FollowContext(context, row, [status, spinner, button] {
    const bool checking =
        status->latest_state() == RobloxStatus::Latest::kChecking;
    gtk_widget_set_visible(spinner, checking);
    gtk_widget_set_sensitive(button, !checking);
  });

  RowSpec spec;
  spec.title = _("Newest Roblox");
  spec.keywords = {"update",     "updates",   "check",
                   "latest",     "apkpure",   "обновление",
                   "обновления", "проверить", "новая версия"};
  spec.hint.subtitle_for = [status](LauncherContext& context,
                                    const std::string&) {
    switch (status->latest_state()) {
      case RobloxStatus::Latest::kNotChecked:
        return std::string(_("Not checked yet"));
      case RobloxStatus::Latest::kChecking:
        return std::string(_("Asking APKPure…"));
      case RobloxStatus::Latest::kFailed:
        return Format(_("Could not check: %s"), status->latest_error().c_str());
      case RobloxStatus::Latest::kReady:
        break;
    }
    const LatestRoblox& latest = status->latest();
    const std::string version =
        VersionLabel(latest.version_name, latest.version_code);
    switch (CompareWithLatest(status->installed(), latest)) {
      case UpdateComparison::kNewerAvailable:
        return AutomaticUpdates(context)
                   ? Format(_("%s is available; Mocktail tests it at the next "
                              "start"),
                            version.c_str())
                   : Format(_("%s is available; automatic updates are off"),
                            version.c_str());
      case UpdateComparison::kUpToDate:
        return Format(_("%s: you have the newest version"), version.c_str());
      case UpdateComparison::kInstalledNewer:
        return Format(_("%s on APKPure; yours is newer"), version.c_str());
      case UpdateComparison::kNotInstalled:
        break;
    }
    return Format(_("%s is the newest version"), version.c_str());
  };
  spec.hint.details =
      // update/main.cc check-latest: ApkPureProvider().CheckLatest().
      _("Asks APKPure, where Mocktail downloads Roblox from, which version of "
        "Roblox for Android is the newest. Nothing is downloaded or installed "
        "here.") +
      std::string("\n\n") +
      // The template's updates.automatic comment.
      _("With automatic updates on, Mocktail does that when Roblox starts: it "
        "tests a new version in two separate test runs with your graphics "
        "backend and switches to it only if both pass.") +
      "\n\n" +
      // src/update sets no CURLOPT_PROXY; the MOCKTAIL_HTTP_PROXY_* settings
      // are read by the runtime only.
      _("This needs an internet connection. Like Roblox downloads, it does "
        "not use the proxy from Network & Updates.");
  return DecorateRow(context, row, std::move(spec));
}

}  // namespace mocktail::launcher_ui
