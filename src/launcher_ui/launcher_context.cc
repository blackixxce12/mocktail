#include "launcher_ui/launcher_context.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "launcher/desktop_entry_cleanup.h"
#include "launcher/window_state_file.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/setting_kinds.h"
#include "runtime/environment.h"
#include "runtime/host_launch_environment.h"
#include "runtime/runtime_config_bootstrap.h"

namespace mocktail::launcher_ui {
namespace {

struct RowWeakData {
  LauncherContext* context;
  std::size_t index;
};

struct ToastAction {
  std::function<void()> action;
};

void RunToastAction(AdwToast*, gpointer data) {
  auto* action = static_cast<ToastAction*>(data);
  if (action->action) action->action();
}

void FreeToastAction(gpointer data, GClosure*) {
  delete static_cast<ToastAction*>(data);
}

std::optional<int> ParseInt(const std::string& text) {
  int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::string ReadSmallFile(const std::filesystem::path& path,
                          std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  std::string bytes;
  if (!input) return bytes;
  bytes.assign(std::istreambuf_iterator<char>(input),
               std::istreambuf_iterator<char>());
  if (bytes.size() > limit) bytes.clear();
  return bytes;
}

// Plain text as markup in which a word broken across lines gets no hyphen
// (bindings.h WithoutHyphens(), which this file does not include).
std::string NoHyphensMarkup(const std::string& text) {
  gchar* escaped = g_markup_escape_text(text.c_str(), -1);
  std::string markup = std::string("<span insert_hyphens=\"false\">") +
                       (escaped != nullptr ? escaped : "") + "</span>";
  g_free(escaped);
  return markup;
}

// The title of the setting a key belongs to, from the rows the pages bound.
std::string SettingTitle(const std::vector<RowRecord>& rows,
                         std::string_view key) {
  for (const RowRecord& record : rows) {
    if (record.key == key && !record.title.empty()) return record.title;
  }
  return std::string(key);
}

// Adds a dialog response that runs `action` (the dialog keeps it).
using DialogActions = std::map<std::string, std::function<void()>>;

void OnDialogResponse(AdwAlertDialog*, const char* response, gpointer data) {
  auto* actions = static_cast<DialogActions*>(data);
  const auto found = actions->find(response != nullptr ? response : "");
  if (found != actions->end() && found->second) {
    // Copy: the action may destroy the dialog and with it the map.
    const std::function<void()> action = found->second;
    action();
  }
}

void FreeDialogActions(gpointer data, GClosure*) {
  delete static_cast<DialogActions*>(data);
}

void ConnectResponses(AdwAlertDialog* dialog, DialogActions actions) {
  g_signal_connect_data(dialog, "response", G_CALLBACK(OnDialogResponse),
                        new DialogActions(std::move(actions)),
                        FreeDialogActions, GConnectFlags(0));
}

}  // namespace

LauncherContext::LauncherContext(LauncherOptions options)
    : options_(std::move(options)),
      paths_(runtime::RuntimePaths::FromEnvironment(
          runtime::ProcessEnvironment())),
      cancellable_(g_cancellable_new()) {
  if (options_.config_file.empty()) {
    options_.config_file = paths_.config_file();
  }
}

LauncherContext::~LauncherContext() {
  g_cancellable_cancel(cancellable_);
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    if (rows_[index].row != nullptr && row_weak_[index] != nullptr) {
      g_object_weak_unref(G_OBJECT(rows_[index].row), OnRowDestroyed,
                          row_weak_[index]);
      delete static_cast<RowWeakData*>(row_weak_[index]);
    }
  }
  if (file_monitor_ != nullptr) {
    g_file_monitor_cancel(file_monitor_);
    g_signal_handlers_disconnect_by_data(file_monitor_, this);
    g_object_unref(file_monitor_);
  }
  g_object_unref(cancellable_);
}

// ---- start-up ---------------------------------------------------------------

void LauncherContext::Load() {
  draft_.Load(options_.config_file);
  env_ = EnvOverrides::FromEnvironment(runtime::ProcessEnvironment());
  UpdateConfigBanners();
  UpdateEnvironmentBanner();
}

void LauncherContext::AttachShell(LauncherShell* shell) { shell_ = shell; }

void LauncherContext::StartMachineDetection() {
  GTask* task = g_task_new(nullptr, cancellable_, OnMachineDetected, this);
  g_task_run_in_thread(
      task, [](GTask* running, gpointer, gpointer, GCancellable*) {
        auto* profile = new MachineProfile(DetectMachineProfile(
            runtime::ProcessEnvironment(), DefaultMachineProbe()));
        g_task_return_pointer(running, profile, [](gpointer data) {
          delete static_cast<MachineProfile*>(data);
        });
      });
  g_object_unref(task);
}

void LauncherContext::OnMachineDetected(GObject*, GAsyncResult* result,
                                        gpointer data) {
  GError* error = nullptr;
  auto* profile = static_cast<MachineProfile*>(
      g_task_propagate_pointer(G_TASK(result), &error));
  if (profile == nullptr) {
    // Cancelled: the context may be gone.
    g_clear_error(&error);
    return;
  }
  auto* context = static_cast<LauncherContext*>(data);
  const MonitorInfo monitor = context->machine_.monitor;
  context->machine_ = std::move(*profile);
  context->machine_.monitor = monitor;
  delete profile;
  context->NotifyMachineChanged();
}

void LauncherContext::SetMonitor(const MonitorInfo& monitor) {
  const MonitorInfo& old = machine_.monitor;
  if (old.valid == monitor.valid && old.width == monitor.width &&
      old.height == monitor.height && old.scale == monitor.scale &&
      old.refresh_millihertz == monitor.refresh_millihertz &&
      old.connector == monitor.connector) {
    return;
  }
  machine_.monitor = monitor;
  NotifyMachineChanged();
}

void LauncherContext::StartFileMonitor() {
  if (file_monitor_ != nullptr || options_.config_file.empty()) return;
  GFile* file = g_file_new_for_path(options_.config_file.c_str());
  file_monitor_ = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES,
                                      cancellable_, nullptr);
  g_object_unref(file);
  if (file_monitor_ != nullptr) {
    g_signal_connect(file_monitor_, "changed", G_CALLBACK(OnConfigFileChanged),
                     this);
  }
}

void LauncherContext::OnConfigFileChanged(GFileMonitor*, GFile*, GFile*,
                                          GFileMonitorEvent event,
                                          gpointer data) {
  if (event == G_FILE_MONITOR_EVENT_CHANGED ||
      event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
    return;  // wait for CHANGES_DONE_HINT
  }
  auto* context = static_cast<LauncherContext*>(data);
  if (context->draft_.ChangedOnDisk()) {
    context->SetBanner(
        BannerKind::kChangedOnDisk,
        {_("config.yaml was changed outside the settings window"), _("Reload"),
         [context] { context->Reload(); }});
  } else {
    context->ClearBanner(BannerKind::kChangedOnDisk);
  }
}

void LauncherContext::SetNarrow(bool narrow) {
  if (narrow == narrow_) return;
  narrow_ = narrow;
  const std::vector<Listener> listeners = listeners_;
  for (const Listener& listener : listeners) {
    if (listener.layout) listener.layout(narrow);
  }
}

// ---- state
// --------------------------------------------------------------------

std::filesystem::path LauncherContext::window_state_file() const {
  // main.cc: ConfigureWindowStatePersistence(state_root/window-state.json)
  return paths_.state_root() / "window-state.json";
}

std::filesystem::path LauncherContext::fast_flags_file() const {
  // main.cc: config_root/fflags.json
  return paths_.config_root() / "fflags.json";
}

const EnvOverride* LauncherContext::EffectiveOverride(
    std::string_view key) const {
  if (!ignore_environment_) return env_.Effective(key);
  // main.cc RemoveUserManagedEnvironment keeps what the command line set.
  for (const EnvOverride* entry : env_.ForKey(key)) {
    if (entry->command_line) return entry;
  }
  return nullptr;
}

bool LauncherContext::narrow() const { return narrow_; }

GtkWindow* LauncherContext::window() const {
  return shell_ != nullptr ? shell_->gtk_window() : nullptr;
}

// ---- values
// ---------------------------------------------------------------------

std::optional<std::string> LauncherContext::Value(std::string_view key) const {
  return draft_.Get(key);
}

std::string LauncherContext::EffectiveValue(std::string_view key,
                                            std::string_view fallback) const {
  if (std::optional<std::string> value = draft_.Get(key); value.has_value()) {
    return *value;
  }
  if (std::optional<std::string> value = draft_.TemplateValue(key);
      value.has_value()) {
    return *value;
  }
  return std::string(fallback);
}

std::string LauncherContext::GameValue(std::string_view key,
                                       std::string_view fallback) const {
  const EnvOverride* env = EffectiveOverride(key);
  if (env != nullptr && env->imported.has_value()) return *env->imported;
  return EffectiveValue(key, fallback);
}

std::string LauncherContext::UnfollowedOverrideNote(
    std::initializer_list<std::string_view> keys) const {
  for (const std::string_view key : keys) {
    const EnvOverride* env = EffectiveOverride(key);
    if (env != nullptr && !env->imported.has_value()) {
      return Format(_("Worked out from config.yaml; this launch uses %s=%s "
                      "instead."),
                    env->name.c_str(),
                    RedactEnvironmentValue(env->value).c_str());
    }
  }
  return {};
}

void LauncherContext::ToastRefusedChange(std::string_view key,
                                         const std::string& error) {
  // The draft's own message for a broken file is not for the user (and is
  // English); the rows are insensitive then, so only programmatic changes
  // get here.
  const std::string title = SettingTitle(rows_, key);
  Toast(
      draft_.read_only()
          ? Format(_("“%s” cannot be changed until config.yaml is fixed"),
                   title.c_str())
          : Format(_("Cannot change “%s”: %s"), title.c_str(), error.c_str()));
}

bool LauncherContext::SetValue(std::string_view key, std::string_view value,
                               launcher::ScalarKind kind) {
  if (draft_.Get(key) == std::optional<std::string>(std::string(value))) {
    return true;
  }
  std::string error;
  if (!draft_.Set(key, value, kind, &error)) {
    ToastRefusedChange(key, error);
    return false;
  }
  NotifySettingChanged(key);
  return true;
}

bool LauncherContext::SetValue(std::string_view key, std::string_view value) {
  return SetValue(key, value, ScalarKindFor(key, value));
}

bool LauncherContext::UnsetValue(std::string_view key) {
  if (!draft_.Get(key).has_value()) return true;
  std::string error;
  if (!draft_.Unset(key, &error)) {
    ToastRefusedChange(key, error);
    return false;
  }
  NotifySettingChanged(key);
  return true;
}

bool LauncherContext::ResetValue(std::string_view key) {
  const std::optional<std::string> value = draft_.TemplateValue(key);
  return value.has_value() ? SetValue(key, *value) : UnsetValue(key);
}

// ---- listeners
// --------------------------------------------------------------------

LauncherContext::ListenerId LauncherContext::OnSettingChanged(
    std::function<void(std::string_view)> fn) {
  Listener listener;
  listener.id = next_listener_id_++;
  listener.setting = std::move(fn);
  listeners_.push_back(std::move(listener));
  return listeners_.back().id;
}

LauncherContext::ListenerId LauncherContext::OnMachineChanged(
    std::function<void()> fn) {
  Listener listener;
  listener.id = next_listener_id_++;
  listener.machine = std::move(fn);
  listeners_.push_back(std::move(listener));
  return listeners_.back().id;
}

LauncherContext::ListenerId LauncherContext::OnLayoutChanged(
    std::function<void(bool)> fn) {
  Listener listener;
  listener.id = next_listener_id_++;
  listener.layout = std::move(fn);
  listeners_.push_back(std::move(listener));
  return listeners_.back().id;
}

void LauncherContext::RemoveListener(ListenerId id) {
  listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                  [id](const Listener& listener) {
                                    return listener.id == id;
                                  }),
                   listeners_.end());
}

void LauncherContext::NotifySettingChanged(std::string_view key) {
  const std::string owned(key);
  const std::vector<Listener> listeners = listeners_;
  for (const Listener& listener : listeners) {
    if (listener.setting) listener.setting(owned);
  }
  RefreshShell();
}

void LauncherContext::NotifyMachineChanged() {
  const std::vector<Listener> listeners = listeners_;
  for (const Listener& listener : listeners) {
    if (listener.machine) listener.machine();
  }
  RefreshShell();
}

// ---- rows
// -------------------------------------------------------------------------

int LauncherContext::RegisterRow(RowRecord record,
                                 std::vector<std::string> keywords,
                                 const std::string& subtitle) {
  record.section = current_section_;
  record.search_id = search_.Add({GetSectionInfo(current_section_).id,
                                  record.title, subtitle, std::move(keywords)});
  GtkWidget* row = record.row;
  rows_.push_back(std::move(record));
  row_weak_.push_back(nullptr);
  if (row != nullptr) {
    auto* weak = new RowWeakData{this, rows_.size() - 1};
    g_object_weak_ref(G_OBJECT(row), OnRowDestroyed, weak);
    row_weak_.back() = weak;
  }
  return rows_.back().search_id;
}

void LauncherContext::OnRowDestroyed(gpointer data, GObject*) {
  auto* weak = static_cast<RowWeakData*>(data);
  if (weak->index < weak->context->rows_.size()) {
    weak->context->rows_[weak->index].row = nullptr;
    weak->context->row_weak_[weak->index] = nullptr;
  }
  delete weak;
}

void LauncherContext::UpdateRowSubtitle(int search_id,
                                        const std::string& subtitle) {
  search_.SetSubtitle(search_id, subtitle);
}

void LauncherContext::SetProblem(const void* owner, std::string text) {
  if (text.empty()) {
    if (problems_.erase(owner) == 0) return;
  } else {
    std::string& current = problems_[owner];
    if (current == text) return;
    current = std::move(text);
  }
  RefreshShell();
}

int LauncherContext::problem_count() const {
  int count = static_cast<int>(problems_.size());
  for (const DirtySource& source : dirty_sources_) {
    if (source.problem && !source.problem().empty()) ++count;
  }
  return count;
}

std::string LauncherContext::first_problem() const {
  if (!problems_.empty()) return problems_.begin()->second;
  for (const DirtySource& source : dirty_sources_) {
    if (source.problem) {
      std::string problem = source.problem();
      if (!problem.empty()) return problem;
    }
  }
  return {};
}

// ---- saving and launching
// ---------------------------------------------------------

void LauncherContext::AddDirtySource(DirtySource source) {
  dirty_sources_.push_back(std::move(source));
  RefreshShell();
}

void LauncherContext::NotifyDirtyChanged() { RefreshShell(); }

int LauncherContext::unsaved_count() const {
  int count = static_cast<int>(draft_.ChangedKeys().size());
  if (count == 0 && draft_.HasChanges()) count = 1;
  for (const DirtySource& source : dirty_sources_) {
    if (source.count) count += std::max(0, source.count());
  }
  return count;
}

bool LauncherContext::can_save() const {
  return !read_only() && problem_count() == 0 && unsaved_count() > 0;
}

bool LauncherContext::can_play() const { return play_blocker().empty(); }

std::string LauncherContext::play_blocker() const {
  if (read_only()) return _("Fix config.yaml to play");
  if (problem_count() > 0) return first_problem();
  return {};
}

void LauncherContext::AddPlayHook(std::function<bool(std::string*)> hook) {
  play_hooks_.push_back(std::move(hook));
}

bool LauncherContext::SaveInternal(bool quiet) {
  if (read_only()) {
    Toast(_("Fix config.yaml before saving"));
    return false;
  }
  if (problem_count() > 0) {
    Toast(first_problem());
    return false;
  }
  // window-state.json overrides window.width/height once it exists
  // (window.cc restores it), so a new size is written there too.
  const bool size_changed =
      draft_.IsChanged("window.width") || draft_.IsChanged("window.height");
  std::string error;
  std::filesystem::path kept;
  if (draft_.HasChanges() && !draft_.Save(&error, &kept)) {
    if (draft_.ChangedOnDisk()) {
      Toast(Format(_("Settings were not saved: %s"), error.c_str()),
            _("Reload"), [this] { Reload(); });
      SetBanner(BannerKind::kChangedOnDisk,
                {_("config.yaml was changed outside the settings window"),
                 _("Reload"), [this] { Reload(); }});
    } else {
      Toast(Format(_("Settings were not saved: %s"), error.c_str()));
    }
    return false;
  }
  ClearBanner(BannerKind::kChangedOnDisk);
  if (size_changed) {
    const std::optional<int> width =
        ParseInt(EffectiveValue("window.width", "1280"));
    const std::optional<int> height =
        ParseInt(EffectiveValue("window.height", "720"));
    if (width.has_value() && height.has_value() &&
        !launcher::UpdateWindowedSize(window_state_file(), *width, *height,
                                      &error)) {
      Toast(Format(_("The window size was saved, but the remembered size "
                     "could not be updated: %s"),
                   error.c_str()));
    }
  }
  for (const DirtySource& source : dirty_sources_) {
    if (!source.save || (source.count && source.count() <= 0)) continue;
    error.clear();
    if (!source.save(&error)) {
      Toast(Format(_("Settings were not saved: %s"), error.c_str()));
      RefreshShell();
      return false;
    }
  }
  NotifySettingChanged("");
  if (quiet) return true;
  if (kept.empty()) {
    Toast(_("Settings saved"));
  } else {
    // Reset All replaced the whole file (SettingsDraft::Save).
    Toast(_("Settings saved; the old config.yaml is kept next to it"),
          _("Open Folder"),
          [this] { OpenPath(options_.config_file.parent_path()); });
  }
  return true;
}

bool LauncherContext::Save() { return SaveInternal(false); }

void LauncherContext::Discard() {
  draft_.Discard();
  for (const DirtySource& source : dirty_sources_) {
    if (source.discard) source.discard();
  }
  ForgetDroppedMove();
  NotifySettingChanged("");
}

void LauncherContext::Reload() {
  const auto reload = [this] {
    std::string error;
    draft_.Reload(&error);
    for (const DirtySource& source : dirty_sources_) {
      if (source.discard) source.discard();
    }
    ClearBanner(BannerKind::kChangedOnDisk);
    UpdateConfigBanners();
    ForgetDroppedMove();
    NotifySettingChanged("");
  };
  if (unsaved_count() == 0 || window() == nullptr) {
    reload();
    return;
  }
  AdwDialog* dialog = adw_alert_dialog_new(
      _("Reload config.yaml?"), _("Your unsaved changes will be lost."));
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "reload",
                                 _("_Reload"), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "reload",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(alert, "cancel");
  ConnectResponses(alert, {{"reload", reload}});
  adw_dialog_present(dialog, GTK_WIDGET(window()));
}

void LauncherContext::Play() {
  if (closing_) return;
  if (!can_play()) {
    Toast(play_blocker());
    return;
  }
  if (unsaved_count() > 0 && !SaveInternal(true)) {
    return;
  }
  for (const auto& hook : play_hooks_) {
    std::string error;
    if (!hook(&error)) {
      Toast(error.empty() ? std::string(_("Roblox could not be started"))
                          : error);
      return;
    }
  }
  Finish(ignore_environment_
             ? runtime::LauncherUiResult::kPlayIgnoringEnvironment
             : runtime::LauncherUiResult::kPlay);
}

void LauncherContext::Finish(runtime::LauncherUiResult outcome) {
  outcome_ = outcome;
  closing_ = true;
  if (shell_ != nullptr) shell_->Finish();
}

void LauncherContext::RequestClose() {
  if (HandleCloseRequest()) {
    Finish(runtime::LauncherUiResult::kQuit);
  }
}

bool LauncherContext::HandleCloseRequest() {
  if (closing_) return true;
  if (unsaved_count() > 0 && window() != nullptr) {
    ShowCloseDialog();
    return false;
  }
  outcome_ = runtime::LauncherUiResult::kQuit;
  return true;
}

void LauncherContext::ShowCloseDialog() {
  if (close_dialog_open_) return;
  close_dialog_open_ = true;
  AdwDialog* dialog = adw_alert_dialog_new(_("Save changes?"),
                                           _("Unsaved settings will be lost."));
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "discard",
                                 _("_Discard"), "save", _("_Save"), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "discard",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance(alert, "save",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(alert, "save");
  adw_alert_dialog_set_close_response(alert, "cancel");
  ConnectResponses(alert, {{"cancel", [this] { close_dialog_open_ = false; }},
                           {"discard",
                            [this] {
                              close_dialog_open_ = false;
                              Discard();
                              Finish(runtime::LauncherUiResult::kQuit);
                            }},
                           {"save", [this] {
                              close_dialog_open_ = false;
                              if (SaveInternal(true))
                                Finish(runtime::LauncherUiResult::kQuit);
                            }}});
  adw_dialog_present(dialog, GTK_WIDGET(window()));
}

void LauncherContext::MoveEnvironmentIntoSettings() {
  if (read_only()) {
    Toast(_("Fix config.yaml before moving variables into settings"));
    return;
  }
  const EnvImportReport report = ImportEnvOverrides(env_, &draft_);
  for (const std::string& error : report.errors) {
    g_warning("environment import: %s", error.c_str());
  }
  ignore_environment_ = true;
  UpdateEnvironmentBanner();
  NotifySettingChanged("");
  const int moved = static_cast<int>(report.imported.size());
  // One short line: a toast never wraps, and a narrow window cut the old
  // "…; the variables are ignored for this launch" in the middle. Both
  // places that move them (the environment dialog's text and the Advanced
  // page's "Move into settings" row) already say the variables are ignored
  // for this launch.
  Toast(Format(ngettext("%d value moved into settings",
                        "%d values moved into settings", moved),
               moved));
}

void LauncherContext::ForgetDroppedMove() {
  if (!ignore_environment_ || DraftHoldsEnvOverrides(env_, draft_)) return;
  // "play ignore-env" would now start Roblox with neither the variables'
  // values nor saved ones (main.cc RemoveUserManagedEnvironment).
  ignore_environment_ = false;
  UpdateEnvironmentBanner();
}

// ---- banners and dialogs
// ------------------------------------------------------------

void LauncherContext::UpdateConfigBanners() {
  if (!draft_.read_only()) {
    ClearBanner(BannerKind::kConfigError);
    return;
  }
  // The loader's messages are English and technical
  // (runtime_config_file.cc), so the banner says what is wrong in the
  // user's language and the dialog behind Fix… quotes the loader. A 0-byte
  // file reached the user as "runtime configuration root must be a
  // mapping".
  Banner banner;
  if (draft_.file_is_blank()) {
    banner.title = _("config.yaml is empty");
  } else if (draft_.load_error_line() > 0) {
    banner.title = Format(_("config.yaml has an error on line %d"),
                          draft_.load_error_line());
  } else {
    banner.title = _("config.yaml cannot be loaded");
  }
  banner.button = _("Fix…");
  banner.on_button = [this] { ShowConfigErrorDialog(); };
  SetBanner(BannerKind::kConfigError, std::move(banner));
}

void LauncherContext::UpdateEnvironmentBanner() {
  // A command-line option is for this launch only, and nothing can be
  // moved; its rows say so themselves.
  if (!env_.HasEnvironment() || ignore_environment_) {
    ClearBanner(BannerKind::kEnvironmentOverrides);
    return;
  }
  const int count = env_.EnvironmentSettingCount();
  SetBanner(BannerKind::kEnvironmentOverrides,
            {Format(ngettext("Shortcut or terminal variables override %d "
                             "setting",
                             "Shortcut or terminal variables override %d "
                             "settings",
                             count),
                    count),
             _("Details"), [this] { ShowEnvironmentDialog(); }});
}

void LauncherContext::ShowConfigErrorDialog() {
  if (window() == nullptr) return;
  const std::filesystem::path backup =
      launcher::ConfigDocument::BackupPath(options_.config_file);
  std::error_code filesystem_error;
  const bool has_backup =
      std::filesystem::is_regular_file(backup, filesystem_error);
  const bool blank = draft_.file_is_blank();
  AdwDialog* dialog = adw_alert_dialog_new(
      blank ? _("config.yaml is empty") : _("config.yaml cannot be loaded"),
      nullptr);
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  if (blank) {
    // The runtime refuses an empty file as well (runtime_config_file.cc:
    // the root must be a mapping), so Play stays blocked; nothing is lost
    // by writing the first-run template over it.
    adw_alert_dialog_set_body(
        alert,
        has_backup
            ? _("Roblox cannot start while the file is empty. Fill it with "
                "Mocktail's defaults, or restore the copy the settings "
                "window kept before it first saved the file.")
            : _("Roblox cannot start while the file is empty. Fill it with "
                "Mocktail's defaults, or write your settings into it in a "
                "text editor and reload it here."));
  } else {
    adw_alert_dialog_format_body(
        alert, "%s\n\n%s",
        has_backup
            ? _("Roblox cannot start until the file loads. Fix it in a text "
                "editor, or restore the copy the settings window kept before "
                "it first saved the file.")
            : _("Roblox cannot start until the file loads. Fix it in a text "
                "editor, then reload it here."),
        Format(_("The loader says: %s"), draft_.load_error().c_str()).c_str());
  }
  adw_alert_dialog_add_responses(alert, "close", _("_Close"), "open",
                                 _("_Open config.yaml"), nullptr);
  if (has_backup) {
    adw_alert_dialog_add_response(alert, "restore", _("_Restore Backup"));
  }
  adw_alert_dialog_add_response(alert, "reload", _("Re_load"));
  if (blank) {
    adw_alert_dialog_add_response(alert, "defaults", _("Use _Defaults"));
    adw_alert_dialog_set_response_appearance(alert, "defaults",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(alert, "defaults");
  }
  adw_alert_dialog_set_close_response(alert, "close");
  ConnectResponses(
      alert, {{"open", [this] { OpenPath(options_.config_file); }},
              {"reload", [this] { Reload(); }},
              {"defaults",
               [this] {
                 std::string error;
                 if (!draft_.RestoreBytes(
                         std::string(runtime::DefaultRuntimeConfigYaml()),
                         &error)) {
                   Toast(Format(_("config.yaml was not changed: %s"),
                                error.c_str()));
                   return;
                 }
                 UpdateConfigBanners();
                 ForgetDroppedMove();
                 NotifySettingChanged("");
                 Toast(_("config.yaml now holds Mocktail's defaults"));
               }},
              {"restore", [this, backup] {
                 std::string error;
                 const std::string bytes = ReadSmallFile(backup, 1024U * 1024U);
                 // The broken file may hold edits made long after the
                 // backup; RestoreBytes keeps it next to it.
                 std::filesystem::path kept;
                 if (bytes.empty() ||
                     !draft_.RestoreBytes(bytes, &error, &kept)) {
                   Toast(Format(_("The backup could not be restored: %s"),
                                error.empty() ? _("it is empty or unreadable")
                                              : error.c_str()));
                   return;
                 }
                 UpdateConfigBanners();
                 ForgetDroppedMove();
                 NotifySettingChanged("");
                 if (kept.empty()) {
                   Toast(_("config.yaml restored from the backup"));
                 } else {
                   Toast(_("Backup restored; the replaced config.yaml is "
                           "kept next to it"),
                         _("Open Folder"), [this] {
                           OpenPath(options_.config_file.parent_path());
                         });
                 }
               }}});
  adw_dialog_present(dialog, GTK_WIDGET(window()));
}

void LauncherContext::ShowEnvironmentDialog() {
  if (window() == nullptr) return;
  // Where the variables come from: the user's copy of the desktop entry
  // when it sets them (research/ux.md 4.9), the session or terminal
  // otherwise.
  auto inspection = std::make_shared<launcher::DesktopEntryInspection>();
  std::string inspect_error;
  launcher::InspectDesktopEntry(
      launcher::DefaultDesktopEntryPaths(runtime::ProcessEnvironment()),
      inspection.get(), &inspect_error);

  const bool movable = env_.HasEnvironment();
  AdwDialog* dialog = adw_alert_dialog_new(
      _("Variables that override settings"),
      movable ? _("These environment variables win over config.yaml, so the "
                  "rows marked ENV do not show what this launch uses. Moving "
                  "them into settings saves their values in config.yaml and "
                  "leaves the variables out of this launch.")
              : _("These values win over config.yaml, so the rows marked ENV "
                  "do not show what this launch uses."));
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  GtkWidget* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  for (const EnvOverride& entry : env_.all()) {
    bool from_shortcut = false;
    bool sensitive = false;
    for (const launcher::EnvAssignment& assignment :
         inspection->env_assignments) {
      if (assignment.name == entry.name) {
        from_shortcut = true;
        sensitive = assignment.sensitive;
      }
    }
    GtkWidget* row = adw_action_row_new();
    // Markup only to keep Pango from adding a hyphen where it breaks the
    // assignment or the shortcut's path in a narrow window ("dire-" /
    // "ct-vulkan"); bindings.h WithoutHyphens(). A zero-width space after
    // "=" lets a long assignment break between the name and the value.
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), TRUE);
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(row),
        NoHyphensMarkup(entry.name + "=\u200b" +
                        (sensitive ? std::string("***")
                                   : RedactEnvironmentValue(entry.value)))
            .c_str());
    std::string subtitle = Format(_("Overrides “%s”"),
                                  SettingTitle(rows_, entry.yaml_key).c_str());
    subtitle += "\n";
    if (entry.command_line) {
      // command_line.cc ApplyCommandLineEnvironment; main.cc keeps it on
      // "play ignore-env".
      subtitle += Format(_("Set by %s on the command line, for this launch "
                           "only; Move into Settings leaves it alone"),
                         entry.option.c_str());
    } else {
      subtitle += from_shortcut
                      ? Format(_("Set by your desktop shortcut %s"),
                               inspection->path.c_str())
                      : std::string(_("Set by the terminal or session that "
                                      "started Mocktail"));
    }
    if (entry.command_line) {
      // Neither moved nor left out: nothing more to say.
    } else if (entry.shadowed) {
      subtitle += "\n";
      subtitle +=
          _("Has no effect: another variable for the same setting "
            "wins");
    } else if (!entry.imported.has_value()) {
      subtitle += "\n";
      subtitle +=
          _("This value has no matching setting; it is only left "
            "out of the launch");
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                NoHyphensMarkup(subtitle).c_str());
    gtk_list_box_append(GTK_LIST_BOX(list), row);
  }
  adw_alert_dialog_set_extra_child(alert, list);
  adw_alert_dialog_add_response(alert, "close", _("_Close"));
  if (movable) {
    adw_alert_dialog_add_response(alert, "move", _("_Move into Settings"));
    adw_alert_dialog_set_response_appearance(alert, "move",
                                             ADW_RESPONSE_SUGGESTED);
  }
  const bool can_clean =
      inspection->found &&
      inspection->plan != launcher::DesktopEntryCleanupPlan::kNone;
  if (can_clean) {
    adw_alert_dialog_add_response(alert, "cleanup", _("Clean _Up Shortcut…"));
  }
  adw_alert_dialog_set_close_response(alert, "close");
  DialogActions actions = {{"move", [this] { MoveEnvironmentIntoSettings(); }}};
  if (can_clean) {
    actions["cleanup"] = [this, inspection] {
      AdwDialog* confirm =
          adw_alert_dialog_new(_("Clean up the desktop shortcut?"), nullptr);
      AdwAlertDialog* confirm_alert = ADW_ALERT_DIALOG(confirm);
      adw_alert_dialog_format_body(
          confirm_alert, "%s",
          inspection->plan == launcher::DesktopEntryCleanupPlan::kDeleteFile
              ? Format(_("Your copy of the Mocktail shortcut (%s) only adds "
                         "these variables. It will be removed so the "
                         "installed shortcut is used again. A backup is "
                         "kept next to it."),
                       inspection->path.c_str())
                    .c_str()
              : Format(_("The variables that override settings are removed "
                         "from the command of %s. Everything else in it "
                         "stays, and a backup is kept next to it."),
                       inspection->path.c_str())
                    .c_str());
      adw_alert_dialog_add_responses(confirm_alert, "cancel", _("_Cancel"),
                                     "clean", _("Clean _Up"), nullptr);
      adw_alert_dialog_set_response_appearance(confirm_alert, "clean",
                                               ADW_RESPONSE_DESTRUCTIVE);
      adw_alert_dialog_set_close_response(confirm_alert, "cancel");
      ConnectResponses(confirm_alert,
                       {{"clean", [this, inspection] {
                           if (selftest()) return;
                           launcher::DesktopEntryApplyResult result;
                           std::string error;
                           if (!launcher::ApplyDesktopEntryCleanup(
                                   *inspection, {}, &result, &error)) {
                             Toast(Format(_("The shortcut was not "
                                            "changed: %s"),
                                          error.c_str()));
                             return;
                           }
                           Toast(
                               _("Shortcut cleaned up; it "
                                 "applies from the next "
                                 "start"));
                         }}});
      adw_dialog_present(confirm, GTK_WIDGET(window()));
    };
  }
  ConnectResponses(alert, std::move(actions));
  adw_dialog_present(dialog, GTK_WIDGET(window()));
}

// ---- feedback
// -------------------------------------------------------------------

void LauncherContext::Toast(const std::string& title) {
  Toast(title, {}, nullptr);
}

void LauncherContext::Toast(const std::string& title, const std::string& button,
                            std::function<void()> on_button) {
  if (shell_ == nullptr) {
    g_message("%s", title.c_str());
    return;
  }
  AdwToast* toast = adw_toast_new("");
  adw_toast_set_use_markup(toast, FALSE);
  adw_toast_set_title(toast, title.c_str());
  if (!button.empty() && on_button) {
    adw_toast_set_button_label(toast, button.c_str());
    g_signal_connect_data(toast, "button-clicked", G_CALLBACK(RunToastAction),
                          new ToastAction{std::move(on_button)},
                          FreeToastAction, GConnectFlags(0));
  }
  shell_->PresentToast(toast);
}

void LauncherContext::SetBanner(BannerKind kind, Banner banner) {
  banners_[static_cast<std::size_t>(kind)] = std::move(banner);
  RefreshShell();
}

void LauncherContext::ClearBanner(BannerKind kind) {
  auto& slot = banners_[static_cast<std::size_t>(kind)];
  if (!slot.has_value()) return;
  slot.reset();
  RefreshShell();
}

const Banner* LauncherContext::TopBanner(BannerKind* kind) const {
  for (std::size_t index = 0; index < banners_.size(); ++index) {
    if (banners_[index].has_value()) {
      if (kind != nullptr) *kind = static_cast<BannerKind>(index);
      return &*banners_[index];
    }
  }
  return nullptr;
}

void LauncherContext::SetIdleStatus(std::string text) {
  idle_status_ = std::move(text);
  RefreshShell();
}

std::string LauncherContext::StatusText() const {
  if (read_only()) return _("config.yaml needs fixing before Roblox can start");
  if (problem_count() > 0) return first_problem();
  const int unsaved = unsaved_count();
  if (unsaved > 0) {
    return Format(ngettext("%d unsaved change", "%d unsaved changes", unsaved),
                  unsaved);
  }
  return idle_status_.empty() ? std::string(_("Ready to play")) : idle_status_;
}

void LauncherContext::RefreshShell() {
  if (shell_ != nullptr) shell_->Refresh();
}

// ---- navigation and files
// ----------------------------------------------------------

void LauncherContext::ShowSection(Section section) {
  if (shell_ != nullptr) shell_->ShowSection(section);
}

void LauncherContext::Reveal(GtkWidget* widget) {
  if (shell_ != nullptr) shell_->Reveal(widget);
}

namespace {

struct OpenRequest {
  OpenRequest() = default;
  OpenRequest(const OpenRequest&) = delete;
  OpenRequest& operator=(const OpenRequest&) = delete;
  ~OpenRequest() {
    if (launch_context != nullptr) g_object_unref(launch_context);
  }

  LauncherContext* context = nullptr;
  std::string uri;
  GAppLaunchContext* launch_context = nullptr;
};

// The context GIO starts the application with: the display's, for the
// startup or activation token, without Mocktail's own variables. The
// application starts from this window's environment, which holds every
// MOCKTAIL_* path and override mocktail passed in, the proxy
// AccountsController set up for the window's own requests and, in the
// portable bundle, the bundled library paths (host_launch_environment.h).
// The WebView helper leaves them out of the browser it opens the same way.
GAppLaunchContext* NewLaunchContext(GtkWindow* window) {
  GdkDisplay* display = window != nullptr
                            ? gtk_widget_get_display(GTK_WIDGET(window))
                            : gdk_display_get_default();
  GAppLaunchContext* context =
      display != nullptr
          ? G_APP_LAUNCH_CONTEXT(gdk_display_get_app_launch_context(display))
          : g_app_launch_context_new();
  runtime::RemoveMocktailEnvironment(context);
  return context;
}

// Flatpak or Snap, as GDK tells them (gdk_running_in_sandbox): there only the
// OpenURI portal reaches the host's applications, and the portal starts them
// with the host's environment, not this window's.
bool InSandbox() {
  return g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS) ||
         g_getenv("SNAP") != nullptr;
}

bool Dismissed(const GError* error) {
  return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
         g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
}

void OnLaunchedDirectly(GObject*, GAsyncResult* result, gpointer data) {
  std::unique_ptr<OpenRequest> request(static_cast<OpenRequest*>(data));
  GError* error = nullptr;
  if (g_app_info_launch_default_for_uri_finish(result, &error)) return;
  if (!Dismissed(error)) {
    request->context->Toast(Format(_("Could not open %s: %s"),
                                   request->uri.c_str(),
                                   error != nullptr ? error->message : ""));
  }
  g_clear_error(&error);
}

// What GtkFileLauncher does when it uses no portal, with a launch context
// that leaves Mocktail's variables out (GtkFileLauncher's own keeps them).
void LaunchDirectly(std::unique_ptr<OpenRequest> request) {
  OpenRequest* owned = request.release();
  g_app_info_launch_default_for_uri_async(owned->uri.c_str(),
                                          owned->launch_context, nullptr,
                                          OnLaunchedDirectly, owned);
}

void OnLaunchedThroughPortal(GObject* source, GAsyncResult* result,
                             gpointer data) {
  std::unique_ptr<OpenRequest> request(static_cast<OpenRequest*>(data));
  GError* error = nullptr;
  if (gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result,
                                      &error)) {
    return;
  }
  const bool dismissed = Dismissed(error);
  g_clear_error(&error);
  if (dismissed) return;
  // No handler through the portal: ask GIO directly.
  LaunchDirectly(std::move(request));
}

}  // namespace

void LauncherContext::OpenPath(const std::filesystem::path& path) {
  if (selftest()) {
    return;
  }
  GFile* file = g_file_new_for_path(path.c_str());
  gchar* uri = g_file_get_uri(file);
  auto request = std::make_unique<OpenRequest>();
  request->context = this;
  request->uri = uri != nullptr ? uri : "";
  request->launch_context = NewLaunchContext(window());
  g_free(uri);
  if (InSandbox()) {
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, window(), nullptr,
                             OnLaunchedThroughPortal, request.release());
    g_object_unref(launcher);
  } else {
    LaunchDirectly(std::move(request));
  }
  g_object_unref(file);
}

}  // namespace mocktail::launcher_ui
