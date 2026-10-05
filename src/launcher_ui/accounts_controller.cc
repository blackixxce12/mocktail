#include "launcher_ui/accounts_controller.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

#include "launcher_ui/i18n.h"
#include "runtime/environment.h"
#include "runtime/system_proxy.h"
#include "services/auth_service.h"
#include "services/http_client.h"

namespace mocktail::launcher_ui {
namespace {

// Every request of the window's own checks gives up after this long
// (services/auth_service.cc and account_store.cc ask for 15 s), so Save and
// Play wait less for a dead network and a cancelled worker stops soon.
constexpr long kRequestTimeoutMs = 8000;
// Closing the window waits this long for a request still in flight; after
// that the worker is left to finish on its own (every store change is
// atomic, and its next requests fail at once).
constexpr std::chrono::milliseconds kShutdownWait(2500);
// BrowserSignInSession::Poll is meant to run every few tens of ms.
constexpr guint kSignInPollMilliseconds = 30;
// How long the sign-in window may take to open. WebKit's first start can
// take several seconds (the 5 s default gave up in a software-rendered
// sandbox); the page shows a Cancel button meanwhile.
constexpr std::chrono::seconds kSignInReadyTimeout(20);

std::map<LauncherContext*, std::weak_ptr<AccountsController>>& Controllers() {
  static auto* controllers =
      new std::map<LauncherContext*, std::weak_ptr<AccountsController>>();
  return *controllers;
}

runtime::ActiveAccountPointer AccountPointer(std::int64_t user_id) {
  runtime::ActiveAccountPointer pointer;
  pointer.guest = false;
  pointer.user_id = user_id;
  return pointer;
}

std::string IdentityName(const services::AuthIdentity& identity) {
  if (!identity.display_name.empty()) return identity.display_name;
  if (!identity.username.empty()) return identity.username;
  return std::to_string(identity.user_id);
}

}  // namespace

struct AccountsController::Network {
  services::CurlHttpClient curl;
  CancellableHttpClient http{curl, kRequestTimeoutMs};
  services::AuthService auth{http};
};

struct AccountsController::WorkerSync {
  std::mutex mutex;
  std::condition_variable finished;
  bool done = false;
};

struct AccountsController::WorkerResult {
  // Migration and reconcile ran (the check after start).
  bool startup = false;
  // The worker ends after this result.
  bool last = false;
  bool loaded = false;
  runtime::AccountStoreSnapshot snapshot;
  std::string load_error;
  std::optional<runtime::AccountMigrationResult> migration;
  std::optional<runtime::AccountReconcileResult> reconcile;
  std::optional<runtime::ProfileRefreshResult> profiles;
};

// ---- creation
// ---------------------------------------------------------------

std::shared_ptr<AccountsController> AccountsController::ForContext(
    LauncherContext* context) {
  auto& controllers = Controllers();
  const auto found = controllers.find(context);
  if (found != controllers.end()) {
    if (std::shared_ptr<AccountsController> existing = found->second.lock()) {
      return existing;
    }
  }
  auto controller = std::make_shared<AccountsController>(context);
  controllers[context] = controller;
  controller->Register();
  return controller;
}

AccountsController::AccountsController(LauncherContext* context)
    : context_(context) {
  const runtime::ProcessEnvironment environment;
  // research/auth.md 5.5.6: with these set the user chose the session, so
  // the store is neither migrated nor used (main.cc resolves no account).
  override_variables_ = AccountOverrideVariables(environment);
  if (environment_override()) return;
  // mocktail runs this window while it holds the instance lock, so no game
  // can run meanwhile (SPEC 4: assume_exclusive instead of probing the
  // lock). The self-test never changes the store.
  store_.emplace(runtime::MakeAccountStoreOptions(
      context->paths(), environment,
      /*assume_exclusive=*/!context->selftest()));
  LoadStore();
  usable_ = !context->selftest() && load_error_.empty();
  if (usable_) {
    network_ = std::make_shared<Network>();
    ConfigureNetwork();
  }
}

AccountsController::~AccountsController() {
  // The context is being destroyed: never call it from here.
  Controllers().erase(context_);
  if (poll_source_ != 0) g_source_remove(poll_source_);
  if (network_ != nullptr) network_->http.Cancel();
  sign_in_.reset();
  StopWorker();
  for (auto& [user_id, avatar] : avatars_) {
    (void)user_id;
    g_object_unref(avatar.texture);
  }
}

void AccountsController::Register() {
  std::shared_ptr<AccountsController> self = shared_from_this();
  context_->AddPlayHook(
      [self](std::string* error) { return self->SelectForPlay(error); });
  // AddDirtySource refreshes the launch bar, which the window builds after
  // the pages: register from the main loop, before the first frame and
  // before any input.
  g_idle_add_full(
      G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        auto* weak = static_cast<std::weak_ptr<AccountsController>*>(data);
        if (std::shared_ptr<AccountsController> controller = weak->lock()) {
          controller->RegisterDirtySource();
        }
        return G_SOURCE_REMOVE;
      },
      new std::weak_ptr<AccountsController>(self),
      [](gpointer data) {
        delete static_cast<std::weak_ptr<AccountsController>*>(data);
      });

  if (!usable_) return;
  // A save, reload or restore may make config.yaml load, or the system
  // proxy resolve, after all.
  std::weak_ptr<AccountsController> weak = self;
  context_->OnSettingChanged([weak](std::string_view key) {
    if (!key.empty()) return;
    if (std::shared_ptr<AccountsController> controller = weak.lock()) {
      controller->RetryNetwork();
    }
  });
  if (!network_blocked_.empty()) return;
  // Network work only once the window is up (research/ux.md 4.5).
  checking_ = true;
  g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE,
      [](gpointer data) -> gboolean {
        auto* weak = static_cast<std::weak_ptr<AccountsController>*>(data);
        if (std::shared_ptr<AccountsController> controller = weak->lock()) {
          if (!controller->shut_down_) controller->StartWorker(true);
        }
        return G_SOURCE_REMOVE;
      },
      new std::weak_ptr<AccountsController>(self),
      [](gpointer data) {
        delete static_cast<std::weak_ptr<AccountsController>*>(data);
      });
}

void AccountsController::RegisterDirtySource() {
  std::shared_ptr<AccountsController> self = shared_from_this();
  DirtySource source;
  source.name = "accounts";
  source.count = [self] { return self->model_.UnsavedCount(); };
  source.problem = [self]() -> std::string {
    if (self->checking_) return _("Checking saved accounts…");
    if (self->sign_in_ != nullptr) {
      return _("Finish signing in, or close the sign-in window");
    }
    return {};
  };
  source.save = [self](std::string* error) {
    return self->SaveAccounts(error);
  };
  source.discard = [self] {
    self->model_.Discard();
    self->Notify();
  };
  context_->AddDirtySource(std::move(source));
}

bool AccountsController::ConfigureNetwork() {
  // mocktail sets the proxy up only after this window (main.cc after
  // LoadRuntimeConfig), and CurlHttpClient reads it from the environment.
  // Runs while no worker thread exists: while the window is being built,
  // or before the first worker once the proxy was not known.
  const LauncherNetworkSetup setup = ResolveLauncherNetwork(
      PlanLauncherNetwork(runtime::ProcessEnvironment(),
                          context_->config_file()),
      [] { return runtime::ResolveSystemProxy(); });
  network_block_ = setup.blocked;
  switch (setup.blocked) {
    case LauncherNetworkBlock::kNone:
      network_blocked_.clear();
      break;
    case LauncherNetworkBlock::kConfig:
      network_blocked_ =
          _("config.yaml does not load, so the proxy for Roblox is not "
            "known, and nothing is sent to Roblox from this window. Fix the "
            "file, then reload it.");
      break;
    case LauncherNetworkBlock::kSystemProxy:
      // main.cc: "Cannot resolve host system proxy" stops the start too.
      network_blocked_ =
          _("The system proxy cannot be determined, so nothing is sent to "
            "Roblox from this window, and Roblox will not start until it "
            "can be.");
      break;
  }
  if (setup.blocked != LauncherNetworkBlock::kNone) {
    g_warning("accounts: no requests to Roblox: %s", setup.error.c_str());
    return false;
  }
  for (const auto& [name, value] : setup.assignments) {
    (void)setenv(name.c_str(), value.c_str(), 1);
  }
  return true;
}

void AccountsController::RetryNetwork() {
  if (shut_down_ || network_blocked_.empty() || worker_running_) return;
  if (!ConfigureNetwork()) return;
  checking_ = true;
  StartWorker(true);
  Changed();
}

void AccountsController::LoadStore() {
  if (!store_.has_value()) return;
  runtime::AccountStoreSnapshot snapshot;
  std::string error;
  if (store_->Load(&snapshot, &error)) {
    model_.SetSnapshot(std::move(snapshot));
    load_error_.clear();
  } else {
    load_error_ = error.empty() ? std::string("unknown error") : error;
  }
}

// ---- worker
// ---------------------------------------------------------------------

void AccountsController::StartWorker(bool startup) {
  if (!usable_ || shut_down_ || !network_blocked_.empty()) return;
  if (worker_running_) {
    if (!startup) profiles_again_ = true;
    return;
  }
  if (worker_.joinable()) worker_.join();
  worker_sync_ = std::make_shared<WorkerSync>();
  worker_running_ = true;
  checking_ = startup;
  worker_ = std::thread(&AccountsController::RunWorker, store_->options(),
                        network_, startup, weak_from_this(), worker_sync_);
}

void AccountsController::RunWorker(runtime::AccountStoreOptions options,
                                   std::shared_ptr<Network> network,
                                   bool startup,
                                   std::weak_ptr<AccountsController> self,
                                   std::shared_ptr<WorkerSync> sync) {
  const runtime::AccountStore store(std::move(options));
  if (startup) {
    auto* result = new WorkerResult();
    result->startup = true;
    runtime::AccountStoreSnapshot before;
    std::string error;
    // research/auth.md 5.5.6: once, also to create the store (signed out)
    // when there is no old session; then 5.5.1 for what the last game
    // changed.
    if (store.Load(&before, &error) &&
        (!before.initialized || before.legacy_session_present)) {
      result->migration = store.MigrateLegacySession(network->auth);
    }
    result->reconcile = store.Reconcile(network->auth);
    result->loaded = store.Load(&result->snapshot, &result->load_error);
    PostResult(self, result);
  }
  auto* result = new WorkerResult();
  result->last = true;
  // Names and headshots, at most daily per account (account_store.h).
  result->profiles = store.RefreshProfiles(network->http, /*force=*/false);
  result->loaded = store.Load(&result->snapshot, &result->load_error);
  PostResult(self, result);
  {
    std::lock_guard<std::mutex> lock(sync->mutex);
    sync->done = true;
  }
  sync->finished.notify_all();
}

void AccountsController::PostResult(
    const std::weak_ptr<AccountsController>& self, WorkerResult* result) {
  struct Posted {
    std::weak_ptr<AccountsController> self;
    std::unique_ptr<WorkerResult> result;
  };
  g_idle_add_full(
      G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        auto* posted = static_cast<Posted*>(data);
        if (std::shared_ptr<AccountsController> controller =
                posted->self.lock()) {
          controller->ApplyResult(*posted->result);
        }
        return G_SOURCE_REMOVE;
      },
      new Posted{self, std::unique_ptr<WorkerResult>(result)},
      [](gpointer data) { delete static_cast<Posted*>(data); });
}

void AccountsController::ApplyResult(WorkerResult& result) {
  if (shut_down_) return;
  if (result.loaded) {
    model_.SetSnapshot(std::move(result.snapshot));
    load_error_.clear();
  } else if (!result.load_error.empty()) {
    load_error_ = result.load_error;
  }
  if (result.startup) {
    checking_ = false;
    if (result.migration.has_value() && !*result.migration) {
      context_->Toast(Format(_("An older saved sign-in could not be moved "
                               "into the account list: %s"),
                             result.migration->error.c_str()));
    }
    if (result.reconcile.has_value()) {
      if (!*result.reconcile) {
        context_->Toast(Format(_("Saved accounts could not be checked: %s"),
                               result.reconcile->error.c_str()));
      }
      // A sign-in inside Roblox during the last game (research/auth.md
      // 5.5.1 step 1) is now a saved account.
      for (const std::int64_t user_id : result.reconcile->filed) {
        if (const runtime::SavedAccount* account =
                model_.FindVisible(user_id)) {
          context_->Toast(
              Format(_("Saved %s, the account you signed in with in Roblox"),
                     ShownName(*account).c_str()));
        }
      }
    }
  }
  if (result.last) {
    worker_running_ = false;
    if (worker_.joinable()) worker_.join();
    if (profiles_again_) {
      profiles_again_ = false;
      StartWorker(false);
    }
  }
  Changed();
}

void AccountsController::StopWorker() {
  if (!worker_.joinable()) return;
  bool done = false;
  {
    std::unique_lock<std::mutex> lock(worker_sync_->mutex);
    done = worker_sync_->finished.wait_for(
        lock, kShutdownWait, [this] { return worker_sync_->done; });
  }
  if (done) {
    worker_.join();
  } else {
    // A request is still in flight; it ends within kRequestTimeoutMs and
    // the thread only holds its own copies (options, network) by then.
    worker_.detach();
  }
}

// ---- state
// ----------------------------------------------------------------------

std::string AccountsController::SelectionUnavailableReason() const {
  if (environment_override()) {
    return _("Account managed by environment override");
  }
  if (!load_error_.empty()) return _("Saved accounts cannot be read");
  return {};
}

std::string AccountsController::AddUnavailableReason() const {
  std::string reason = SelectionUnavailableReason();
  if (!reason.empty()) return reason;
  switch (network_block_) {
    case LauncherNetworkBlock::kNone:
      break;
    case LauncherNetworkBlock::kConfig:
      return _("Fix config.yaml first: it sets how Roblox is reached");
    case LauncherNetworkBlock::kSystemProxy:
      return _("The system proxy cannot be determined");
  }
  if (checking_) return _("Wait until the saved accounts are checked");
  if (sign_in_ != nullptr) return _("A sign-in window is already open");
  return {};
}

AccountChipState AccountsController::chip() const {
  return DescribeAccountChip(model_, environment_override(),
                             !load_error_.empty());
}

GdkPaintable* AccountsController::AvatarFor(
    const runtime::SavedAccount& account) {
  if (account.avatar_file.empty()) return nullptr;
  std::error_code error;
  const std::filesystem::file_time_type modified =
      std::filesystem::last_write_time(account.avatar_file, error);
  if (error) return nullptr;
  const auto found = avatars_.find(account.user_id);
  if (found != avatars_.end() && found->second.modified == modified) {
    return GDK_PAINTABLE(found->second.texture);
  }
  // A 150×150 PNG the store checked (account_store.cc); AdwAvatar scales it
  // at the surface's scale, so it stays sharp at 1.25-2×.
  GError* load_error = nullptr;
  GdkTexture* texture =
      gdk_texture_new_from_filename(account.avatar_file.c_str(), &load_error);
  if (texture == nullptr) {
    g_clear_error(&load_error);
    return nullptr;
  }
  if (found != avatars_.end()) {
    g_object_unref(found->second.texture);
    found->second = {texture, modified};
  } else {
    avatars_.emplace(account.user_id, AvatarEntry{texture, modified});
  }
  return GDK_PAINTABLE(texture);
}

// ---- actions
// --------------------------------------------------------------------

void AccountsController::Choose(const runtime::ActiveAccountPointer& pointer) {
  const std::string reason = SelectionUnavailableReason();
  if (!reason.empty()) {
    context_->Toast(reason);
    return;
  }
  model_.Choose(pointer);
  Changed();
}

void AccountsController::StartSignIn(std::optional<std::int64_t> again_for) {
  if (context_->selftest()) {
    context_->Toast(_("Signing in is not available in the self-test"));
    return;
  }
  const std::string reason = AddUnavailableReason();
  if (!reason.empty()) {
    context_->Toast(reason);
    return;
  }
  const std::filesystem::path helper = runtime::ResolveWebViewHelperPath();
  if (helper.empty()) {
    context_->Toast(
        _("The sign-in window is not installed "
          "(mocktail_webview_helper is missing)"));
    return;
  }
  std::string error;
  // The website session WebKit kept must not sign in an account by itself
  // (research/auth.md 5.5.3 step 2).
  if (!store_->PrepareBrowserSignIn(&error)) {
    context_->Toast(Format(_("The sign-in window could not be prepared: %s"),
                           error.c_str()));
    return;
  }
  auto store_error = std::make_shared<std::string>();
  const runtime::AccountStoreOptions options = store_->options();
  runtime::BrowserSignInOptions sign_in_options;
  sign_in_options.ready_timeout = kSignInReadyTimeout;
  // Runs on the session's check thread once Roblox accepted the session.
  // The selection stays staged: the new account is chosen in the window
  // and written on Save or Play like any other choice.
  auto session = std::make_unique<runtime::BrowserSignInSession>(
      network_->auth,
      [options, store_error](const services::AuthIdentity& identity,
                             std::string_view cookie_value) {
        std::string add_error;
        const bool added = runtime::AccountStore(options).AddValidatedSession(
            identity, cookie_value, /*make_active=*/false, &add_error);
        if (!added) *store_error = add_error;
        return added;
      },
      sign_in_options);
  std::string title = _("Add Roblox account");
  if (again_for.has_value()) {
    if (const runtime::SavedAccount* account = model_.FindVisible(*again_for)) {
      title = Format(_("Sign in to Roblox again as %s"),
                     ShownName(*account).c_str());
    }
  }
  if (!session->Start(helper, runtime::kBrowserSignInUrl, title,
                      /*clear_jar=*/true, &error)) {
    context_->Toast(
        Format(_("The sign-in window could not open: %s"), error.c_str()));
    return;
  }
  sign_in_ = std::move(session);
  sign_in_again_for_ = again_for;
  signed_in_.reset();
  sign_in_store_error_ = std::move(store_error);
  sign_in_unverified_shown_ = false;
  poll_source_ = g_timeout_add(kSignInPollMilliseconds, PollSignIn, this);
  Changed();
}

void AccountsController::CancelSignIn() {
  if (sign_in_ != nullptr) sign_in_->Cancel();
}

gboolean AccountsController::PollSignIn(gpointer data) {
  auto* self = static_cast<AccountsController*>(data);
  if (self->sign_in_ == nullptr) {
    self->poll_source_ = 0;
    return G_SOURCE_REMOVE;
  }
  bool closed = false;
  for (const runtime::BrowserSignInEvent& event : self->sign_in_->Poll()) {
    switch (event.type) {
      case runtime::BrowserSignInEventType::kAccepted:
        self->signed_in_ = event.identity;
        break;
      case runtime::BrowserSignInEventType::kRejected:
        // browser_sign_in.cc clears the refused session; the window stays.
        self->context_->Toast(
            _("Roblox did not accept that sign-in. Try "
              "again in the sign-in window."));
        break;
      case runtime::BrowserSignInEventType::kUnverified:
        if (!self->sign_in_unverified_shown_) {
          self->sign_in_unverified_shown_ = true;
          self->context_->Toast(
              _("Roblox could not be reached to check the "
                "sign-in. Mocktail tries again in a few "
                "seconds."));
        }
        break;
      case runtime::BrowserSignInEventType::kFailed: {
        std::string message = event.message;
        if (self->sign_in_store_error_ != nullptr &&
            !self->sign_in_store_error_->empty()) {
          message += ": " + *self->sign_in_store_error_;
        }
        self->context_->Toast(
            Format(_("Signing in did not finish: %s"), message.c_str()));
        break;
      }
      case runtime::BrowserSignInEventType::kClosed:
        closed = true;
        break;
    }
  }
  if (!closed) return G_SOURCE_CONTINUE;
  self->poll_source_ = 0;
  self->FinishSignIn();
  return G_SOURCE_REMOVE;
}

void AccountsController::FinishSignIn() {
  sign_in_.reset();
  sign_in_store_error_.reset();
  const std::optional<services::AuthIdentity> identity = std::move(signed_in_);
  signed_in_.reset();
  const std::optional<std::int64_t> again_for = sign_in_again_for_;
  sign_in_again_for_.reset();
  if (identity.has_value()) {
    std::string previous;
    if (again_for.has_value() && *again_for != identity->user_id) {
      if (const runtime::SavedAccount* account =
              model_.FindVisible(*again_for)) {
        previous = ShownName(*account);
      }
    }
    LoadStore();
    model_.Choose(AccountPointer(identity->user_id));
    const std::string name = IdentityName(*identity);
    if (!previous.empty()) {
      // research/auth.md 5.5.3 step 5: another account signed in; it is
      // added, the expired one stays.
      context_->Toast(Format(_("Signed in as %s, a different account than "
                               "%s. Both are saved."),
                             name.c_str(), previous.c_str()));
    } else {
      context_->Toast(Format(_("Signed in as %s"), name.c_str()));
    }
    // Its headshot.
    StartWorker(false);
  }
  Changed();
}

void AccountsController::ConfirmRemoval(std::int64_t user_id) {
  const runtime::SavedAccount* account = model_.FindVisible(user_id);
  if (account == nullptr) return;
  const std::string reason = SelectionUnavailableReason();
  if (!reason.empty()) {
    context_->Toast(reason);
    return;
  }
  const std::string name = ShownName(*account);
  // account_store.h RemoveAccount: the folder, the avatar and the session
  // artifacts go; there is no server-side sign-out.
  AdwDialog* dialog = adw_alert_dialog_new(
      Format(_("Remove %s?"), name.c_str()).c_str(),
      _("Mocktail deletes this account's saved sign-in from this computer "
        "when you save or play. It does not sign out at Roblox: the session "
        "stays valid until it expires, and your sign-ins elsewhere are not "
        "affected. To end it everywhere, use “Log out of all other sessions” "
        "in Roblox's security settings. To play with it here again, sign in "
        "again."));
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "remove",
                                 _("_Remove"), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "remove",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  struct Response {
    std::weak_ptr<AccountsController> self;
    std::int64_t user_id;
    std::string name;
  };
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
        auto* removal = static_cast<Response*>(data);
        if (g_strcmp0(response, "remove") != 0) return;
        if (std::shared_ptr<AccountsController> self = removal->self.lock()) {
          if (!self->shut_down_) {
            self->StageRemoval(removal->user_id, removal->name);
          }
        }
      }),
      new Response{weak_from_this(), user_id, name},
      +[](gpointer data, GClosure*) { delete static_cast<Response*>(data); },
      GConnectFlags(0));
  adw_dialog_present(dialog, GTK_WIDGET(context_->window()));
}

void AccountsController::StageRemoval(std::int64_t user_id,
                                      const std::string& name) {
  const std::optional<AccountsModel::StagedRemoval> removal =
      model_.StageRemoval(user_id);
  if (!removal.has_value()) return;
  Changed();
  std::weak_ptr<AccountsController> weak = weak_from_this();
  context_->Toast(
      Format(_("%s will be removed when you save or play"), name.c_str()),
      _("Undo"), [weak, staged = *removal] {
        std::shared_ptr<AccountsController> self = weak.lock();
        if (self == nullptr || self->shut_down_) return;
        self->model_.UndoRemoval(staged);
        self->Changed();
      });
}

void AccountsController::PlaySignedOut() {
  std::string reason = SelectionUnavailableReason();
  if (reason.empty()) reason = context_->play_blocker();
  if (!reason.empty()) {
    context_->Toast(reason);
    return;
  }
  runtime::ActiveAccountPointer guest;
  model_.Choose(guest);
  Changed();
  context_->Play();
}

// ---- saving
// ---------------------------------------------------------------------

bool AccountsController::SaveAccounts(std::string* error) {
  // The self-test and an environment override never write the store.
  if (!usable_ || !store_.has_value()) return true;
  std::string store_error;
  const std::vector<std::int64_t> removals = model_.staged_removals();
  for (const std::int64_t user_id : removals) {
    if (!store_->RemoveAccount(user_id, &store_error)) {
      *error = Format(_("Saved accounts: %s"), store_error.c_str());
      LoadStore();
      Notify();
      return false;
    }
    model_.FinishRemoval(user_id);
    const auto avatar = avatars_.find(user_id);
    if (avatar != avatars_.end()) {
      g_object_unref(avatar->second.texture);
      avatars_.erase(avatar);
    }
  }
  if (model_.SelectionChanged()) {
    const std::optional<runtime::ActiveAccountPointer> selection =
        model_.Selection();
    if (selection.has_value() &&
        !store_->SelectForLaunch(*selection, &store_error)) {
      *error = Format(_("Saved accounts: %s"), store_error.c_str());
      LoadStore();
      Notify();
      return false;
    }
  }
  LoadStore();
  Notify();
  return true;
}

bool AccountsController::SelectForPlay(std::string* error) {
  if (!usable_ || !store_.has_value()) return true;
  // Without a store and without a choice the runtime keeps today's auth
  // root (research/auth.md 5.4), which may hold a session not moved yet.
  const std::optional<runtime::ActiveAccountPointer> selection =
      model_.Selection();
  if (!selection.has_value()) return true;
  // Again even when Save just wrote it: SelectForLaunch also clears the web
  // session and Roblox's cached sign-in data when the shared app data last
  // saw another account, or for every guest start (account_store.h).
  std::string store_error;
  if (!store_->SelectForLaunch(*selection, &store_error)) {
    *error = Format(_("The account for this launch could not be selected: %s"),
                    store_error.c_str());
    return false;
  }
  return true;
}

// ---- observers
// ------------------------------------------------------------------

AccountsController::ObserverId AccountsController::AddObserver(
    std::function<void()> observer) {
  const ObserverId id = next_observer_++;
  observers_.emplace_back(id, std::move(observer));
  return id;
}

void AccountsController::RemoveObserver(ObserverId id) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (it->first == id) {
      observers_.erase(it);
      return;
    }
  }
}

void AccountsController::Notify() {
  const auto observers = observers_;
  for (const auto& [id, observer] : observers) {
    (void)id;
    if (observer) observer();
  }
}

void AccountsController::Changed() {
  Notify();
  if (!shut_down_) context_->NotifyDirtyChanged();
}

void AccountsController::Shutdown() {
  if (shut_down_) return;
  shut_down_ = true;
  observers_.clear();
  if (poll_source_ != 0) {
    g_source_remove(poll_source_);
    poll_source_ = 0;
  }
  // New requests fail at once; the sign-in window closes and its check, if
  // any, ends after the request in flight.
  if (network_ != nullptr) network_->http.Cancel();
  sign_in_.reset();
  StopWorker();
  worker_running_ = false;
  checking_ = false;
}

}  // namespace mocktail::launcher_ui
