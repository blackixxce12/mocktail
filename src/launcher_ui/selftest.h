#ifndef MOCKTAIL_LAUNCHER_UI_SELFTEST_H_
#define MOCKTAIL_LAUNCHER_UI_SELFTEST_H_

#include <adwaita.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "launcher_ui/launcher_context.h"
#include "launcher_ui/window.h"

// mocktail_launcher_ui --selftest <out-dir> (developer flag, SPEC 5a): opens
// the real window on a scratch config under <out-dir>/home, walks every
// section, changes a set of rows through the bindings, saves and validates
// the result with the real loader, searches, resizes through the
// breakpoint, renders each page to <out-dir>/<section>-<width>.png through
// GtkSnapshot -> GskRenderer -> GdkTexture at the surface scale, writes
// <out-dir>/report.json and exits 0 (no errors) or 1. It needs no input
// injection, so it runs the same in every compositor sandbox.
namespace mocktail::launcher_ui {

// Points every Mocktail and XDG path at <out_dir>/home before anything reads
// them, writes a scratch config.yaml and window-state.json there, and sets
// one environment override so the banner and badges are exercised. Never
// touches the real HOME.
bool PrepareSelftestEnvironment(const std::filesystem::path& out_dir,
                                std::string* error);

class Selftest {
 public:
  Selftest(AdwApplication* application, LauncherContext* context,
           LauncherWindow* window, std::filesystem::path out_dir);

  // Runs the steps once the window is ready; quits the application at the
  // end.
  void Start();
  int exit_code() const { return errors_.empty() ? 0 : 1; }

 private:
  using Step = std::function<guint()>;  // returns the delay before the next

  void RunNext();
  guint WaitForMachine();
  guint RecordWindow();
  guint RenderSection(Section section, const std::string& suffix);
  guint BuildBindingPage();
  // Opens the "Learn more" popover of the first row bound to `key` (or
  // titled `key`) and renders it to hint-<key>.png.
  guint OpenHint(const std::string& key);
  guint RenderHint(const std::string& key);
  guint ChangeRows();
  guint RenderBindingPage();
  guint SearchStep();
  guint SaveStep();
  guint ChangedOnDisk();
  guint OpenEnvironmentDialog();
  guint RenderEnvironmentDialog();
  guint MoveThenDropMovedValues();
  guint MoveEnvironment();
  // Sets every kind of Roblox override and opens the overview.
  guint ShowRobloxDecides();
  guint Resize(int width, int height);
  guint RecordResize(int requested_width);
  guint CheckHints();
  guint Finish();
  bool Render(GtkWidget* content, const std::filesystem::path& path,
              std::string* detail);
  void Error(std::string message);
  void Note(std::string key, std::string json_value);

  AdwApplication* application_;
  LauncherContext* context_;
  LauncherWindow* window_;
  std::filesystem::path out_dir_;
  std::vector<Step> steps_;
  std::size_t next_step_ = 0;
  int machine_waits_ = 0;
  std::vector<std::string> errors_;
  std::vector<std::string> warnings_;
  std::vector<std::pair<std::string, std::string>> notes_;  // key, JSON
  std::vector<std::string> rendered_;
  GtkWidget* binding_page_ = nullptr;
  GtkWidget* switch_row_ = nullptr;
  GtkWidget* spin_row_ = nullptr;
  GtkWidget* entry_row_ = nullptr;
  GtkWidget* combo_row_ = nullptr;
  GtkWidget* hint_button_ = nullptr;
  // The overview's last list, opened before it is rendered.
  GtkWidget* decides_lists_ = nullptr;
  std::vector<std::string> resize_observations_;
};

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_SELFTEST_H_
