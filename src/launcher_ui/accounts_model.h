#ifndef MOCKTAIL_LAUNCHER_UI_ACCOUNTS_MODEL_H_
#define MOCKTAIL_LAUNCHER_UI_ACCOUNTS_MODEL_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/account_store.h"
#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "services/http_client.h"

// The GTK-free part of the Accounts page and the launch bar's account chip:
// the staged account selection, what the chip shows, how long ago an
// account played, and the network settings for the window's own requests to
// Roblox. The store itself is runtime/account_store.h; nothing here touches
// files except PlanLauncherNetwork, which only reads config.yaml.
namespace mocktail::launcher_ui {

enum class AccountStatus {
  kReady,
  // No session, or Roblox refused it: sign in again.
  kSignedOut,
  // The session changed and Roblox could not be asked yet.
  kUnverified,
};

AccountStatus StatusOf(const runtime::SavedAccount& account);

// The display name, else the username, else the user id.
std::string ShownName(const runtime::SavedAccount& account);

// The account the next Play starts with and the accounts to remove, staged
// like the settings: the store (accounts/active, the account folders)
// changes only on Save or Play, never while the user picks.
class AccountsModel {
 public:
  // A removal the user confirmed but did not save yet, with what Undo puts
  // back.
  struct StagedRemoval {
    std::int64_t user_id = 0;
    std::optional<runtime::ActiveAccountPointer> previous_choice;
  };

  // Takes the store's state (start, a check that finished, a save). A
  // choice that still names guest or a saved account stays, unless the
  // store now holds it anyway; staged removals of accounts that are gone
  // are dropped.
  void SetSnapshot(runtime::AccountStoreSnapshot snapshot);
  const runtime::AccountStoreSnapshot& snapshot() const { return snapshot_; }

  // Saved accounts in the store's order, without those staged for removal.
  std::vector<const runtime::SavedAccount*> VisibleAccounts() const;
  const runtime::SavedAccount* FindVisible(std::int64_t user_id) const;

  // What accounts/active selects now, when the runtime can use it: nullopt
  // without a store (the legacy auth root) and for a pointer to an account
  // that is not saved (the runtime starts signed out then).
  std::optional<runtime::ActiveAccountPointer> SavedSelection() const;
  // What Play uses: the user's choice, else the saved selection.
  std::optional<runtime::ActiveAccountPointer> Selection() const;
  bool IsSelected(const runtime::ActiveAccountPointer& pointer) const;
  // Ignored for an account that is not listed.
  void Choose(const runtime::ActiveAccountPointer& pointer);
  // The choice differs from what accounts/active holds.
  bool SelectionChanged() const;

  // Hides the account until Save. When it was selected, the selection moves
  // where the store moves it on removal: the first remaining account, else
  // guest. nullopt when the account is not listed.
  std::optional<StagedRemoval> StageRemoval(std::int64_t user_id);
  void UndoRemoval(const StagedRemoval& removal);
  // The store removed it.
  void FinishRemoval(std::int64_t user_id);
  const std::vector<std::int64_t>& staged_removals() const { return removals_; }

  // Staged removals plus one for a changed selection.
  int UnsavedCount() const;
  void Discard();

 private:
  bool Listed(const runtime::ActiveAccountPointer& pointer) const;
  bool Saved(std::int64_t user_id) const;

  runtime::AccountStoreSnapshot snapshot_;
  std::optional<runtime::ActiveAccountPointer> choice_;
  std::vector<std::int64_t> removals_;
};

// What the launch bar's account chip shows.
enum class AccountChipKind {
  // MOCKTAIL_AUTH_ROOT, MOCKTAIL_COOKIE_FILE or MOCKTAIL_ROBLOX_COOKIES
  // chooses the session; the saved accounts are not used.
  kEnvironment,
  // The store could not be read.
  kUnavailable,
  kAccount,
  // Guest is selected and accounts are saved.
  kGuest,
  // Nothing is saved, or the selection is unusable.
  kNotSignedIn,
  // A session from an older Mocktail that is not in the store yet; the
  // runtime keeps using it.
  kLegacySession,
};

struct AccountChipState {
  AccountChipKind kind = AccountChipKind::kNotSignedIn;
  // kAccount only.
  const runtime::SavedAccount* account = nullptr;
};

AccountChipState DescribeAccountChip(const AccountsModel& model,
                                     bool environment_override,
                                     bool load_failed);

// The variables that choose the session instead of the account store
// (names only: MOCKTAIL_ROBLOX_COOKIES holds a session).
std::vector<std::string> AccountOverrideVariables(
    const runtime::Environment& environment);

// "Last played …" in the largest whole unit.
struct LastPlayed {
  enum class Kind { kNever, kJustNow, kMinutes, kHours, kDays, kDate };
  Kind kind = Kind::kNever;
  std::int64_t count = 0;
};

LastPlayed DescribeLastPlayed(std::int64_t played_at, std::int64_t now);

// The network setup mocktail does before Roblox starts (main.cc: the
// configured or system proxy and the CA bundle), for the window's own
// requests to Roblox: checking saved sessions, names and avatars, and the
// sign-in window. Variables the environment already sets are kept.
struct LauncherNetworkPlan {
  std::vector<std::pair<std::string, std::string>> assignments;
  // network.use_system_proxy: ResolveSystemProxy() decides the proxy.
  bool system_proxy = false;
  // config.yaml could not be loaded; requests go out directly.
  std::string error;
};

LauncherNetworkPlan PlanLauncherNetwork(
    const runtime::Environment& environment,
    const std::filesystem::path& config_file);

// An HttpClient for work that must be able to stop: once Cancel() is
// called every new request fails at once without reaching the network, so a
// worker can be waited for after at most the request it is in, which takes
// at most `maximum_timeout_ms`.
class CancellableHttpClient final : public services::HttpClient {
 public:
  CancellableHttpClient(services::HttpClient& client, long maximum_timeout_ms)
      : client_(client), maximum_timeout_ms_(maximum_timeout_ms) {}

  services::HttpResponse Get(const services::HttpRequest& request) override;
  services::HttpResponse Post(const services::HttpRequest& request,
                              const std::string& body) override;

  void Cancel() { cancelled_.store(true); }
  bool cancelled() const { return cancelled_.load(); }

 private:
  services::HttpRequest Limit(const services::HttpRequest& request) const;

  services::HttpClient& client_;
  long maximum_timeout_ms_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ACCOUNTS_MODEL_H_
