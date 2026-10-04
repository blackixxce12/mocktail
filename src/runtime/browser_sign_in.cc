#include "runtime/browser_sign_in.h"

#include <cstdlib>
#include <mutex>
#include <system_error>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::chrono::milliseconds kReadyPollTimeout(1);

struct HelperExitContext {
  std::mutex mutex;
  bool finished = false;
};

class HelperSignInWindow final : public BrowserSignInWindow {
 public:
  HelperSignInWindow(std::shared_ptr<WebViewHelperProcess> process,
                     std::shared_ptr<HelperExitContext> exit_context)
      : process_(std::move(process)), exit_context_(std::move(exit_context)) {}

  bool exited() const override {
    std::lock_guard<std::mutex> lock(exit_context_->mutex);
    return exit_context_->finished;
  }
  bool WaitUntilReady(std::chrono::milliseconds timeout) override {
    return process_->WaitUntilReady(timeout);
  }
  bool DrainEvents(std::vector<WebViewHelperEvent>* events) override {
    return process_->DrainEvents(events);
  }
  bool SetRobloxCookie(std::string_view value) override {
    return process_->SetRobloxCookie(value);
  }
  bool ClearRobloxCookie() override { return process_->ClearRobloxCookie(); }
  bool SetTitle(std::string_view title) override {
    return process_->SetTitle(title);
  }
  bool SetVisible(bool visible) override {
    return process_->SetVisible(visible);
  }
  bool RequestClose() override { return process_->RequestClose(); }

 private:
  std::shared_ptr<WebViewHelperProcess> process_;
  std::shared_ptr<HelperExitContext> exit_context_;
};

BrowserSignInEvent MakeEvent(BrowserSignInEventType type,
                             std::string message = {}) {
  BrowserSignInEvent event;
  event.type = type;
  event.message = std::move(message);
  return event;
}

}  // namespace

std::filesystem::path ResolveWebViewHelperPath() {
  std::filesystem::path helper;
  const char* helper_override = std::getenv("MOCKTAIL_WEBVIEW_HELPER");
  if (helper_override != nullptr && helper_override[0] != '\0') {
    helper = helper_override;
  } else {
    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
      helper = executable.parent_path() / "mocktail_webview_helper";
    }
  }
  std::error_code error;
  if (helper.empty() || !std::filesystem::exists(helper, error)) {
    return {};
  }
  return helper;
}

BrowserSignInSession::BrowserSignInSession(services::AuthService& auth_service,
                                           BrowserSignInPersistCallback persist,
                                           BrowserSignInOptions options)
    : auth_service_(auth_service),
      persist_(std::move(persist)),
      options_(options) {}

BrowserSignInSession::~BrowserSignInSession() {
  if (active() && window_ != nullptr && !window_->exited()) {
    (void)window_->RequestClose();
  }
  if (check_.valid()) {
    check_.wait();
  }
  Clear();
}

bool BrowserSignInSession::Start(const std::filesystem::path& helper,
                                 std::string_view url, std::string_view title,
                                 bool clear_jar, std::string* error) {
  if (state_ != State::kIdle) {
    if (error != nullptr) {
      *error = "a sign-in window was already opened";
    }
    return false;
  }
  auto exit_context = std::make_shared<HelperExitContext>();
  WebViewHelperExitObserver exit_observer;
  exit_observer.context = exit_context;
  exit_observer.on_exit = [](void* context) {
    auto* exit = static_cast<HelperExitContext*>(context);
    std::lock_guard<std::mutex> lock(exit->mutex);
    exit->finished = true;
  };
  const WebViewHelperLaunchResult launched =
      LaunchWebViewHelper(helper, url, exit_observer);
  if (!launched || launched.process == nullptr) {
    if (error != nullptr) {
      *error = launched.error.empty() ? "cannot start the sign-in window"
                                      : launched.error;
    }
    return false;
  }
  return Attach(
      std::make_unique<HelperSignInWindow>(launched.process, exit_context),
      title, clear_jar);
}

bool BrowserSignInSession::Attach(std::unique_ptr<BrowserSignInWindow> window,
                                  std::string_view title, bool clear_jar) {
  if (state_ != State::kIdle || window == nullptr) {
    return false;
  }
  window_ = std::move(window);
  title_ = std::string(title);
  clear_jar_ = clear_jar;
  ready_deadline_ = std::chrono::steady_clock::now() + options_.ready_timeout;
  state_ = State::kStarting;
  return true;
}

void BrowserSignInSession::Cancel() {
  if (active() && window_ != nullptr) {
    (void)window_->RequestClose();
  }
}

std::vector<BrowserSignInEvent> BrowserSignInSession::Poll() {
  std::vector<BrowserSignInEvent> events;
  if (state_ == State::kStarting) {
    if (window_->exited()) {
      state_ = State::kDone;
      events.push_back(MakeEvent(BrowserSignInEventType::kFailed,
                                 "the sign-in window closed while loading"));
      events.push_back(MakeEvent(BrowserSignInEventType::kClosed));
      return events;
    }
    if (window_->WaitUntilReady(kReadyPollTimeout)) {
      // A first-launch sign-in keeps the persistent jar, so a session from an
      // earlier browser sign-in is offered again (and checked). Adding an
      // account must not report the account WebKit already holds.
      if (clear_jar_) {
        (void)window_->ClearRobloxCookie();
      } else {
        (void)window_->SetRobloxCookie("");
      }
      (void)window_->SetTitle(title_);
      (void)window_->SetVisible(true);
      state_ = State::kOpen;
    } else if (std::chrono::steady_clock::now() >= ready_deadline_) {
      (void)window_->RequestClose();
      state_ = State::kDone;
      events.push_back(MakeEvent(BrowserSignInEventType::kFailed,
                                 "the sign-in window did not load"));
      events.push_back(MakeEvent(BrowserSignInEventType::kClosed));
    }
    return events;
  }
  if (state_ != State::kOpen) {
    return events;
  }

  if (window_->exited()) {
    // A check still running decides whether the session was kept.
    if (check_.valid()) {
      if (check_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        return events;
      }
      CheckResult result = check_.get();
      if (result.status == BrowserSignInStatus::kAccepted) {
        accepted_ = true;
        BrowserSignInEvent accepted =
            MakeEvent(BrowserSignInEventType::kAccepted);
        accepted.identity = std::move(result.identity);
        events.push_back(std::move(accepted));
      }
    }
    Clear();
    state_ = State::kDone;
    events.push_back(MakeEvent(BrowserSignInEventType::kClosed));
    return events;
  }

  // WebKit reports a session left from an earlier sign-in as soon as the page
  // opens. Roblox must accept a session before it is saved and the window
  // closes; a rejected one is cleared so the user can sign in again. The check
  // runs on a worker so helper events keep draining meanwhile.
  std::vector<WebViewHelperEvent> helper_events;
  if (window_->DrainEvents(&helper_events)) {
    for (WebViewHelperEvent& event : helper_events) {
      if (event.type == WebViewHelperEventType::kRobloxCookie && !accepted_) {
        SecurelyClearString(&captured_cookie_);
        captured_cookie_ = event.payload;
        next_check_ = std::chrono::steady_clock::now();
      }
      SecurelyClearString(&event.payload);
    }
  }
  if (check_.valid() &&
      check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    FinishCheck(&events);
  }
  StartCheck();
  return events;
}

void BrowserSignInSession::FinishCheck(
    std::vector<BrowserSignInEvent>* events) {
  CheckResult result = check_.get();
  switch (result.status) {
    case BrowserSignInStatus::kAccepted: {
      accepted_ = true;
      (void)window_->RequestClose();
      BrowserSignInEvent accepted =
          MakeEvent(BrowserSignInEventType::kAccepted);
      accepted.identity = std::move(result.identity);
      events->push_back(std::move(accepted));
      break;
    }
    case BrowserSignInStatus::kRejected:
      events->push_back(MakeEvent(BrowserSignInEventType::kRejected));
      // Roblox serves a revoked session as signed out, so the page already
      // offers sign-in. Only drop the session from WebKit: a reload could
      // throw away a sign-in, captcha or two-step check in progress. A
      // session reported meanwhile replaced the rejected one.
      if (captured_cookie_.empty()) {
        (void)window_->ClearRobloxCookie();
      }
      break;
    case BrowserSignInStatus::kUnverified:
      events->push_back(MakeEvent(BrowserSignInEventType::kUnverified));
      if (captured_cookie_.empty()) {
        captured_cookie_ = checked_cookie_;
        next_check_ = std::chrono::steady_clock::now() + options_.recheck_delay;
      }
      break;
    case BrowserSignInStatus::kStoreFailed:
      events->push_back(
          MakeEvent(BrowserSignInEventType::kFailed,
                    "could not save the browser sign-in session"));
      (void)window_->RequestClose();
      break;
  }
  SecurelyClearString(&checked_cookie_);
}

void BrowserSignInSession::StartCheck() {
  if (accepted_ || check_.valid() || captured_cookie_.empty() ||
      std::chrono::steady_clock::now() < next_check_) {
    return;
  }
  checked_cookie_ = std::move(captured_cookie_);
  captured_cookie_.clear();
  // Only the worker reads checked_cookie_ until the result is collected.
  check_ = std::async(std::launch::async, [this]() {
    CheckResult result;
    RobloxSessionValidation validation =
        ValidateRobloxSession(auth_service_, checked_cookie_);
    result.status = validation.status;
    if (validation.status == BrowserSignInStatus::kAccepted) {
      if (!persist_ || !persist_(validation.identity, checked_cookie_)) {
        result.status = BrowserSignInStatus::kStoreFailed;
      }
      result.identity = std::move(validation.identity);
    }
    return result;
  });
}

void BrowserSignInSession::Clear() {
  SecurelyClearString(&captured_cookie_);
  SecurelyClearString(&checked_cookie_);
}

}  // namespace runtime
}  // namespace mocktail
