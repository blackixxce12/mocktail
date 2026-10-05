// The GTK-free part of the settings window's Accounts page: the staged
// account selection and removals, the account chip, "last played", the
// override variables, and the network setup for the window's own requests.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "launcher_ui/accounts_model.h"
#include "runtime/runtime_config_bootstrap.h"

namespace mocktail::launcher_ui {
namespace {

using runtime::AccountStoreSnapshot;
using runtime::ActiveAccountPointer;
using runtime::SavedAccount;
using runtime::SavedAccountState;

class MapEnvironment final : public runtime::Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_launcher_ui_accounts_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) path_ = created;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

  std::filesystem::path Write(const std::filesystem::path& relative,
                              std::string_view contents) const {
    const std::filesystem::path file = path_ / relative;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << contents;
    return file;
  }

 private:
  std::filesystem::path path_;
};

ActiveAccountPointer Guest() { return ActiveAccountPointer{}; }

ActiveAccountPointer Account(std::int64_t user_id) {
  ActiveAccountPointer pointer;
  pointer.guest = false;
  pointer.user_id = user_id;
  return pointer;
}

SavedAccount Saved(std::int64_t user_id, std::string username,
                   std::string display_name) {
  SavedAccount account;
  account.user_id = user_id;
  account.username = std::move(username);
  account.display_name = std::move(display_name);
  account.state = SavedAccountState::kSignedIn;
  account.has_session = true;
  return account;
}

// Two saved accounts, in store order 20, 10; `active` selected.
AccountStoreSnapshot TwoAccounts(std::optional<ActiveAccountPointer> active) {
  AccountStoreSnapshot snapshot;
  snapshot.initialized = true;
  snapshot.active = active;
  snapshot.accounts.push_back(Saved(20, "second_user", "Second"));
  snapshot.accounts.push_back(Saved(10, "first_user", "First"));
  return snapshot;
}

std::string ReplaceOnce(std::string text, std::string_view from,
                        std::string_view to) {
  const std::size_t found = text.find(from);
  EXPECT_NE(found, std::string::npos) << from;
  if (found != std::string::npos) text.replace(found, from.size(), to);
  return text;
}

// ---- selection
// ------------------------------------------------------------------

TEST(LauncherUiAccountsModelTest, NoStoreMeansNoSelection) {
  AccountsModel model;
  AccountStoreSnapshot snapshot;
  snapshot.legacy_session_present = true;
  model.SetSnapshot(snapshot);
  EXPECT_FALSE(model.SavedSelection().has_value());
  EXPECT_FALSE(model.Selection().has_value());
  EXPECT_FALSE(model.SelectionChanged());
  EXPECT_EQ(model.UnsavedCount(), 0);

  // Choosing guest is a change: it creates the store's pointer on Save.
  model.Choose(Guest());
  EXPECT_TRUE(model.SelectionChanged());
  EXPECT_EQ(model.UnsavedCount(), 1);
}

TEST(LauncherUiAccountsModelTest, PointerToMissingAccountIsUnusable) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(99)));
  EXPECT_FALSE(model.SavedSelection().has_value());
  EXPECT_FALSE(model.Selection().has_value());
  EXPECT_FALSE(model.IsSelected(Account(99)));
}

TEST(LauncherUiAccountsModelTest, ChoosingStagesUntilTheStoreHoldsIt) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_FALSE(model.SelectionChanged());

  model.Choose(Account(20));
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_FALSE(model.IsSelected(Account(10)));
  EXPECT_TRUE(model.SelectionChanged());
  EXPECT_EQ(model.UnsavedCount(), 1);
  ASSERT_TRUE(model.SavedSelection().has_value());
  EXPECT_EQ(*model.SavedSelection(), Account(10));

  // Back to what the store holds: nothing to save.
  model.Choose(Account(10));
  EXPECT_FALSE(model.SelectionChanged());
  EXPECT_EQ(model.UnsavedCount(), 0);

  model.Choose(Guest());
  EXPECT_TRUE(model.IsSelected(Guest()));
  model.Discard();
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_EQ(model.UnsavedCount(), 0);
}

TEST(LauncherUiAccountsModelTest, UnlistedAccountsCannotBeChosen) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  model.Choose(Account(55));
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_FALSE(model.SelectionChanged());
}

TEST(LauncherUiAccountsModelTest, SnapshotKeepsAStillValidChoice) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  model.Choose(Account(20));
  // A profile refresh: same store, new names.
  AccountStoreSnapshot refreshed = TwoAccounts(Account(10));
  refreshed.accounts[0].display_name = "Renamed";
  model.SetSnapshot(refreshed);
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_EQ(model.FindVisible(20)->display_name, "Renamed");

  // The account went away (removed elsewhere): the choice goes too.
  AccountStoreSnapshot without = TwoAccounts(Account(10));
  without.accounts.erase(without.accounts.begin());
  model.SetSnapshot(without);
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_FALSE(model.SelectionChanged());
}

TEST(LauncherUiAccountsModelTest, SnapshotThatNowHoldsTheChoiceIsSaved) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  model.Choose(Account(20));
  // Save wrote it.
  model.SetSnapshot(TwoAccounts(Account(20)));
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_FALSE(model.SelectionChanged());
  // The next start's reconcile moves the pointer: no staged choice is left
  // to overrule it.
  model.SetSnapshot(TwoAccounts(Account(10)));
  EXPECT_TRUE(model.IsSelected(Account(10)));
}

// ---- removal
// --------------------------------------------------------------------

TEST(LauncherUiAccountsModelTest, RemovingTheSelectedAccountMovesTheSelection) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(20)));
  const std::optional<AccountsModel::StagedRemoval> removal =
      model.StageRemoval(20);
  ASSERT_TRUE(removal.has_value());
  // Hidden, and the selection goes where RemoveAccount moves the pointer:
  // the first remaining account.
  EXPECT_EQ(model.FindVisible(20), nullptr);
  ASSERT_EQ(model.VisibleAccounts().size(), 1U);
  EXPECT_EQ(model.VisibleAccounts()[0]->user_id, 10);
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_EQ(model.UnsavedCount(), 2);  // the removal and the selection

  model.UndoRemoval(*removal);
  EXPECT_NE(model.FindVisible(20), nullptr);
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_EQ(model.UnsavedCount(), 0);
}

TEST(LauncherUiAccountsModelTest, RemovingTheLastAccountSelectsGuest) {
  AccountsModel model;
  AccountStoreSnapshot snapshot;
  snapshot.initialized = true;
  snapshot.active = Account(7);
  snapshot.accounts.push_back(Saved(7, "only", "Only"));
  model.SetSnapshot(snapshot);
  ASSERT_TRUE(model.StageRemoval(7).has_value());
  EXPECT_TRUE(model.IsSelected(Guest()));
  EXPECT_TRUE(model.VisibleAccounts().empty());
  // The store does the same, so after the removal nothing is left to save.
  model.FinishRemoval(7);
  snapshot.accounts.clear();
  snapshot.active = Guest();
  model.SetSnapshot(snapshot);
  EXPECT_EQ(model.UnsavedCount(), 0);
  EXPECT_TRUE(model.staged_removals().empty());
}

TEST(LauncherUiAccountsModelTest, RemovingAnotherAccountKeepsTheSelection) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(20)));
  ASSERT_TRUE(model.StageRemoval(10).has_value());
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_EQ(model.UnsavedCount(), 1);
  EXPECT_FALSE(model.StageRemoval(10).has_value()) << "already staged";
  EXPECT_FALSE(model.StageRemoval(404).has_value());
  model.Discard();
  EXPECT_EQ(model.VisibleAccounts().size(), 2U);
  EXPECT_EQ(model.UnsavedCount(), 0);
}

TEST(LauncherUiAccountsModelTest, UndoRestoresAnEarlierChoice) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  model.Choose(Account(20));
  const auto removal = model.StageRemoval(20);
  ASSERT_TRUE(removal.has_value());
  // 10 is what the store holds: no selection change is left.
  EXPECT_TRUE(model.IsSelected(Account(10)));
  EXPECT_EQ(model.UnsavedCount(), 1);
  model.UndoRemoval(*removal);
  EXPECT_TRUE(model.IsSelected(Account(20)));
  EXPECT_EQ(model.UnsavedCount(), 1);
}

TEST(LauncherUiAccountsModelTest, SnapshotDropsRemovalsOfVanishedAccounts) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  ASSERT_TRUE(model.StageRemoval(20).has_value());
  AccountStoreSnapshot without = TwoAccounts(Account(10));
  without.accounts.erase(without.accounts.begin());
  model.SetSnapshot(without);
  EXPECT_TRUE(model.staged_removals().empty());
  EXPECT_EQ(model.UnsavedCount(), 0);
}

// ---- chip and names
// --------------------------------------------------------------

TEST(LauncherUiAccountsModelTest, ChipFollowsTheSelection) {
  AccountsModel model;
  model.SetSnapshot(TwoAccounts(Account(10)));
  AccountChipState chip = DescribeAccountChip(model, false, false);
  EXPECT_EQ(chip.kind, AccountChipKind::kAccount);
  ASSERT_NE(chip.account, nullptr);
  EXPECT_EQ(chip.account->username, "first_user");

  model.Choose(Guest());
  EXPECT_EQ(DescribeAccountChip(model, false, false).kind,
            AccountChipKind::kGuest);
  EXPECT_EQ(DescribeAccountChip(model, true, false).kind,
            AccountChipKind::kEnvironment);
  EXPECT_EQ(DescribeAccountChip(model, false, true).kind,
            AccountChipKind::kUnavailable);

  AccountStoreSnapshot empty;
  empty.initialized = true;
  empty.active = Guest();
  model.SetSnapshot(empty);
  EXPECT_EQ(DescribeAccountChip(model, false, false).kind,
            AccountChipKind::kNotSignedIn);

  AccountStoreSnapshot legacy;
  legacy.legacy_session_present = true;
  model.SetSnapshot(legacy);
  EXPECT_EQ(DescribeAccountChip(model, false, false).kind,
            AccountChipKind::kLegacySession);

  model.SetSnapshot(TwoAccounts(Account(404)));
  EXPECT_EQ(DescribeAccountChip(model, false, false).kind,
            AccountChipKind::kNotSignedIn);
}

TEST(LauncherUiAccountsModelTest, StatusAndShownName) {
  SavedAccount account = Saved(5, "user_five", "Five");
  EXPECT_EQ(StatusOf(account), AccountStatus::kReady);
  EXPECT_EQ(ShownName(account), "Five");
  account.state = SavedAccountState::kUnverified;
  EXPECT_EQ(StatusOf(account), AccountStatus::kUnverified);
  // A retired session is signed out whatever account.json says.
  account.has_session = false;
  EXPECT_EQ(StatusOf(account), AccountStatus::kSignedOut);
  account.display_name.clear();
  EXPECT_EQ(ShownName(account), "user_five");
  account.username.clear();
  EXPECT_EQ(ShownName(account), "5");
}

TEST(LauncherUiAccountsModelTest, OverrideVariablesAreNamedNotShown) {
  const MapEnvironment environment(
      {{"MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=secret"},
       {"MOCKTAIL_COOKIE_FILE", ""},
       {"MOCKTAIL_AUTH_ROOT", "/tmp/auth"}});
  const std::vector<std::string> names = AccountOverrideVariables(environment);
  EXPECT_EQ(names, (std::vector<std::string>{"MOCKTAIL_AUTH_ROOT",
                                             "MOCKTAIL_ROBLOX_COOKIES"}));
  EXPECT_TRUE(AccountOverrideVariables(MapEnvironment()).empty());
}

TEST(LauncherUiAccountsModelTest, LastPlayedUsesTheLargestWholeUnit) {
  constexpr std::int64_t kNow = 1'800'000'000;
  EXPECT_EQ(DescribeLastPlayed(0, kNow).kind, LastPlayed::Kind::kNever);
  EXPECT_EQ(DescribeLastPlayed(kNow - 30, kNow).kind,
            LastPlayed::Kind::kJustNow);
  LastPlayed played = DescribeLastPlayed(kNow - 5 * 60 - 10, kNow);
  EXPECT_EQ(played.kind, LastPlayed::Kind::kMinutes);
  EXPECT_EQ(played.count, 5);
  played = DescribeLastPlayed(kNow - 3 * 3600, kNow);
  EXPECT_EQ(played.kind, LastPlayed::Kind::kHours);
  EXPECT_EQ(played.count, 3);
  played = DescribeLastPlayed(kNow - 36 * 3600, kNow);
  EXPECT_EQ(played.kind, LastPlayed::Kind::kDays);
  EXPECT_EQ(played.count, 1);
  played = DescribeLastPlayed(kNow - 29 * 86400, kNow);
  EXPECT_EQ(played.kind, LastPlayed::Kind::kDays);
  EXPECT_EQ(played.count, 29);
  EXPECT_EQ(DescribeLastPlayed(kNow - 31 * 86400, kNow).kind,
            LastPlayed::Kind::kDate);
  EXPECT_EQ(DescribeLastPlayed(kNow + 600, kNow).kind, LastPlayed::Kind::kDate);
}

// ---- network
// --------------------------------------------------------------------

TEST(LauncherUiAccountsModelTest, NetworkUsesTheConfiguredProxyAndBundle) {
  const TemporaryDirectory directory;
  const std::filesystem::path bundle =
      directory.Write("cacert.pem", "-----BEGIN CERTIFICATE-----\n");
  std::string yaml(runtime::DefaultRuntimeConfigYaml());
  yaml = ReplaceOnce(yaml, "  # proxy_host: 127.0.0.1\n",
                     "  proxy_host: 10.0.0.2\n");
  yaml = ReplaceOnce(yaml, "  # proxy_port: 8080\n", "  proxy_port: 3128\n");
  yaml = ReplaceOnce(yaml,
                     "  # ca_bundle: /home/user/.config/mocktail/cacert.pem\n",
                     "  ca_bundle: " + bundle.string() + "\n");
  const std::filesystem::path config = directory.Write("config.yaml", yaml);

  LauncherNetworkPlan plan = PlanLauncherNetwork(MapEnvironment(), config);
  ASSERT_TRUE(plan.error.empty()) << plan.error;
  EXPECT_FALSE(plan.system_proxy);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"MOCKTAIL_CA_BUNDLE", bundle.string()},
      {"MOCKTAIL_HTTP_PROXY_HOST", "10.0.0.2"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "3128"},
      {"MOCKTAIL_HTTP_PROXY_SCHEME", "http"}};
  EXPECT_EQ(plan.assignments, expected);

  // The environment wins, as it does for the game.
  plan = PlanLauncherNetwork(
      MapEnvironment({{"MOCKTAIL_HTTP_PROXY_HOST", "192.168.1.1"},
                      {"MOCKTAIL_HTTP_PROXY_PORT", "8000"},
                      {"MOCKTAIL_CA_BUNDLE", bundle.string()}}),
      config);
  ASSERT_TRUE(plan.error.empty()) << plan.error;
  EXPECT_TRUE(plan.assignments.empty());
}

TEST(LauncherUiAccountsModelTest, NetworkDirectByDefaultOrSystemProxy) {
  const TemporaryDirectory directory;
  std::string yaml(runtime::DefaultRuntimeConfigYaml());
  const std::filesystem::path plain = directory.Write("plain.yaml", yaml);
  LauncherNetworkPlan plan = PlanLauncherNetwork(MapEnvironment(), plain);
  ASSERT_TRUE(plan.error.empty()) << plan.error;
  EXPECT_TRUE(plan.assignments.empty());
  EXPECT_FALSE(plan.system_proxy);

  yaml = ReplaceOnce(yaml, "  use_system_proxy: false\n",
                     "  use_system_proxy: true\n");
  const std::filesystem::path system = directory.Write("system.yaml", yaml);
  plan = PlanLauncherNetwork(MapEnvironment(), system);
  ASSERT_TRUE(plan.error.empty()) << plan.error;
  EXPECT_TRUE(plan.system_proxy);
  EXPECT_TRUE(plan.assignments.empty());
}

TEST(LauncherUiAccountsModelTest, NetworkSkipsFleasionsLocalProxy) {
  const TemporaryDirectory directory;
  std::string yaml(runtime::DefaultRuntimeConfigYaml());
  yaml = ReplaceOnce(yaml,
                     "    # Boolean (default: false): trust Fleasion's CA "
                     "without editing Roblox files.\n    enabled: false\n",
                     "    enabled: true\n");
  yaml = ReplaceOnce(
      yaml,
      "    # ca_certificate: /home/user/.config/Fleasion/proxy_ca/ca.crt\n",
      "    ca_certificate: /home/user/.config/Fleasion/proxy_ca/ca.crt\n");
  const std::filesystem::path config = directory.Write("config.yaml", yaml);
  const LauncherNetworkPlan plan =
      PlanLauncherNetwork(MapEnvironment(), config);
  ASSERT_TRUE(plan.error.empty()) << plan.error;
  EXPECT_TRUE(plan.assignments.empty());
  EXPECT_FALSE(plan.system_proxy);
}

TEST(LauncherUiAccountsModelTest, NetworkReportsABrokenConfig) {
  const TemporaryDirectory directory;
  const std::filesystem::path config =
      directory.Write("config.yaml", "graphics:\n  no_such_key: 1\n");
  const LauncherNetworkPlan plan =
      PlanLauncherNetwork(MapEnvironment(), config);
  EXPECT_FALSE(plan.error.empty());
  EXPECT_TRUE(plan.assignments.empty());
}

// main.cc refuses to start Roblox when the proxy cannot be worked out; the
// window's own requests (the session check, names and avatars, signing
// in) must not go out directly then either.
TEST(LauncherUiAccountsModelTest, NetworkWaitsWhileTheProxyIsUnknown) {
  const TemporaryDirectory directory;
  bool asked = false;
  const auto resolver = [&asked](runtime::SystemProxyResult result) {
    return [&asked, result] {
      asked = true;
      return result;
    };
  };

  // A typo anywhere in config.yaml hides network.proxy_host.
  std::string yaml(runtime::DefaultRuntimeConfigYaml());
  yaml = ReplaceOnce(yaml, "  # proxy_host: 127.0.0.1\n",
                     "  proxy_host: 10.0.0.2\n");
  yaml = ReplaceOnce(yaml, "  # proxy_port: 8080\n", "  proxy_port: 3128\n");
  const std::filesystem::path proxied = directory.Write("proxied.yaml", yaml);
  LauncherNetworkSetup setup = ResolveLauncherNetwork(
      PlanLauncherNetwork(MapEnvironment(), proxied), nullptr);
  EXPECT_EQ(setup.blocked, LauncherNetworkBlock::kNone);
  EXPECT_EQ(setup.assignments.size(), 3U);
  const std::filesystem::path broken = directory.Write(
      "broken.yaml",
      ReplaceOnce(yaml, "  # vsync: off\n", "  vsync: sometimes\n"));
  setup = ResolveLauncherNetwork(PlanLauncherNetwork(MapEnvironment(), broken),
                                 resolver({}));
  EXPECT_EQ(setup.blocked, LauncherNetworkBlock::kConfig);
  EXPECT_FALSE(setup.error.empty());
  EXPECT_TRUE(setup.assignments.empty());
  EXPECT_FALSE(asked);

  // The system proxy cannot be resolved.
  yaml = std::string(runtime::DefaultRuntimeConfigYaml());
  yaml = ReplaceOnce(yaml, "  use_system_proxy: false\n",
                     "  use_system_proxy: true\n");
  const std::filesystem::path system = directory.Write("system.yaml", yaml);
  const LauncherNetworkPlan plan =
      PlanLauncherNetwork(MapEnvironment(), system);
  setup = ResolveLauncherNetwork(
      plan, resolver({std::nullopt, "proxy resolver unavailable"}));
  EXPECT_TRUE(asked);
  EXPECT_EQ(setup.blocked, LauncherNetworkBlock::kSystemProxy);
  EXPECT_EQ(setup.error, "proxy resolver unavailable");
  EXPECT_TRUE(setup.assignments.empty());

  // Resolved: its proxy, or a direct connection.
  runtime::NetworkProxyConfig proxy;
  proxy.scheme = "socks5";
  proxy.host = "10.0.0.3";
  proxy.port = 1080;
  setup = ResolveLauncherNetwork(plan, resolver({proxy, ""}));
  EXPECT_EQ(setup.blocked, LauncherNetworkBlock::kNone);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"MOCKTAIL_HTTP_PROXY_HOST", "10.0.0.3"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "1080"},
      {"MOCKTAIL_HTTP_PROXY_SCHEME", "socks5"}};
  EXPECT_EQ(setup.assignments, expected);
  setup = ResolveLauncherNetwork(plan, resolver({std::nullopt, ""}));
  EXPECT_EQ(setup.blocked, LauncherNetworkBlock::kNone);
  EXPECT_TRUE(setup.assignments.empty());
}

// ---- cancellable client
// ----------------------------------------------------------

class RecordingHttpClient final : public services::HttpClient {
 public:
  services::HttpResponse Get(const services::HttpRequest& request) override {
    timeouts.push_back(request.timeout_ms);
    return services::HttpResponse(true, 200, "{}", {});
  }
  services::HttpResponse Post(const services::HttpRequest& request,
                              const std::string& body) override {
    timeouts.push_back(request.timeout_ms);
    bodies.push_back(body);
    return services::HttpResponse(true, 200, "{}", {});
  }

  std::vector<long> timeouts;
  std::vector<std::string> bodies;
};

TEST(LauncherUiAccountsModelTest, CancellableClientLimitsAndStops) {
  RecordingHttpClient inner;
  CancellableHttpClient client(inner, 8000);
  services::HttpRequest request;
  request.url = "https://users.roblox.com/v1/users/authenticated";
  request.timeout_ms = 15000;
  EXPECT_TRUE(client.Get(request).transport_ok);
  request.timeout_ms = 2000;
  EXPECT_TRUE(client.Post(request, "{}").transport_ok);
  EXPECT_EQ(inner.timeouts, (std::vector<long>{8000, 2000}));

  client.Cancel();
  const services::HttpResponse cancelled = client.Get(request);
  EXPECT_FALSE(cancelled.transport_ok);
  EXPECT_FALSE(client.Post(request, "{}").transport_ok);
  EXPECT_EQ(inner.timeouts.size(), 2U) << "nothing reached the network";
}

}  // namespace
}  // namespace mocktail::launcher_ui
