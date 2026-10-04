#include "launcher_ui/accounts_model.h"

#include <algorithm>

#include "runtime/runtime_config_file.h"

namespace mocktail::launcher_ui {
namespace {

constexpr std::int64_t kMinute = 60;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
// Older plays read better as a date.
constexpr std::int64_t kRelativeDays = 30;

runtime::ActiveAccountPointer AccountPointer(std::int64_t user_id) {
  runtime::ActiveAccountPointer pointer;
  pointer.guest = false;
  pointer.user_id = user_id;
  return pointer;
}

bool Contains(const std::vector<std::int64_t>& values, std::int64_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

AccountStatus StatusOf(const runtime::SavedAccount& account) {
  if (!account.has_session ||
      account.state == runtime::SavedAccountState::kSignedOut) {
    return AccountStatus::kSignedOut;
  }
  if (account.state == runtime::SavedAccountState::kUnverified) {
    return AccountStatus::kUnverified;
  }
  return AccountStatus::kReady;
}

std::string ShownName(const runtime::SavedAccount& account) {
  if (!account.display_name.empty()) return account.display_name;
  if (!account.username.empty()) return account.username;
  return std::to_string(account.user_id);
}

// ---- AccountsModel
// --------------------------------------------------------------

void AccountsModel::SetSnapshot(runtime::AccountStoreSnapshot snapshot) {
  snapshot_ = std::move(snapshot);
  removals_.erase(
      std::remove_if(removals_.begin(), removals_.end(),
                     [this](std::int64_t user_id) { return !Saved(user_id); }),
      removals_.end());
  if (choice_.has_value() &&
      (!Listed(*choice_) || SavedSelection() == choice_)) {
    choice_.reset();
  }
}

std::vector<const runtime::SavedAccount*> AccountsModel::VisibleAccounts()
    const {
  std::vector<const runtime::SavedAccount*> visible;
  for (const runtime::SavedAccount& account : snapshot_.accounts) {
    if (!Contains(removals_, account.user_id)) visible.push_back(&account);
  }
  return visible;
}

const runtime::SavedAccount* AccountsModel::FindVisible(
    std::int64_t user_id) const {
  if (Contains(removals_, user_id)) return nullptr;
  for (const runtime::SavedAccount& account : snapshot_.accounts) {
    if (account.user_id == user_id) return &account;
  }
  return nullptr;
}

bool AccountsModel::Saved(std::int64_t user_id) const {
  return std::any_of(snapshot_.accounts.begin(), snapshot_.accounts.end(),
                     [user_id](const runtime::SavedAccount& account) {
                       return account.user_id == user_id;
                     });
}

bool AccountsModel::Listed(const runtime::ActiveAccountPointer& pointer) const {
  return pointer.guest || FindVisible(pointer.user_id) != nullptr;
}

std::optional<runtime::ActiveAccountPointer> AccountsModel::SavedSelection()
    const {
  if (!snapshot_.initialized || !snapshot_.active.has_value()) {
    return std::nullopt;
  }
  if (!snapshot_.active->guest && !Saved(snapshot_.active->user_id)) {
    return std::nullopt;
  }
  return snapshot_.active;
}

std::optional<runtime::ActiveAccountPointer> AccountsModel::Selection() const {
  return choice_.has_value() ? choice_ : SavedSelection();
}

bool AccountsModel::IsSelected(
    const runtime::ActiveAccountPointer& pointer) const {
  const std::optional<runtime::ActiveAccountPointer> selection = Selection();
  return selection.has_value() && *selection == pointer;
}

void AccountsModel::Choose(const runtime::ActiveAccountPointer& pointer) {
  if (!Listed(pointer)) return;
  const std::optional<runtime::ActiveAccountPointer> saved = SavedSelection();
  if (saved.has_value() && *saved == pointer) {
    choice_.reset();
  } else {
    choice_ = pointer;
  }
}

bool AccountsModel::SelectionChanged() const {
  if (!choice_.has_value()) return false;
  const std::optional<runtime::ActiveAccountPointer> saved = SavedSelection();
  return !saved.has_value() || *saved != *choice_;
}

std::optional<AccountsModel::StagedRemoval> AccountsModel::StageRemoval(
    std::int64_t user_id) {
  if (FindVisible(user_id) == nullptr) return std::nullopt;
  StagedRemoval removal;
  removal.user_id = user_id;
  removal.previous_choice = choice_;
  const bool was_selected = IsSelected(AccountPointer(user_id));
  removals_.push_back(user_id);
  if (was_selected) {
    // AccountStore::RemoveAccount moves an active pointer the same way.
    runtime::ActiveAccountPointer replacement;
    const std::vector<const runtime::SavedAccount*> remaining =
        VisibleAccounts();
    if (!remaining.empty()) replacement = AccountPointer(remaining[0]->user_id);
    Choose(replacement);
  }
  return removal;
}

void AccountsModel::UndoRemoval(const StagedRemoval& removal) {
  const auto found =
      std::find(removals_.begin(), removals_.end(), removal.user_id);
  if (found == removals_.end()) return;
  removals_.erase(found);
  choice_ = removal.previous_choice;
  if (choice_.has_value() && !Listed(*choice_)) choice_.reset();
}

void AccountsModel::FinishRemoval(std::int64_t user_id) {
  removals_.erase(std::remove(removals_.begin(), removals_.end(), user_id),
                  removals_.end());
  snapshot_.accounts.erase(
      std::remove_if(snapshot_.accounts.begin(), snapshot_.accounts.end(),
                     [user_id](const runtime::SavedAccount& account) {
                       return account.user_id == user_id;
                     }),
      snapshot_.accounts.end());
}

int AccountsModel::UnsavedCount() const {
  return static_cast<int>(removals_.size()) + (SelectionChanged() ? 1 : 0);
}

void AccountsModel::Discard() {
  choice_.reset();
  removals_.clear();
}

// ---- chip, names, time
// ----------------------------------------------------------

AccountChipState DescribeAccountChip(const AccountsModel& model,
                                     bool environment_override,
                                     bool load_failed) {
  AccountChipState state;
  if (environment_override) {
    state.kind = AccountChipKind::kEnvironment;
    return state;
  }
  if (load_failed) {
    state.kind = AccountChipKind::kUnavailable;
    return state;
  }
  const std::optional<runtime::ActiveAccountPointer> selection =
      model.Selection();
  if (selection.has_value() && !selection->guest) {
    state.account = model.FindVisible(selection->user_id);
    state.kind = state.account != nullptr ? AccountChipKind::kAccount
                                          : AccountChipKind::kNotSignedIn;
    return state;
  }
  if (selection.has_value()) {
    state.kind = model.VisibleAccounts().empty() ? AccountChipKind::kNotSignedIn
                                                 : AccountChipKind::kGuest;
    return state;
  }
  // Until the store exists the runtime uses the legacy auth root, which may
  // hold a session Roblox could not be asked about yet.
  state.kind =
      !model.snapshot().initialized && model.snapshot().legacy_session_present
          ? AccountChipKind::kLegacySession
          : AccountChipKind::kNotSignedIn;
  return state;
}

std::vector<std::string> AccountOverrideVariables(
    const runtime::Environment& environment) {
  // The same names as runtime::AccountStoreOverriddenByEnvironment.
  std::vector<std::string> names;
  for (const char* name : {"MOCKTAIL_AUTH_ROOT", "MOCKTAIL_COOKIE_FILE",
                           "MOCKTAIL_ROBLOX_COOKIES"}) {
    if (environment.HasNonEmpty(name)) names.emplace_back(name);
  }
  return names;
}

LastPlayed DescribeLastPlayed(std::int64_t played_at, std::int64_t now) {
  LastPlayed result;
  if (played_at <= 0) return result;
  const std::int64_t elapsed = now - played_at;
  if (elapsed < 0) {
    // A clock that went back: the date is still true.
    result.kind = LastPlayed::Kind::kDate;
  } else if (elapsed < kMinute) {
    result.kind = LastPlayed::Kind::kJustNow;
  } else if (elapsed < kHour) {
    result.kind = LastPlayed::Kind::kMinutes;
    result.count = elapsed / kMinute;
  } else if (elapsed < kDay) {
    result.kind = LastPlayed::Kind::kHours;
    result.count = elapsed / kHour;
  } else if (elapsed < kRelativeDays * kDay) {
    result.kind = LastPlayed::Kind::kDays;
    result.count = elapsed / kDay;
  } else {
    result.kind = LastPlayed::Kind::kDate;
  }
  return result;
}

// ---- network
// --------------------------------------------------------------------

LauncherNetworkPlan PlanLauncherNetwork(
    const runtime::Environment& environment,
    const std::filesystem::path& config_file) {
  LauncherNetworkPlan plan;
  // The same layering as the game: variables already set win over
  // config.yaml (runtime_config_file.h LoadRuntimeConfig).
  const runtime::RuntimeConfigLoadResult loaded =
      runtime::LoadRuntimeConfig(environment, config_file);
  if (!loaded) {
    plan.error = loaded.error;
    return plan;
  }
  const runtime::RuntimeConfig& config = loaded.config;
  const auto set = [&](const char* name, std::string value) {
    if (!environment.HasNonEmpty(name)) {
      plan.assignments.emplace_back(name, std::move(value));
    }
  };
  // CurlHttpClient reads MOCKTAIL_CA_BUNDLE and MOCKTAIL_HTTP_PROXY_* at
  // every request (src/services/http_client.cc).
  if (config.ca_bundle().has_value()) {
    set("MOCKTAIL_CA_BUNDLE", config.ca_bundle()->string());
  }
  // With Fleasion the proxy is Fleasion's local one, which the user starts
  // before Roblox (main.cc prints "start Fleasion before Roblox"), so it may
  // not run yet; and Fleasion's own CA bundle is made only then.
  if (config.fleasion_enabled() ||
      environment.HasNonEmpty("MOCKTAIL_HTTP_PROXY_HOST")) {
    return plan;
  }
  if (config.network_proxy().has_value()) {
    const runtime::NetworkProxyConfig& proxy = *config.network_proxy();
    set("MOCKTAIL_HTTP_PROXY_HOST", proxy.host);
    set("MOCKTAIL_HTTP_PROXY_PORT", std::to_string(proxy.port));
    set("MOCKTAIL_HTTP_PROXY_SCHEME", proxy.scheme);
  } else if (config.use_system_proxy()) {
    plan.system_proxy = true;
  }
  return plan;
}

services::HttpRequest CancellableHttpClient::Limit(
    const services::HttpRequest& request) const {
  services::HttpRequest limited = request;
  if (limited.timeout_ms <= 0 || limited.timeout_ms > maximum_timeout_ms_) {
    limited.timeout_ms = maximum_timeout_ms_;
  }
  return limited;
}

services::HttpResponse CancellableHttpClient::Get(
    const services::HttpRequest& request) {
  if (cancelled()) {
    return services::HttpResponse(false, 0, {}, "cancelled");
  }
  return client_.Get(Limit(request));
}

services::HttpResponse CancellableHttpClient::Post(
    const services::HttpRequest& request, const std::string& body) {
  if (cancelled()) {
    return services::HttpResponse(false, 0, {}, "cancelled");
  }
  return client_.Post(Limit(request), body);
}

}  // namespace mocktail::launcher_ui
