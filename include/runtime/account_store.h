#ifndef MOCKTAIL_RUNTIME_ACCOUNT_STORE_H_
#define MOCKTAIL_RUNTIME_ACCOUNT_STORE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_paths.h"
#include "services/auth_service.h"
#include "services/http_client.h"

// Saved Roblox accounts for the launcher. Layout under the base auth root
// (<data root>/auth, never an account-store MOCKTAIL_AUTH_ROOT):
//
//   accounts/                  0700
//     active                   0600  "guest" or a user id; the runtime contract
//     store.json               0600  order and the last launched slot
//     .store.lock              0600  held for every multi-file change
//     <user id>/               0700  that account's auth root
//       roblox.cookie          0600  ".ROBLOSECURITY=<value>\n"
//       account.json           0600  public names, state, timestamps
//     guest/                   0700  auth root of signed-out starts
//     .signed-out/             0700  the runtime's auth root while "active"
//                                    is unusable; emptied by each such
//                                    start, never filed into the store
//   <cache root>/avatars/<user id>.png   0700 directory, 0600 files
//
// Nothing here holds a session outside roblox.cookie. Directory names come
// only from validated user ids; names never reach a path.
namespace mocktail {
namespace runtime {

enum class SavedAccountState {
  kSignedIn,
  // The session was refused or moved to another account: sign in again.
  kSignedOut,
  // The session changed and Roblox could not be asked yet.
  kUnverified,
};

struct SavedAccount {
  std::int64_t user_id = 0;
  std::string username;
  std::string display_name;
  SavedAccountState state = SavedAccountState::kUnverified;
  std::int64_t added_at = 0;
  std::int64_t last_used_at = 0;
  std::int64_t last_verified_at = 0;
  std::int64_t profile_refreshed_at = 0;
  // Whether roblox.cookie holds a session. Without one the state is
  // kSignedOut whatever account.json says.
  bool has_session = false;
  // The cached headshot, or empty when there is none yet.
  std::filesystem::path avatar_file;
};

struct AccountStoreSnapshot {
  // accounts/active exists; until then the runtime uses the legacy root.
  bool initialized = false;
  // Missing or invalid pointers read as nullopt (the runtime then starts in
  // the guest slot, or in the legacy root when not initialized).
  std::optional<ActiveAccountPointer> active;
  // The slot whose session artifacts (WebKit jar, appStorage hydration) the
  // shared app data currently holds; nullopt when unknown or cleared.
  std::optional<ActiveAccountPointer> last_launched;
  std::vector<SavedAccount> accounts;
  // A session saved in the guest slot that Reconcile has not filed yet.
  bool guest_session_pending = false;
  // A legacy <auth root>/roblox.cookie session not migrated yet.
  bool legacy_session_present = false;
};

struct AccountStoreOptions {
  std::filesystem::path auth_root;
  std::filesystem::path app_storage_file;
  std::filesystem::path webview_data_directory;
  std::filesystem::path avatar_directory;
  // The caller guarantees no game instance runs (the launcher runs under the
  // instance lock its parent holds). Every change is refused otherwise.
  bool assume_exclusive = false;
  // Unix seconds; the system clock when empty.
  std::function<std::int64_t()> now;
};

// The launcher's options for base_paths, which must be resolved before the
// active account changes MOCKTAIL_AUTH_ROOT. The WebKit jar follows
// XDG_DATA_HOME like the helper; appStorage honours MOCKTAIL_RUNTIME_ROOT.
AccountStoreOptions MakeAccountStoreOptions(const RuntimePaths& base_paths,
                                            const Environment& environment,
                                            bool assume_exclusive);

enum class AccountMigrationOutcome {
  // No legacy session; the store exists (signed out unless it existed).
  kNothingToMigrate,
  // The legacy session now lives under its account, which is active unless
  // the store already existed with a usable selection.
  kMigrated,
  // Roblox refused the legacy session; it was deleted and guest is active
  // unless the store already existed.
  kRejected,
  // Roblox could not be asked; nothing changed and the runtime keeps using
  // the legacy root.
  kPostponed,
};

struct AccountMigrationResult {
  AccountMigrationOutcome outcome = AccountMigrationOutcome::kPostponed;
  std::int64_t user_id = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct AccountReconcileResult {
  // Accounts that received a session from the guest slot or from another
  // account's slot (a sign-in inside Roblox).
  std::vector<std::int64_t> filed;
  // Accounts whose session was refused or replaced.
  std::vector<std::int64_t> signed_out;
  // Accounts whose changed session Roblox confirmed.
  std::vector<std::int64_t> verified;
  // Some session could not be checked; Reconcile tries again next time.
  bool pending = false;
  bool active_changed = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct ProfileRefreshResult {
  int names_updated = 0;
  int avatars_updated = 0;
  int failed = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

class AccountStore final {
 public:
  explicit AccountStore(AccountStoreOptions options);

  const AccountStoreOptions& options() const { return options_; }
  std::filesystem::path accounts_directory() const;

  // Reads the store without changing it. Safe while a game runs.
  bool Load(AccountStoreSnapshot* snapshot, std::string* error) const;

  // Moves the single legacy <auth root>/roblox.cookie session into the store
  // once. The caller skips this while an environment override chooses the
  // session (AccountStoreOverriddenByEnvironment). The pointer is written
  // last and the legacy file deleted after it, so an interruption leaves
  // either the legacy layout or a complete store. A legacy session that
  // appears once the store exists (an older Mocktail, or a run whose session
  // came from MOCKTAIL_COOKIE_FILE or MOCKTAIL_ROBLOX_COOKIES, saved it) is
  // filed too, but replaces only an unusable selection, and the next start
  // clears the session artifacts that run left (Load reports it first as
  // legacy_session_present on an initialized store).
  AccountMigrationResult MigrateLegacySession(
      services::AuthService& auth_service) const;

  // Files what the last run changed: a session saved in the guest slot moves
  // under its account and becomes active; a slot whose session changed is
  // checked again, and a session that now belongs to another account moves
  // there; refused or retired sessions mark their account signed out.
  // Sessions go only to users/authenticated, outside the store lock.
  AccountReconcileResult Reconcile(services::AuthService& auth_service) const;

  // Saves a session Roblox has just accepted for identity (from a browser
  // sign-in), replacing that account's previous one. make_active selects it
  // for the next start. Safe to call from the sign-in worker.
  bool AddValidatedSession(const services::AuthIdentity& identity,
                           std::string_view cookie_value, bool make_active,
                           std::string* error) const;

  // Selects the slot the next start uses. Starting another account than the
  // shared app data last saw, or a guest, first clears the session artifacts
  // another account could leak through (ClearSessionArtifacts).
  bool SelectForLaunch(const ActiveAccountPointer& selection,
                       std::string* error) const;

  // Clears the session artifacts before a browser sign-in adds an account,
  // so WebKit cannot report the account it already holds. Call it before the
  // sign-in window opens, never while it runs.
  bool PrepareBrowserSignIn(std::string* error) const;

  // Removes an account: its directory (only the names the store and the
  // runtime create; anything else stops the removal untouched), its avatar,
  // and the session artifacts. An active account is replaced by the first
  // remaining one, or guest. There is no server-side sign-out.
  bool RemoveAccount(std::int64_t user_id, std::string* error) const;

  // Removes PlayerHydrationBlob and PlayerHydrationSignature from
  // appStorage.json under its lock, and deletes WebKit's cookie jar
  // (cookies.sqlite and its journals). Other appStorage keys stay.
  bool ClearSessionArtifacts(std::string* error) const;

  // Refreshes public names and avatar headshots, at most daily per account
  // unless forced. Sends no session. Blocking: call it from a worker.
  ProfileRefreshResult RefreshProfiles(services::HttpClient& http_client,
                                       bool force) const;

 private:
  std::int64_t Now() const;

  AccountStoreOptions options_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ACCOUNT_STORE_H_
