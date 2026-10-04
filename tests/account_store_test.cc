#include "runtime/account_store.h"

#define JSON_NOEXCEPTION 1
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/browser_sign_in.h"
#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "services/auth_service.h"
#include "services/http_client.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

constexpr char kSessionA[] = "_|test-account-store-session-a";
constexpr char kSessionA2[] = "_|test-account-store-session-a-rotated";
constexpr char kSessionB[] = "_|test-account-store-session-b";
constexpr char kTracker[] = "RBXEventTrackerV2=CreateDate=1&rbxid=&browserid=7";
constexpr char kAuthenticatedUrl[] =
    "https://users.roblox.com/v1/users/authenticated";
constexpr char kPng[] = "\x89PNG\r\n\x1a\nfake-image-bytes";

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    if (found == values_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

 private:
  std::map<std::string, std::string> values_;
};

// Routes requests by URL; users/authenticated answers by the session sent.
class FakeRoblox final : public services::HttpClient {
 public:
  services::HttpResponse Get(const services::HttpRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(request);
    if (request.url == kAuthenticatedUrl) {
      std::string session;
      for (const std::string& header : request.headers) {
        constexpr std::string_view kCookie = "Cookie: .ROBLOSECURITY=";
        if (header.rfind(kCookie, 0) == 0) {
          session = header.substr(kCookie.size());
        }
      }
      if (offline_) {
        return {false, 0, {}, "offline"};
      }
      const auto found = sessions_.find(session);
      if (found == sessions_.end()) {
        return {true, 401, R"({"errors":[]})", {}};
      }
      return {true, 200, IdentityJson(found->second), {}};
    }
    if (offline_) {
      return {false, 0, {}, "offline"};
    }
    const auto found = routes_.find(request.url);
    if (found == routes_.end()) {
      return {true, 404, "{}", {}};
    }
    return found->second;
  }

  void AddSession(std::string session, std::int64_t user_id,
                  std::string username, std::string display_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    services::AuthIdentity identity;
    identity.user_id = user_id;
    identity.username = std::move(username);
    identity.display_name = std::move(display_name);
    sessions_[std::move(session)] = std::move(identity);
  }
  void Route(std::string url, services::HttpResponse response) {
    std::lock_guard<std::mutex> lock(mutex_);
    routes_[std::move(url)] = std::move(response);
  }
  void SetOffline(bool offline) {
    std::lock_guard<std::mutex> lock(mutex_);
    offline_ = offline;
  }
  std::vector<services::HttpRequest> requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }
  void ClearRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
  }

 private:
  static std::string IdentityJson(const services::AuthIdentity& identity) {
    Json body = Json::object();
    body["id"] = identity.user_id;
    body["name"] = identity.username;
    body["displayName"] = identity.display_name;
    return body.dump();
  }

  std::mutex mutex_;
  bool offline_ = false;
  std::map<std::string, services::AuthIdentity> sessions_;
  std::map<std::string, services::HttpResponse> routes_;
  std::vector<services::HttpRequest> requests_;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_account_store_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void WritePrivate(const std::filesystem::path& path,
                  const std::string& contents, mode_t mode = 0600) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
  ASSERT_EQ(chmod(path.c_str(), mode), 0);
}

mode_t ModeOf(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? (status.st_mode & 07777) : 0;
}

bool Exists(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0;
}

Json ReadJson(const std::filesystem::path& path) {
  return Json::parse(ReadFile(path), nullptr, false);
}

std::string Canonical(std::string_view session) {
  return ".ROBLOSECURITY=" + std::string(session) + "\n";
}

services::AuthIdentity Identity(std::int64_t user_id, std::string username,
                                std::string display_name) {
  services::AuthIdentity identity;
  identity.user_id = user_id;
  identity.username = std::move(username);
  identity.display_name = std::move(display_name);
  return identity;
}

ActiveAccountPointer Account(std::int64_t user_id) {
  ActiveAccountPointer pointer;
  pointer.guest = false;
  pointer.user_id = user_id;
  return pointer;
}

class AccountStoreTest : public ::testing::Test {
 protected:
  AccountStoreTest() : auth_service_(roblox_) {
    root_ = temporary_.path();
    auth_root_ = root_ / "data/mocktail/auth";
    accounts_ = auth_root_ / "accounts";
    app_storage_ =
        root_ /
        "data/mocktail/android/data/files/appData/LocalStorage/appStorage.json";
    webview_ = root_ / "data/mocktail/webview";
    avatars_ = root_ / "cache/mocktail/avatars";
    roblox_.AddSession(kSessionA, 42, "alpha", "Alpha");
    roblox_.AddSession(kSessionA2, 42, "alpha", "Alpha Renamed");
    roblox_.AddSession(kSessionB, 77, "bravo", "Bravo");
  }

  AccountStoreOptions Options(bool exclusive = true) {
    AccountStoreOptions options;
    options.auth_root = auth_root_;
    options.app_storage_file = app_storage_;
    options.webview_data_directory = webview_;
    options.avatar_directory = avatars_;
    options.assume_exclusive = exclusive;
    options.now = [this]() { return now_; };
    return options;
  }

  AccountStore Store(bool exclusive = true) {
    return AccountStore(Options(exclusive));
  }

  ActiveAccountResolution Resolve() {
    const MapEnvironment environment(
        {{"HOME", root_.string()},
         {"XDG_DATA_HOME", (root_ / "data").string()},
         {"XDG_CACHE_HOME", (root_ / "cache").string()}});
    return ResolveActiveAccountAuthRoot(
        RuntimePaths::FromEnvironment(environment, root_), environment);
  }

  void WriteLegacy(const std::string& contents, mode_t mode = 0600) {
    WritePrivate(auth_root_ / "roblox.cookie", contents, mode);
    ASSERT_EQ(chmod(auth_root_.c_str(), 0700), 0);
  }

  // A store as the launcher leaves it, with A (42) and B (77) saved.
  void SeedTwoAccounts() {
    AccountStore store = Store();
    std::string error;
    ASSERT_TRUE(store.AddValidatedSession(Identity(42, "alpha", "Alpha"),
                                          kSessionA, true, &error))
        << error;
    ASSERT_TRUE(store.AddValidatedSession(Identity(77, "bravo", "Bravo"),
                                          kSessionB, false, &error))
        << error;
  }

  void WriteSessionArtifacts() {
    Json storage = Json::object();
    storage["PlayerHydrationBlob"] = "blob";
    storage["PlayerHydrationSignature"] = "signature";
    storage["BrowserTrackerId"] = "7";
    storage["AppConfiguration"] = Json::object({{"GUAC:42:app-policy", "x"}});
    storage["DeviceLevelTheme"] = "{\"42\":\"Dark\"}";
    WritePrivate(app_storage_, storage.dump());
    WritePrivate(webview_ / "cookies.sqlite", "jar");
    WritePrivate(webview_ / "cookies.sqlite-wal", "wal");
    WritePrivate(webview_ / "cookies.sqlite-shm", "shm");
    WritePrivate(webview_ / "storage/salt", "salt");
  }

  bool ArtifactsCleared() {
    const Json storage = ReadJson(app_storage_);
    return storage.is_object() && !storage.contains("PlayerHydrationBlob") &&
           !storage.contains("PlayerHydrationSignature") &&
           !Exists(webview_ / "cookies.sqlite") &&
           !Exists(webview_ / "cookies.sqlite-wal") &&
           !Exists(webview_ / "cookies.sqlite-shm");
  }

  bool NoRequestCarriedASession() {
    for (const services::HttpRequest& request : roblox_.requests()) {
      for (const std::string& header : request.headers) {
        if (header.find("ROBLOSECURITY") != std::string::npos &&
            request.url != kAuthenticatedUrl) {
          return false;
        }
      }
      if (request.url.find("_|") != std::string::npos) {
        return false;
      }
    }
    return true;
  }

  TemporaryDirectory temporary_;
  std::filesystem::path root_;
  std::filesystem::path auth_root_;
  std::filesystem::path accounts_;
  std::filesystem::path app_storage_;
  std::filesystem::path webview_;
  std::filesystem::path avatars_;
  std::int64_t now_ = 1700000000;
  FakeRoblox roblox_;
  services::AuthService auth_service_;
};

TEST_F(AccountStoreTest, MigratesAValidLegacySessionUnderItsAccount) {
  WriteLegacy(Canonical(kSessionA) + kTracker + "\n");
  AccountStore store = Store();

  const AccountMigrationResult result =
      store.MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kMigrated);
  EXPECT_EQ(result.user_id, 42);

  // The tracker line is not carried over: account files hold one session.
  EXPECT_EQ(ReadFile(accounts_ / "42/roblox.cookie"), Canonical(kSessionA));
  EXPECT_EQ(ModeOf(accounts_ / "42/roblox.cookie"), 0600u);
  EXPECT_EQ(ModeOf(accounts_ / "42"), 0700u);
  EXPECT_EQ(ModeOf(accounts_), 0700u);
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
  EXPECT_EQ(ModeOf(accounts_ / "active"), 0600u);
  EXPECT_FALSE(Exists(auth_root_ / "roblox.cookie"));
  EXPECT_FALSE(Exists(auth_root_ / ".roblox.cookie.mocktail-writer.lock"));

  const Json account = ReadJson(accounts_ / "42/account.json");
  EXPECT_EQ(account["user_id"], 42);
  EXPECT_EQ(account["username"], "alpha");
  EXPECT_EQ(account["display_name"], "Alpha");
  EXPECT_EQ(account["state"], "signed_in");
  EXPECT_EQ(account["added_at"], now_);
  EXPECT_EQ(account["credential_sha256"].get<std::string>().size(), 64u);
  EXPECT_EQ(ReadFile(accounts_ / "42/account.json").find(kSessionA),
            std::string::npos);
  const Json listing = ReadJson(accounts_ / "store.json");
  EXPECT_EQ(listing["order"], Json::array({42}));
  EXPECT_EQ(listing["last_launched"], "42");

  const ActiveAccountResolution resolution = Resolve();
  ASSERT_TRUE(resolution) << resolution.error;
  EXPECT_EQ(resolution.kind, ActiveAccountKind::kAccount);
  EXPECT_EQ(resolution.auth_root, accounts_ / "42");

  // Once migrated there is nothing left to do.
  roblox_.ClearRequests();
  const AccountMigrationResult again =
      store.MigrateLegacySession(auth_service_);
  ASSERT_TRUE(again) << again.error;
  EXPECT_EQ(again.outcome, AccountMigrationOutcome::kNothingToMigrate);
  EXPECT_TRUE(roblox_.requests().empty());
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
}

TEST_F(AccountStoreTest, ARejectedLegacySessionStartsTheStoreSignedOut) {
  WriteLegacy(Canonical("_|revoked"));
  const AccountMigrationResult result =
      Store().MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kRejected);
  EXPECT_EQ(ReadFile(accounts_ / "active"), "guest\n");
  EXPECT_FALSE(Exists(auth_root_ / "roblox.cookie"));
  EXPECT_FALSE(Exists(accounts_ / "42"));
  EXPECT_EQ(Resolve().kind, ActiveAccountKind::kGuest);
}

TEST_F(AccountStoreTest, OfflineMigrationChangesNothing) {
  WriteLegacy(Canonical(kSessionA));
  roblox_.SetOffline(true);
  const AccountMigrationResult result =
      Store().MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kPostponed);
  EXPECT_EQ(ReadFile(auth_root_ / "roblox.cookie"), Canonical(kSessionA));
  EXPECT_FALSE(Exists(accounts_));
  EXPECT_EQ(Resolve().kind, ActiveAccountKind::kLegacy);
}

TEST_F(AccountStoreTest, WithoutALegacySessionTheStoreStartsAsGuest) {
  AccountStore store = Store();
  AccountMigrationResult result = store.MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kNothingToMigrate);
  EXPECT_EQ(ReadFile(accounts_ / "active"), "guest\n");
  EXPECT_TRUE(roblox_.requests().empty());

  // A retired legacy file (no session left) is simply removed.
  WriteLegacy(std::string(60, ' ') + "\n");
  result = store.MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kNothingToMigrate);
  EXPECT_FALSE(Exists(auth_root_ / "roblox.cookie"));
  EXPECT_TRUE(roblox_.requests().empty());
}

TEST_F(AccountStoreTest, ALegacySessionSavedAfterMigrationIsFiledToo) {
  // An older Mocktail, or a run whose session came from MOCKTAIL_COOKIE_FILE
  // or MOCKTAIL_ROBLOX_COOKIES, writes the legacy file again after the
  // migration.
  SeedTwoAccounts();
  WriteLegacy(Canonical(kSessionB));
  const AccountMigrationResult result =
      Store().MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kMigrated);
  EXPECT_EQ(result.user_id, 77);
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));
  EXPECT_FALSE(Exists(auth_root_ / "roblox.cookie"));
  // The user's selection stays, and the app data that run left belongs to
  // no slot the store knows.
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
  EXPECT_TRUE(ReadJson(accounts_ / "store.json")["last_launched"].is_null());

  // A rejected one is dropped without changing the selection.
  std::string error;
  ASSERT_TRUE(Store().SelectForLaunch(Account(42), &error)) << error;
  WriteLegacy(Canonical("_|revoked"));
  const AccountMigrationResult rejected =
      Store().MigrateLegacySession(auth_service_);
  ASSERT_TRUE(rejected) << rejected.error;
  EXPECT_EQ(rejected.outcome, AccountMigrationOutcome::kRejected);
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
  EXPECT_TRUE(ReadJson(accounts_ / "store.json")["last_launched"].is_null());
  EXPECT_FALSE(Exists(auth_root_ / "roblox.cookie"));
}

// The first launcher start was offline, so the legacy session waited while
// the user played as a guest and signed in inside Roblox as someone else.
TEST_F(AccountStoreTest, ALateMigrationKeepsTheSelectionAndClearsTheAppData) {
  WriteLegacy(Canonical(kSessionA));
  roblox_.SetOffline(true);
  AccountStore store = Store();
  ASSERT_EQ(store.MigrateLegacySession(auth_service_).outcome,
            AccountMigrationOutcome::kPostponed);
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(ActiveAccountPointer{}, &error)) << error;
  WriteSessionArtifacts();

  roblox_.SetOffline(false);
  const AccountMigrationResult result =
      store.MigrateLegacySession(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.outcome, AccountMigrationOutcome::kMigrated);
  EXPECT_EQ(result.user_id, 42);
  EXPECT_EQ(ReadFile(accounts_ / "active"), "guest\n");
  EXPECT_TRUE(ReadJson(accounts_ / "store.json")["last_launched"].is_null());

  // The guest run's hydration data and web session never reach alpha.
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  EXPECT_TRUE(ArtifactsCleared());
}

// A selection that cannot be used anyway goes to the migrated account.
TEST_F(AccountStoreTest, ALateMigrationReplacesAnUnusableSelection) {
  for (const std::string pointer : {"garbage\n", "99\n"}) {
    SCOPED_TRACE(pointer);
    std::error_code ignored;
    std::filesystem::remove_all(auth_root_, ignored);
    SeedTwoAccounts();
    WritePrivate(accounts_ / "active", pointer);
    WriteLegacy(Canonical(kSessionB));
    const AccountMigrationResult result =
        Store().MigrateLegacySession(auth_service_);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.outcome, AccountMigrationOutcome::kMigrated);
    EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
    EXPECT_TRUE(ReadJson(accounts_ / "store.json")["last_launched"].is_null());
  }
}

TEST_F(AccountStoreTest, MigrationRefusesUnsafeOrSharedState) {
  WriteLegacy(Canonical(kSessionA), 0640);
  AccountMigrationResult result = Store().MigrateLegacySession(auth_service_);
  EXPECT_FALSE(result);
  EXPECT_TRUE(roblox_.requests().empty());
  EXPECT_FALSE(Exists(accounts_));

  ASSERT_EQ(chmod((auth_root_ / "roblox.cookie").c_str(), 0600), 0);
  result = Store(false).MigrateLegacySession(auth_service_);
  EXPECT_FALSE(result);
  EXPECT_FALSE(Exists(accounts_));
  EXPECT_EQ(ReadFile(auth_root_ / "roblox.cookie"), Canonical(kSessionA));
}

TEST_F(AccountStoreTest, ReconcileFilesAGuestSlotSignIn) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(ActiveAccountPointer{}, &error)) << error;
  // A native sign-in during the guest start saved this session.
  WritePrivate(accounts_ / "guest/roblox.cookie", Canonical(kSessionB));
  ASSERT_EQ(chmod((accounts_ / "guest").c_str(), 0700), 0);

  AccountStoreSnapshot snapshot;
  ASSERT_TRUE(store.Load(&snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.guest_session_pending);

  const AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.filed, std::vector<std::int64_t>{77});
  EXPECT_TRUE(result.active_changed);
  EXPECT_FALSE(result.pending);
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));
  EXPECT_FALSE(Exists(accounts_ / "guest/roblox.cookie"));
  EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["last_launched"], "77");
  ASSERT_TRUE(store.Load(&snapshot, &error)) << error;
  EXPECT_FALSE(snapshot.guest_session_pending);
}

TEST_F(AccountStoreTest,
       ReconcileDropsARejectedGuestSessionAndKeepsAnUnverifiedOne) {
  SeedTwoAccounts();
  AccountStore store = Store();
  WritePrivate(accounts_ / "guest/roblox.cookie", Canonical("_|revoked"));
  ASSERT_EQ(chmod((accounts_ / "guest").c_str(), 0700), 0);
  AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(Exists(accounts_ / "guest/roblox.cookie"));
  EXPECT_TRUE(result.filed.empty());

  WritePrivate(accounts_ / "guest/roblox.cookie", Canonical(kSessionB));
  roblox_.SetOffline(true);
  result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.pending);
  EXPECT_EQ(ReadFile(accounts_ / "guest/roblox.cookie"), Canonical(kSessionB));
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
}

TEST_F(AccountStoreTest, ReconcileMovesASessionThatBelongsToAnotherAccount) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  ASSERT_TRUE(store.RemoveAccount(77, &error)) << error;
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  // Inside Roblox the user signed out of A and into B in A's slot.
  WritePrivate(accounts_ / "42/roblox.cookie", Canonical(kSessionB));

  const AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.filed, std::vector<std::int64_t>{77});
  EXPECT_EQ(result.signed_out, std::vector<std::int64_t>{42});
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));
  EXPECT_FALSE(Exists(accounts_ / "42/roblox.cookie"));
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["state"], "signed_out");
  EXPECT_FALSE(
      ReadJson(accounts_ / "42/account.json").contains("credential_sha256"));
  EXPECT_EQ(ReadJson(accounts_ / "77/account.json")["username"], "bravo");
  EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["last_launched"], "77");

  AccountStoreSnapshot snapshot;
  ASSERT_TRUE(store.Load(&snapshot, &error)) << error;
  ASSERT_EQ(snapshot.accounts.size(), 2u);
  EXPECT_EQ(snapshot.accounts[0].user_id, 42);
  EXPECT_FALSE(snapshot.accounts[0].has_session);
  EXPECT_EQ(snapshot.accounts[0].state, SavedAccountState::kSignedOut);
  EXPECT_EQ(snapshot.accounts[1].user_id, 77);
  EXPECT_EQ(snapshot.accounts[1].state, SavedAccountState::kSignedIn);
}

TEST_F(AccountStoreTest, ReconcileMarksARetiredSessionSignedOut) {
  SeedTwoAccounts();
  // The runtime redacts a refused session in place.
  WritePrivate(accounts_ / "42/roblox.cookie",
               std::string(Canonical(kSessionA).size() - 1, ' ') + "\n");
  AccountStore store = Store();
  const AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.signed_out, std::vector<std::int64_t>{42});
  EXPECT_TRUE(roblox_.requests().empty());
  const Json account = ReadJson(accounts_ / "42/account.json");
  EXPECT_EQ(account["state"], "signed_out");
  EXPECT_EQ(account["username"], "alpha");
  EXPECT_TRUE(Exists(accounts_ / "42"));
}

TEST_F(AccountStoreTest, ReconcileChecksARotatedSessionOnce) {
  SeedTwoAccounts();
  // The engine rotated A's session during the run (and the tracker line was
  // appended by the runtime).
  WritePrivate(accounts_ / "42/roblox.cookie",
               Canonical(kSessionA2) + kTracker + "\n");
  AccountStore store = Store();
  roblox_.ClearRequests();
  AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.verified, std::vector<std::int64_t>{42});
  ASSERT_EQ(roblox_.requests().size(), 1u);
  // The runtime's file is left alone; only the record changes.
  EXPECT_EQ(ReadFile(accounts_ / "42/roblox.cookie"),
            Canonical(kSessionA2) + kTracker + "\n");
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["display_name"],
            "Alpha Renamed");

  roblox_.ClearRequests();
  result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.verified.empty());
  EXPECT_TRUE(roblox_.requests().empty());
}

TEST_F(AccountStoreTest, ReconcileRetiresAChangedSessionRobloxRefuses) {
  SeedTwoAccounts();
  WritePrivate(accounts_ / "42/roblox.cookie",
               Canonical("_|revoked") + kTracker + "\n");
  const AccountReconcileResult result = Store().Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.signed_out, std::vector<std::int64_t>{42});
  const std::string retired = ReadFile(accounts_ / "42/roblox.cookie");
  EXPECT_EQ(retired.find("ROBLOSECURITY"), std::string::npos);
  EXPECT_NE(retired.find("RBXEventTrackerV2"), std::string::npos);
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["state"], "signed_out");
}

TEST_F(AccountStoreTest, ReconcileLeavesAnUncheckedSessionUnverified) {
  SeedTwoAccounts();
  WritePrivate(accounts_ / "42/roblox.cookie", Canonical(kSessionA2));
  roblox_.SetOffline(true);
  AccountStore store = Store();
  AccountReconcileResult result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.pending);
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["state"], "unverified");
  EXPECT_EQ(ReadFile(accounts_ / "42/roblox.cookie"), Canonical(kSessionA2));

  roblox_.SetOffline(false);
  result = store.Reconcile(auth_service_);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.verified, std::vector<std::int64_t>{42});
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["state"], "signed_in");
}

TEST_F(AccountStoreTest, SwitchingAccountsClearsOnlyTheSessionArtifacts) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  WriteSessionArtifacts();

  // Starting the account the app data already belongs to keeps it.
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  EXPECT_TRUE(Exists(webview_ / "cookies.sqlite"));
  EXPECT_TRUE(ReadJson(app_storage_).contains("PlayerHydrationBlob"));

  ASSERT_TRUE(store.SelectForLaunch(Account(77), &error)) << error;
  EXPECT_TRUE(ArtifactsCleared());
  const Json storage = ReadJson(app_storage_);
  EXPECT_EQ(storage["BrowserTrackerId"], "7");
  EXPECT_EQ(storage["AppConfiguration"]["GUAC:42:app-policy"], "x");
  EXPECT_EQ(storage["DeviceLevelTheme"], "{\"42\":\"Dark\"}");
  EXPECT_EQ(ModeOf(app_storage_), 0600u);
  EXPECT_EQ(ReadFile(webview_ / "storage/salt"), "salt");
  EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["last_launched"], "77");
  EXPECT_EQ(ReadJson(accounts_ / "77/account.json")["last_used_at"], now_);

  const ActiveAccountResolution resolution = Resolve();
  ASSERT_TRUE(resolution) << resolution.error;
  EXPECT_EQ(resolution.auth_root, accounts_ / "77");
}

TEST_F(AccountStoreTest, EveryGuestStartGetsACleanJar) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(ActiveAccountPointer{}, &error)) << error;
  WriteSessionArtifacts();
  ASSERT_TRUE(store.SelectForLaunch(ActiveAccountPointer{}, &error)) << error;
  EXPECT_TRUE(ArtifactsCleared());
  EXPECT_EQ(ReadFile(accounts_ / "active"), "guest\n");
  EXPECT_EQ(Resolve().kind, ActiveAccountKind::kGuest);
}

TEST_F(AccountStoreTest, SelectingRequiresASavedAccountAndExclusiveAccess) {
  SeedTwoAccounts();
  std::string error;
  EXPECT_FALSE(Store().SelectForLaunch(Account(99), &error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(Store(false).SelectForLaunch(Account(77), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
  EXPECT_FALSE(Store(false).ClearSessionArtifacts(&error));
  EXPECT_FALSE(Store(false).RemoveAccount(42, &error));
  EXPECT_FALSE(Store(false).AddValidatedSession(Identity(5, "e", "E"),
                                                kSessionA, true, &error));
  EXPECT_FALSE(Store(false).PrepareBrowserSignIn(&error));
  EXPECT_FALSE(Store(false).Reconcile(auth_service_));
  EXPECT_FALSE(Store(false).RefreshProfiles(roblox_, true));
}

TEST_F(AccountStoreTest,
       BrowserSignInStartsFromACleanJarAndForcesTheNextClear) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  WriteSessionArtifacts();
  ASSERT_TRUE(store.PrepareBrowserSignIn(&error)) << error;
  EXPECT_TRUE(ArtifactsCleared());
  EXPECT_TRUE(ReadJson(accounts_ / "store.json")["last_launched"].is_null());

  // The sign-in left another account's session in the jar.
  WritePrivate(webview_ / "cookies.sqlite", "jar");
  ASSERT_TRUE(store.SelectForLaunch(Account(42), &error)) << error;
  EXPECT_FALSE(Exists(webview_ / "cookies.sqlite"));
}

TEST_F(AccountStoreTest, AddsValidatedSessions) {
  AccountStore store = Store();
  std::string error;
  // Without the pointer the runtime keeps the legacy root, so an account
  // added without selecting it leaves the store uninitialized.
  ASSERT_TRUE(store.AddValidatedSession(Identity(77, "bravo", "Bravo"),
                                        Canonical(kSessionB), false, &error))
      << error;
  EXPECT_FALSE(Exists(accounts_ / "active"));
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));

  ASSERT_TRUE(store.AddValidatedSession(Identity(42, "alpha", "Alpha"),
                                        kSessionA, true, &error))
      << error;
  EXPECT_EQ(ReadFile(accounts_ / "42/roblox.cookie"), Canonical(kSessionA));
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["order"], Json::array({77, 42}));
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["last_launched"], "42");

  // Signing in again replaces the session and keeps when it was added.
  now_ += 100;
  ASSERT_TRUE(store.AddValidatedSession(Identity(42, "alpha", "Alpha 2"),
                                        kSessionA2, true, &error))
      << error;
  EXPECT_EQ(ReadFile(accounts_ / "42/roblox.cookie"), Canonical(kSessionA2));
  const Json account = ReadJson(accounts_ / "42/account.json");
  EXPECT_EQ(account["added_at"], now_ - 100);
  EXPECT_EQ(account["last_verified_at"], now_);
  EXPECT_EQ(account["display_name"], "Alpha 2");

  EXPECT_FALSE(store.AddValidatedSession(Identity(0, "zero", ""), kSessionA,
                                         true, &error));
  EXPECT_FALSE(
      store.AddValidatedSession(Identity(9, "", ""), kSessionA, true, &error));
  EXPECT_FALSE(store.AddValidatedSession(Identity(9, "nine", ""),
                                         "RBXEventTrackerV2=1", true, &error));
  EXPECT_FALSE(store.AddValidatedSession(Identity(9, "nine", ""),
                                         "_|split line", true, &error));
  EXPECT_FALSE(Exists(accounts_ / "9"));
  EXPECT_FALSE(Exists(accounts_ / "0"));
}

TEST_F(AccountStoreTest, RemovesOnlyTheFilesTheStoreKnows) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  WritePrivate(accounts_ / "42/.roblox.cookie.tmp-1-2", "partial");
  WritePrivate(accounts_ / "42/.account.json.tmp-1-2", "partial");
  WritePrivate(avatars_ / "42.png", kPng);
  WritePrivate(avatars_ / "77.png", kPng);
  ASSERT_EQ(chmod(avatars_.c_str(), 0700), 0);
  WriteSessionArtifacts();

  ASSERT_TRUE(store.RemoveAccount(42, &error)) << error;
  EXPECT_FALSE(Exists(accounts_ / "42"));
  EXPECT_FALSE(Exists(avatars_ / "42.png"));
  EXPECT_TRUE(Exists(avatars_ / "77.png"));
  EXPECT_TRUE(ArtifactsCleared());
  // The active account was removed: the next one takes its place.
  EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
  EXPECT_EQ(ReadJson(accounts_ / "store.json")["order"], Json::array({77}));

  ASSERT_TRUE(store.RemoveAccount(77, &error)) << error;
  EXPECT_EQ(ReadFile(accounts_ / "active"), "guest\n");
  EXPECT_EQ(Resolve().kind, ActiveAccountKind::kGuest);
  // Removing again is harmless.
  EXPECT_TRUE(store.RemoveAccount(77, &error)) << error;
}

TEST_F(AccountStoreTest, RemovalStopsAtUnknownEntries) {
  SeedTwoAccounts();
  AccountStore store = Store();
  std::string error;
  WritePrivate(accounts_ / "77/notes.txt", "mine");
  WriteSessionArtifacts();
  EXPECT_FALSE(store.RemoveAccount(77, &error));
  EXPECT_NE(error.find("notes.txt"), std::string::npos);
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));
  EXPECT_TRUE(Exists(accounts_ / "77/account.json"));
  EXPECT_TRUE(Exists(webview_ / "cookies.sqlite"));

  std::filesystem::remove(accounts_ / "77/notes.txt");
  std::filesystem::create_directory(accounts_ / "77/roblox.cookie.d");
  EXPECT_FALSE(store.RemoveAccount(77, &error));
  std::filesystem::remove(accounts_ / "77/roblox.cookie.d");
  std::filesystem::create_directory(accounts_ / "77/.roblox.cookie.tmp-9");
  EXPECT_FALSE(store.RemoveAccount(77, &error));
  EXPECT_TRUE(Exists(accounts_ / "77/roblox.cookie"));

  // Removing a non-active account keeps the selection.
  std::filesystem::remove(accounts_ / "77/.roblox.cookie.tmp-9");
  ASSERT_TRUE(store.RemoveAccount(77, &error)) << error;
  EXPECT_EQ(ReadFile(accounts_ / "active"), "42\n");
}

TEST_F(AccountStoreTest, LoadListsAccountsInOrderAndIgnoresStrangers) {
  SeedTwoAccounts();
  WriteLegacy(Canonical(kSessionA));
  std::filesystem::create_directory(accounts_ / "0");
  std::filesystem::create_directory(accounts_ / "007");
  std::filesystem::create_directory(accounts_ / "names");
  std::filesystem::create_directory(accounts_ / "9");
  ASSERT_EQ(chmod((accounts_ / "9").c_str(), 0755), 0);
  std::filesystem::create_directory_symlink(accounts_ / "42", accounts_ / "13");
  WritePrivate(avatars_ / "77.png", kPng);
  WritePrivate(accounts_ / "77/account.json", "{not json");

  AccountStoreSnapshot snapshot;
  std::string error;
  ASSERT_TRUE(Store(false).Load(&snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.initialized);
  ASSERT_TRUE(snapshot.active.has_value());
  EXPECT_EQ(*snapshot.active, Account(42));
  EXPECT_TRUE(snapshot.legacy_session_present);
  ASSERT_EQ(snapshot.accounts.size(), 2u);
  EXPECT_EQ(snapshot.accounts[0].user_id, 42);
  EXPECT_EQ(snapshot.accounts[0].username, "alpha");
  EXPECT_TRUE(snapshot.accounts[0].has_session);
  EXPECT_TRUE(snapshot.accounts[0].avatar_file.empty());
  EXPECT_EQ(snapshot.accounts[1].user_id, 77);
  // A damaged record still lists the account, without names.
  EXPECT_TRUE(snapshot.accounts[1].username.empty());
  EXPECT_EQ(snapshot.accounts[1].state, SavedAccountState::kUnverified);
  EXPECT_EQ(snapshot.accounts[1].avatar_file, avatars_ / "77.png");
}

TEST_F(AccountStoreTest, LoadRefusesASharedStore) {
  SeedTwoAccounts();
  ASSERT_EQ(chmod(accounts_.c_str(), 0750), 0);
  AccountStoreSnapshot snapshot;
  std::string error;
  EXPECT_FALSE(Store().Load(&snapshot, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(Store().SelectForLaunch(Account(42), &error));
}

TEST_F(AccountStoreTest, AnEmptyStoreLoads) {
  AccountStoreSnapshot snapshot;
  std::string error;
  ASSERT_TRUE(Store(false).Load(&snapshot, &error)) << error;
  EXPECT_FALSE(snapshot.initialized);
  EXPECT_TRUE(snapshot.accounts.empty());
  EXPECT_FALSE(snapshot.legacy_session_present);
}

TEST_F(AccountStoreTest, RefreshesPublicProfilesAtMostDaily) {
  SeedTwoAccounts();
  roblox_.Route("https://users.roblox.com/v1/users/42",
                {true,
                 200,
                 R"({"id":42,"name":"alpha2","displayName":"Alpha Two"})",
                 {}});
  roblox_.Route(
      "https://users.roblox.com/v1/users/77",
      {true, 200, R"({"id":77,"name":"bravo","displayName":"B"})", {}});
  roblox_.Route(
      "https://thumbnails.roblox.com/v1/users/avatar-headshot?userIds=42,77"
      "&size=150x150&format=Png&isCircular=false",
      {true,
       200,
       R"({"data":[{"targetId":42,"state":"Completed","imageUrl":"https://tr.rbxcdn.com/a/150/150/AvatarHeadshot/Png"},)"
       R"({"targetId":77,"state":"Completed","imageUrl":"https://evil.example/b.png"}]})",
       {}});
  roblox_.Route("https://tr.rbxcdn.com/a/150/150/AvatarHeadshot/Png",
                {true, 200, std::string(kPng, sizeof(kPng) - 1), {}});
  roblox_.ClearRequests();

  AccountStore store = Store();
  ProfileRefreshResult result = store.RefreshProfiles(roblox_, false);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.names_updated, 2);
  EXPECT_EQ(result.avatars_updated, 1);
  EXPECT_EQ(result.failed, 1);
  EXPECT_EQ(ReadFile(avatars_ / "42.png"), std::string(kPng, sizeof(kPng) - 1));
  EXPECT_EQ(ModeOf(avatars_ / "42.png"), 0600u);
  EXPECT_EQ(ModeOf(avatars_), 0700u);
  EXPECT_FALSE(Exists(avatars_ / "77.png"));
  const Json alpha = ReadJson(accounts_ / "42/account.json");
  EXPECT_EQ(alpha["username"], "alpha2");
  EXPECT_EQ(alpha["display_name"], "Alpha Two");
  EXPECT_EQ(alpha["profile_refreshed_at"], now_);
  EXPECT_EQ(ReadJson(accounts_ / "77/account.json")["profile_refreshed_at"], 0);
  EXPECT_TRUE(NoRequestCarriedASession());
  for (const services::HttpRequest& request : roblox_.requests()) {
    EXPECT_NE(request.url.find("https://"), std::string::npos);
    EXPECT_FALSE(request.follow_redirects);
    EXPECT_EQ(request.url.find("evil.example"), std::string::npos);
  }

  // A fresh profile is not fetched again the same day; one without an
  // avatar is.
  roblox_.ClearRequests();
  now_ += 60;
  result = store.RefreshProfiles(roblox_, false);
  ASSERT_TRUE(result) << result.error;
  for (const services::HttpRequest& request : roblox_.requests()) {
    EXPECT_EQ(request.url.find("/users/42"), std::string::npos);
  }
  now_ += 24 * 60 * 60;
  roblox_.ClearRequests();
  result = store.RefreshProfiles(roblox_, false);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.names_updated, 2);
}

TEST_F(AccountStoreTest, ProfilesSurviveAnOfflineRefresh) {
  SeedTwoAccounts();
  roblox_.SetOffline(true);
  const ProfileRefreshResult result = Store().RefreshProfiles(roblox_, true);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.names_updated, 0);
  EXPECT_EQ(result.failed, 2);
  EXPECT_EQ(ReadJson(accounts_ / "42/account.json")["username"], "alpha");
}

TEST_F(AccountStoreTest, BrowserSignInFilesTheAccountRobloxReports) {
  // The launcher's add-account flow: the session's persist callback runs on
  // its worker and files the session under the resolved account.
  AccountStore store = Store();
  std::string error;
  ASSERT_TRUE(store.PrepareBrowserSignIn(&error)) << error;

  struct Window final : BrowserSignInWindow {
    std::shared_ptr<std::vector<WebViewHelperEvent>> pending =
        std::make_shared<std::vector<WebViewHelperEvent>>();
    bool exited() const override { return false; }
    bool WaitUntilReady(std::chrono::milliseconds) override { return true; }
    bool DrainEvents(std::vector<WebViewHelperEvent>* events) override {
      for (WebViewHelperEvent& event : *pending) {
        events->push_back(std::move(event));
      }
      pending->clear();
      return true;
    }
    bool SetRobloxCookie(std::string_view) override { return true; }
    bool ClearRobloxCookie() override { return true; }
    bool SetTitle(std::string_view) override { return true; }
    bool SetVisible(bool) override { return true; }
    bool RequestClose() override { return true; }
  };
  auto window = std::make_unique<Window>();
  WebViewHelperEvent cookie;
  cookie.type = WebViewHelperEventType::kRobloxCookie;
  cookie.payload = kSessionB;
  window->pending->push_back(cookie);

  BrowserSignInSession session(
      auth_service_,
      [&store](const services::AuthIdentity& identity, std::string_view value) {
        std::string persist_error;
        return store.AddValidatedSession(identity, value, true, &persist_error);
      });
  ASSERT_TRUE(session.Attach(std::move(window), "Add account", true));
  std::optional<BrowserSignInEvent> accepted;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!accepted.has_value() && std::chrono::steady_clock::now() < deadline) {
    for (BrowserSignInEvent& event : session.Poll()) {
      if (event.type == BrowserSignInEventType::kAccepted) {
        accepted = std::move(event);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(accepted->identity.user_id, 77);
  EXPECT_EQ(ReadFile(accounts_ / "77/roblox.cookie"), Canonical(kSessionB));
  EXPECT_EQ(ReadFile(accounts_ / "active"), "77\n");
}

TEST(AccountStoreOptionsTest, FollowsTheRuntimeAndWebKitLocations) {
  const MapEnvironment environment({{"HOME", "/home/player"},
                                    {"XDG_DATA_HOME", "/xdg/data"},
                                    {"MOCKTAIL_DATA_ROOT", "/custom/data"},
                                    {"XDG_CACHE_HOME", "/xdg/cache"}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment, "/");
  AccountStoreOptions options =
      MakeAccountStoreOptions(paths, environment, true);
  EXPECT_EQ(options.auth_root, "/custom/data/auth");
  EXPECT_EQ(options.app_storage_file,
            "/custom/data/android/data/files/appData/LocalStorage/"
            "appStorage.json");
  EXPECT_EQ(options.webview_data_directory, "/xdg/data/mocktail/webview");
  EXPECT_EQ(options.avatar_directory, "/xdg/cache/mocktail/avatars");
  EXPECT_TRUE(options.assume_exclusive);
  EXPECT_EQ(AccountStore(options).accounts_directory(),
            "/custom/data/auth/accounts");

  const MapEnvironment relative({{"HOME", "/home/player"},
                                 {"XDG_DATA_HOME", "relative"},
                                 {"MOCKTAIL_RUNTIME_ROOT", "/runtime"}});
  options = MakeAccountStoreOptions(
      RuntimePaths::FromEnvironment(relative, "/"), relative, false);
  EXPECT_EQ(options.webview_data_directory,
            "/home/player/.local/share/mocktail/webview");
  EXPECT_EQ(options.app_storage_file,
            "/runtime/data/files/appData/LocalStorage/appStorage.json");
  EXPECT_FALSE(options.assume_exclusive);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
