#ifndef MOCKTAIL_LAUNCHER_UI_NETWORK_UPDATES_STATUS_H_
#define MOCKTAIL_LAUNCHER_UI_NETWORK_UPDATES_STATUS_H_

#include <adwaita.h>

#include <string>

#include "launcher_ui/launcher_context.h"
#include "launcher_ui/page_rules.h"

// The installed and the newest Roblox version, shown on the Network &
// Updates and the About page. The installed version comes from
// `mocktail_updater status` (offline, fast; SPEC 5), the newest from
// `mocktail_updater check-latest` when the user asks. Both run as
// subprocesses so the window never blocks, and never during the self-test
// (it reads current.json directly instead and does not check online).
namespace mocktail::launcher_ui {

class RobloxStatus {
 public:
  enum class Installed { kReading, kReady, kFailed };
  enum class Latest { kNotChecked, kChecking, kReady, kFailed };

  // The one status of this window; created, and its reading started, by
  // the first page that asks.
  static RobloxStatus* For(LauncherContext* context);
  ~RobloxStatus();
  RobloxStatus(const RobloxStatus&) = delete;
  RobloxStatus& operator=(const RobloxStatus&) = delete;

  Installed installed_state() const { return installed_state_; }
  const InstalledRoblox& installed() const { return installed_; }
  const std::string& installed_error() const { return installed_error_; }

  Latest latest_state() const { return latest_state_; }
  const LatestRoblox& latest() const { return latest_; }
  const std::string& latest_error() const { return latest_error_; }

  // Asks the updater for the newest version (ignored while one runs);
  // the result is a toast and the rows' subtitles.
  void CheckLatest();

 private:
  explicit RobloxStatus(LauncherContext* context);
  void ReadInstalled();
  void ReadManifestFile();
  void FinishInstalled();
  bool Spawn(const char* command, GSubprocess** process, std::string* error);
  static void OnStatusDone(GObject* source, GAsyncResult* result,
                           gpointer data);
  static void OnLatestDone(GObject* source, GAsyncResult* result,
                           gpointer data);
  void Changed();

  LauncherContext* context_;
  Installed installed_state_ = Installed::kReading;
  InstalledRoblox installed_;
  std::string installed_error_;
  Latest latest_state_ = Latest::kNotChecked;
  LatestRoblox latest_;
  std::string latest_error_;
  GSubprocess* status_process_ = nullptr;
  GSubprocess* latest_process_ = nullptr;
  guint status_timeout_ = 0;
  guint latest_timeout_ = 0;
};

// "Installed Roblox": the version (and, with `detailed`, the build ID and
// when it was installed).
GtkWidget* BuildInstalledRobloxRow(LauncherContext* context, bool detailed);
// "Newest Roblox" with a Check button.
GtkWidget* BuildLatestRobloxRow(LauncherContext* context);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_NETWORK_UPDATES_STATUS_H_
