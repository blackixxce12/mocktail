#ifndef MOCKTAIL_LAUNCHER_UI_ACCOUNTS_CONTROLLER_H_
#define MOCKTAIL_LAUNCHER_UI_ACCOUNTS_CONTROLLER_H_

#include <adwaita.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "launcher_ui/accounts_model.h"
#include "launcher_ui/launcher_context.h"
#include "runtime/account_store.h"
#include "runtime/browser_sign_in.h"

// The saved Roblox accounts behind the Accounts page and the launch bar's
// account chip (accounts_page.cc), one per LauncherContext.
//
// It reads the store when the window opens, then, on a worker, moves a
// session from an older Mocktail into it and files what the last game
// changed (AccountStore::MigrateLegacySession and Reconcile: research/
// auth.md 5.5.1 and 5.5.6), and refreshes names and avatar headshots. None
// of that, and no sign-in, happens while the proxy for Roblox is not known
// (ResolveLauncherNetwork); it starts once a save, reload or restore makes
// it known. The
// selection and removals are staged in an AccountsModel and written by the
// "accounts" dirty source on Save, and the selection once more by the Play
// hook, so accounts/active never changes while the user is picking. "Add
// account" runs a BrowserSignInSession polled from a GLib timeout.
//
// Nothing here runs in --selftest except reading the store; nothing at all
// while MOCKTAIL_AUTH_ROOT, MOCKTAIL_COOKIE_FILE or MOCKTAIL_ROBLOX_COOKIES
// chooses the session (runtime::AccountStoreOverriddenByEnvironment).
// Single-threaded like the context: call it from the GTK main thread.
namespace mocktail::launcher_ui {

class AccountsController final
    : public std::enable_shared_from_this<AccountsController> {
 public:
  // The controller of `context`, made on first use. It registers the
  // "accounts" dirty source and the Play hook, which keep it alive as long
  // as the context.
  static std::shared_ptr<AccountsController> ForContext(
      LauncherContext* context);

  explicit AccountsController(LauncherContext* context);
  ~AccountsController();
  AccountsController(const AccountsController&) = delete;
  AccountsController& operator=(const AccountsController&) = delete;

  // ---- state ----------------------------------------------------------------
  LauncherContext* context() const { return context_; }
  const AccountsModel& model() const { return model_; }
  bool environment_override() const { return !override_variables_.empty(); }
  // The variables choosing the session (names only).
  const std::vector<std::string>& override_variables() const {
    return override_variables_;
  }
  // Why the store could not be read, empty when it could.
  const std::string& load_error() const { return load_error_; }
  // Why nothing is sent to Roblox from the window (the proxy is not known:
  // ResolveLauncherNetwork), empty when requests may go out.
  const std::string& network_blocked() const { return network_blocked_; }
  // The check after start (migration and reconcile) is running; Save and
  // Play wait for it.
  bool checking() const { return checking_; }
  bool signing_in() const { return sign_in_ != nullptr; }
  // Why an account cannot be added now, empty when it can.
  std::string AddUnavailableReason() const;
  // Why the selection cannot be changed, empty when it can.
  std::string SelectionUnavailableReason() const;
  AccountChipState chip() const;
  // The cached headshot, or nullptr (AdwAvatar then shows initials).
  GdkPaintable* AvatarFor(const runtime::SavedAccount& account);

  // ---- actions ------------------------------------------------------------
  // Stages the account (or guest) for the next Play.
  void Choose(const runtime::ActiveAccountPointer& pointer);
  // Opens the website sign-in window ("Add Roblox account"). With
  // `again_for`, the account whose session expired.
  void StartSignIn(std::optional<std::int64_t> again_for = std::nullopt);
  void CancelSignIn();
  // Asks, then stages the removal with an Undo toast.
  void ConfirmRemoval(std::int64_t user_id);
  // "Sign in inside Roblox": guest, then Play.
  void PlaySignedOut();

  // ---- observers ------------------------------------------------------------
  // Called after anything above changed (the page and the chip rebuild).
  using ObserverId = unsigned int;
  ObserverId AddObserver(std::function<void()> observer);
  void RemoveObserver(ObserverId id);

  // The page is being destroyed (the window closes): stops the sign-in
  // window, the timers and the worker. Afterwards the controller only
  // answers the context's dirty-source and Play callbacks.
  void Shutdown();

 private:
  struct Network;
  struct WorkerResult;
  struct WorkerSync;
  struct AvatarEntry {
    GdkTexture* texture = nullptr;
    std::filesystem::file_time_type modified;
  };

  void Register();
  void RegisterDirtySource();
  // Sets the proxy up for the window's requests; false (network_blocked_
  // says why) while it is not known.
  bool ConfigureNetwork();
  // After a save, reload or restore: the proxy may be known now.
  void RetryNetwork();
  void LoadStore();
  void StartWorker(bool startup);
  static void RunWorker(runtime::AccountStoreOptions options,
                        std::shared_ptr<Network> network, bool startup,
                        std::weak_ptr<AccountsController> self,
                        std::shared_ptr<WorkerSync> sync);
  static void PostResult(const std::weak_ptr<AccountsController>& self,
                         WorkerResult* result);
  void StageRemoval(std::int64_t user_id, const std::string& name);
  void StopWorker();
  void ApplyResult(WorkerResult& result);
  void FinishWorker();
  static gboolean PollSignIn(gpointer data);
  void FinishSignIn();
  bool SaveAccounts(std::string* error);
  bool SelectForPlay(std::string* error);
  void Notify();
  void Changed();

  LauncherContext* context_;
  bool usable_ = false;
  std::vector<std::string> override_variables_;
  std::string load_error_;
  LauncherNetworkBlock network_block_ = LauncherNetworkBlock::kNone;
  std::string network_blocked_;
  AccountsModel model_;
  std::optional<runtime::AccountStore> store_;
  std::shared_ptr<Network> network_;

  bool checking_ = false;
  bool worker_running_ = false;
  bool profiles_again_ = false;
  std::thread worker_;
  std::shared_ptr<WorkerSync> worker_sync_;

  std::unique_ptr<runtime::BrowserSignInSession> sign_in_;
  std::optional<std::int64_t> sign_in_again_for_;
  std::optional<services::AuthIdentity> signed_in_;
  std::shared_ptr<std::string> sign_in_store_error_;
  bool sign_in_unverified_shown_ = false;
  guint poll_source_ = 0;

  std::map<std::int64_t, AvatarEntry> avatars_;
  std::vector<std::pair<ObserverId, std::function<void()>>> observers_;
  ObserverId next_observer_ = 1;
  bool shut_down_ = false;
};

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ACCOUNTS_CONTROLLER_H_
