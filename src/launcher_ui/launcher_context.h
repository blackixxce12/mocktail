#ifndef MOCKTAIL_LAUNCHER_UI_LAUNCHER_CONTEXT_H_
#define MOCKTAIL_LAUNCHER_UI_LAUNCHER_CONTEXT_H_

#include <adwaita.h>

#include <array>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/config_document.h"
#include "launcher_ui/env_overrides.h"
#include "launcher_ui/machine_profile.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/search_index.h"
#include "launcher_ui/settings_draft.h"
#include "runtime/launcher_ui_launch.h"
#include "runtime/runtime_paths.h"

namespace mocktail::launcher_ui {

// What the window shell does for the context. LauncherWindow implements it;
// pages never call it directly.
class LauncherShell {
 public:
  virtual ~LauncherShell() = default;
  virtual GtkWindow* gtk_window() = 0;
  virtual void PresentToast(AdwToast* toast) = 0;
  // Banners, the launch bar (unsaved count, problems, status) and the
  // Save/Play sensitivity changed.
  virtual void Refresh() = 0;
  virtual void ShowSection(Section section) = 0;
  // Navigates to the page holding `widget`, scrolls to it, focuses it and
  // highlights it briefly.
  virtual void Reveal(GtkWidget* widget) = 0;
  virtual bool narrow() const = 0;
  // Closes the window and ends the application with the context's outcome.
  virtual void Finish() = 0;
};

// The persistent states shown above the page, one at a time, in this
// priority order (research/ux.md 4.4).
enum class BannerKind {
  kConfigError = 0,
  // Not used while mocktail spawns the window under its instance lock; kept
  // for a standalone settings window.
  kAlreadyRunning = 1,
  kEnvironmentOverrides = 2,
  kChangedOnDisk = 3,
};
inline constexpr std::size_t kBannerKindCount = 4;

struct Banner {
  std::string title;
  // Empty: no button.
  std::string button;
  std::function<void()> on_button;
};

// What a bound row is, for the self-test and for hint coverage.
enum class RowKind { kSwitch, kCombo, kSpin, kEntry, kAction };

struct RowRecord {
  GtkWidget* row = nullptr;  // weak; cleared when the row is destroyed
  Section section = Section::kGraphics;
  RowKind kind = RowKind::kAction;
  std::string key;  // config.yaml key, empty for unbound rows
  std::string title;
  int search_id = -1;
  bool has_subtitle = false;
  bool has_details = false;
};

// Extra state saved together with config.yaml (fflags.json, ...). Every
// callback is optional.
struct DirtySource {
  std::string name;
  // Unsaved changes it holds.
  std::function<int()> count;
  // A problem that blocks Save and Play, empty when none.
  std::function<std::string()> problem;
  // Runs after config.yaml was saved; false (with *error) stops the save.
  std::function<bool(std::string* error)> save;
  std::function<void()> discard;
};

struct LauncherOptions {
  // config.yaml the window edits: MOCKTAIL_CONFIG_FILE, else the runtime
  // path.
  std::filesystem::path config_file;
  // mocktail created config.yaml on this start
  // (MOCKTAIL_LAUNCHER_CONFIG_CREATED=1).
  bool config_created = false;
  // --selftest: no helper processes, no network, no account changes.
  bool selftest = false;
};

// The settings window's shared state and services. One instance lives as
// long as the application; pages get a pointer to it and bind their rows
// through bindings.h, which uses everything below. Single-threaded: call it
// from the GTK main thread only.
class LauncherContext {
 public:
  explicit LauncherContext(LauncherOptions options);
  ~LauncherContext();
  LauncherContext(const LauncherContext&) = delete;
  LauncherContext& operator=(const LauncherContext&) = delete;

  // ---- start-up (main.cc / window.cc) -----------------------------------
  // Loads config.yaml, the environment overrides and the base paths, and
  // sets the initial banners. Fast; before the window is built.
  void Load();
  void AttachShell(LauncherShell* shell);
  // Detects the machine profile on a worker thread; OnMachineChanged
  // listeners run when it is done.
  void StartMachineDetection();
  // The window's monitor, after map and on every change.
  void SetMonitor(const MonitorInfo& monitor);
  // Watches config.yaml for changes made outside the window.
  void StartFileMonitor();
  // Called by the window before it builds each page.
  void BeginSection(Section section) { current_section_ = section; }
  void SetNarrow(bool narrow);
  // Color for inline warnings, from the theme (set by the window).
  void SetWarningColor(std::string hex) { warning_color_ = std::move(hex); }

  // ---- state --------------------------------------------------------------
  const LauncherOptions& options() const { return options_; }
  bool selftest() const { return options_.selftest; }
  // Base paths from the environment (the account store, logs, fflags.json
  // and window-state.json). config_file() is the file this window edits.
  const runtime::RuntimePaths& paths() const { return paths_; }
  const std::filesystem::path& config_file() const {
    return options_.config_file;
  }
  std::filesystem::path window_state_file() const;
  std::filesystem::path fast_flags_file() const;
  SettingsDraft& draft() { return draft_; }
  const SettingsDraft& draft() const { return draft_; }
  const EnvOverrides& env() const { return env_; }
  // Overrides the game uses this launch: none once the user moved them
  // into the settings ("play ignore-env").
  bool ignoring_environment() const { return ignore_environment_; }
  const EnvOverride* EffectiveOverride(std::string_view key) const;
  const MachineProfile& machine() const { return machine_; }
  SearchIndex& search() { return search_; }
  const std::vector<RowRecord>& rows() const { return rows_; }
  // config.yaml cannot be edited until its error is fixed.
  bool read_only() const { return draft_.read_only(); }
  Section current_section() const { return current_section_; }
  bool narrow() const;
  GtkWindow* window() const;
  const std::string& warning_color() const { return warning_color_; }

  // ---- values ---------------------------------------------------------------
  // The value config.yaml will hold (draft), nullopt when absent.
  std::optional<std::string> Value(std::string_view key) const;
  // The value the game reads from config.yaml: Value(), else the first-run
  // template's value, else `fallback` (the runtime's built-in default for
  // keys the template leaves commented out).
  std::string EffectiveValue(std::string_view key,
                             std::string_view fallback = {}) const;
  // The value the game uses for `key` this launch: the config.yaml form of
  // the variable overriding it while overrides apply (SDL_VIDEODRIVER=
  // wayland reads as display.server: wayland), else EffectiveValue().
  // Hints worked out from another row's setting read this, so they say
  // what this launch does; a row's own value stays the draft's.
  std::string GameValue(std::string_view key,
                        std::string_view fallback = {}) const;
  // "Worked out from config.yaml; this launch uses NAME=value instead." for
  // the first of `keys` overridden by a variable whose value has no
  // config.yaml form, which GameValue() cannot follow; empty otherwise.
  std::string UnfollowedOverrideNote(
      std::initializer_list<std::string_view> keys) const;
  // Change the draft. Errors (a value the editor refuses) are shown as a
  // toast and return false. Listeners run on success.
  bool SetValue(std::string_view key, std::string_view value,
                launcher::ScalarKind kind);
  bool SetValue(std::string_view key, std::string_view value);
  bool UnsetValue(std::string_view key);
  // The template value, or absent when the template has none.
  bool ResetValue(std::string_view key);

  // ---- listeners
  // -------------------------------------------------------------
  using ListenerId = unsigned int;
  // key is the changed key, or empty when everything may have changed
  // (discard, reload, environment moved into the settings).
  ListenerId OnSettingChanged(std::function<void(std::string_view key)> fn);
  ListenerId OnMachineChanged(std::function<void()> fn);
  ListenerId OnLayoutChanged(std::function<void(bool narrow)> fn);
  void RemoveListener(ListenerId id);
  void NotifySettingChanged(std::string_view key);
  void NotifyMachineChanged();

  // ---- rows (bindings.h) ---------------------------------------------------
  // Registers a row for search, the self-test and hint coverage. The record
  // forgets the widget when it is destroyed.
  int RegisterRow(RowRecord record, std::vector<std::string> keywords,
                  const std::string& subtitle);
  void UpdateRowSubtitle(int search_id, const std::string& subtitle);
  // A row whose current input cannot be saved (an invalid host, a size out
  // of range). Save and Play stay disabled while any problem is set; the
  // launch bar names the first. Empty text clears the owner's problem.
  void SetProblem(const void* owner, std::string text);
  int problem_count() const;
  std::string first_problem() const;

  // ---- saving and launching ------------------------------------------------
  void AddDirtySource(DirtySource source);
  // Dirty sources call this after they changed.
  void NotifyDirtyChanged();
  // Changed settings plus the dirty sources' counts.
  int unsaved_count() const;
  bool can_save() const;
  bool can_play() const;
  // Why Play is disabled, empty when it is not.
  std::string play_blocker() const;
  // Runs after a successful save, before "play" is reported: the account
  // selection (accounts/active). A failure keeps the window open.
  void AddPlayHook(std::function<bool(std::string* error)> hook);

  // Validate, save config.yaml (and window-state.json's windowed size when
  // window.width/height changed), then the dirty sources. Shows a toast.
  bool Save();
  void Discard();
  // Reads config.yaml again (asks first when there are unsaved changes).
  void Reload();
  // Validate, save, run the play hooks, report "play" (or "play
  // ignore-env") and close.
  void Play();
  // Close the window: asks Save / Discard / Cancel when there are unsaved
  // changes, then reports "quit".
  void RequestClose();
  // Answers the shell's close-request: true when the window may close now.
  bool HandleCloseRequest();
  // "Move into settings": imports the environment overrides into the draft
  // and asks mocktail to leave them out of this launch.
  void MoveEnvironmentIntoSettings();
  // The banner's Details dialog: every overriding variable with its
  // (redacted) value and origin, Move into Settings and Clean Up Shortcut.
  void ShowEnvironmentDialog();
  runtime::LauncherUiResult outcome() const { return outcome_; }

  // ---- feedback
  // ---------------------------------------------------------------
  void Toast(const std::string& title);
  void Toast(const std::string& title, const std::string& button,
             std::function<void()> on_button);
  void SetBanner(BannerKind kind, Banner banner);
  void ClearBanner(BannerKind kind);
  // The banner to show, nullptr when none.
  const Banner* TopBanner(BannerKind* kind = nullptr) const;
  // Shown in the launch bar when nothing needs attention ("Roblox 2.736 ·
  // Vulkan"); pages may set it.
  void SetIdleStatus(std::string text);
  std::string StatusText() const;

  // ---- navigation and files ------------------------------------------------
  void ShowSection(Section section);
  void Reveal(GtkWidget* widget);
  // Opens a file or folder with the desktop's default application: inside
  // Flatpak or Snap through the OpenURI portal, otherwise through GIO with
  // Mocktail's own variables left out of the application's environment
  // (host_launch_environment.h). Failures are toasts.
  void OpenPath(const std::filesystem::path& path);

 private:
  struct Listener {
    ListenerId id = 0;
    std::function<void(std::string_view)> setting;
    std::function<void()> machine;
    std::function<void(bool)> layout;
  };

  void RefreshShell();
  void UpdateConfigBanners();
  void UpdateEnvironmentBanner();
  void ShowConfigErrorDialog();
  // A change the draft refused, named by its row's title.
  void ToastRefusedChange(std::string_view key, const std::string& error);
  void ShowCloseDialog();
  bool SaveInternal(bool quiet);
  void Finish(runtime::LauncherUiResult outcome);
  static void OnMachineDetected(GObject* source, GAsyncResult* result,
                                gpointer data);
  static void OnConfigFileChanged(GFileMonitor* monitor, GFile* file,
                                  GFile* other, GFileMonitorEvent event,
                                  gpointer data);
  static void OnRowDestroyed(gpointer data, GObject* where_the_object_was);

  LauncherOptions options_;
  runtime::RuntimePaths paths_;
  SettingsDraft draft_;
  EnvOverrides env_;
  bool ignore_environment_ = false;
  MachineProfile machine_;
  SearchIndex search_;
  std::vector<RowRecord> rows_;
  // The weak-reference data of each live row (owned here).
  std::vector<void*> row_weak_;
  std::map<const void*, std::string> problems_;
  std::vector<DirtySource> dirty_sources_;
  std::vector<std::function<bool(std::string*)>> play_hooks_;
  std::vector<Listener> listeners_;
  ListenerId next_listener_id_ = 1;
  std::array<std::optional<Banner>, kBannerKindCount> banners_;
  std::string idle_status_;
  std::string warning_color_ = "#f6d32d";
  Section current_section_ = Section::kGraphics;
  LauncherShell* shell_ = nullptr;
  GFileMonitor* file_monitor_ = nullptr;
  GCancellable* cancellable_ = nullptr;
  bool narrow_ = false;
  bool closing_ = false;
  bool close_dialog_open_ = false;
  runtime::LauncherUiResult outcome_ = runtime::LauncherUiResult::kQuit;
};

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_LAUNCHER_CONTEXT_H_
