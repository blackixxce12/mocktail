#ifndef MOCKTAIL_RUNTIME_BROWSER_SIGN_IN_H_
#define MOCKTAIL_RUNTIME_BROWSER_SIGN_IN_H_

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/private_credential_file.h"
#include "runtime/webview_helper_launcher.h"
#include "services/auth_service.h"

namespace mocktail {
namespace runtime {

inline constexpr std::string_view kBrowserSignInUrl =
    "https://www.roblox.com/login";

// MOCKTAIL_WEBVIEW_HELPER, else mocktail_webview_helper next to the running
// executable. Empty when neither exists.
std::filesystem::path ResolveWebViewHelperPath();

// The window a sign-in drives. Production wraps the WebKit helper; tests
// supply a fake.
class BrowserSignInWindow {
 public:
  virtual ~BrowserSignInWindow() = default;

  // True once the window process is gone.
  virtual bool exited() const = 0;
  // Waits at most timeout for the window to load; zero only checks.
  virtual bool WaitUntilReady(std::chrono::milliseconds timeout) = 0;
  virtual bool DrainEvents(std::vector<WebViewHelperEvent>* events) = 0;
  // An empty value keeps WebKit's persistent cookie jar authoritative.
  virtual bool SetRobloxCookie(std::string_view value) = 0;
  virtual bool ClearRobloxCookie() = 0;
  virtual bool SetTitle(std::string_view title) = 0;
  virtual bool SetVisible(bool visible) = 0;
  virtual bool RequestClose() = 0;
};

enum class BrowserSignInEventType {
  // Roblox accepted the session and the persist callback stored it. The
  // window is closing.
  kAccepted,
  // Roblox refused the session; it was cleared from WebKit and the window
  // stays open for another sign-in.
  kRejected,
  // Roblox could not be asked; the session is checked again shortly.
  kUnverified,
  // The window is gone. Always the last event of a session.
  kClosed,
  // The window could not start, or an accepted session could not be stored.
  kFailed,
};

struct BrowserSignInEvent {
  BrowserSignInEventType type = BrowserSignInEventType::kClosed;
  // kAccepted only. Public account names; never the session.
  services::AuthIdentity identity;
  // kFailed only. Never contains the session.
  std::string message;
};

// Stores a session Roblox accepted, on the validation worker thread. Returns
// false when it could not be stored. The value is the raw or prefixed cookie
// WebKit reported; the session clears its own copy afterwards.
using BrowserSignInPersistCallback = std::function<bool(
    const services::AuthIdentity& identity, std::string_view cookie_value)>;

struct BrowserSignInOptions {
  std::chrono::milliseconds ready_timeout{5000};
  std::chrono::milliseconds recheck_delay{5000};
};

// A Roblox sign-in in the WebKit helper window that never blocks its caller:
// Poll() it every few tens of milliseconds from a loop or a GLib timeout.
// Each session WebKit reports is checked with Roblox on a worker before the
// persist callback sees it, because the jar can still hold a revoked one: an
// accepted session is stored and closes the window, a rejected one is cleared
// from WebKit while the window stays open, and one that cannot be checked is
// checked again after recheck_delay. A session reported meanwhile replaces
// the pending one. Sessions never pass through argv, the environment or logs.
class BrowserSignInSession final {
 public:
  BrowserSignInSession(services::AuthService& auth_service,
                       BrowserSignInPersistCallback persist,
                       BrowserSignInOptions options = {});
  // Closes a window that is still open and waits for a running check.
  ~BrowserSignInSession();

  BrowserSignInSession(const BrowserSignInSession&) = delete;
  BrowserSignInSession& operator=(const BrowserSignInSession&) = delete;
  BrowserSignInSession(BrowserSignInSession&&) = delete;
  BrowserSignInSession& operator=(BrowserSignInSession&&) = delete;

  // Opens the helper on url without waiting for it. clear_jar drops every
  // Roblox session WebKit kept (adding an account); otherwise the persistent
  // jar stays authoritative (the first-launch sign-in). A false return leaves
  // the session idle with the reason in error.
  bool Start(const std::filesystem::path& helper, std::string_view url,
             std::string_view title, bool clear_jar, std::string* error);
  // Drives an already launched window instead of the WebKit helper.
  bool Attach(std::unique_ptr<BrowserSignInWindow> window,
              std::string_view title, bool clear_jar);

  // Events since the last call, in order; kClosed comes last, once.
  std::vector<BrowserSignInEvent> Poll();
  // Asks the window to close; Poll reports kClosed when it is gone.
  void Cancel();

  bool active() const {
    return state_ == State::kStarting || state_ == State::kOpen;
  }
  bool accepted() const { return accepted_; }

 private:
  enum class State { kIdle, kStarting, kOpen, kDone };

  struct CheckResult {
    BrowserSignInStatus status = BrowserSignInStatus::kUnverified;
    services::AuthIdentity identity;
  };

  void FinishCheck(std::vector<BrowserSignInEvent>* events);
  void StartCheck();
  void Clear();

  services::AuthService& auth_service_;
  BrowserSignInPersistCallback persist_;
  BrowserSignInOptions options_;
  std::unique_ptr<BrowserSignInWindow> window_;
  State state_ = State::kIdle;
  std::string title_;
  bool clear_jar_ = false;
  bool accepted_ = false;
  std::chrono::steady_clock::time_point ready_deadline_;
  std::chrono::steady_clock::time_point next_check_;
  // Sensitive: cleared after every check and on destruction.
  std::string captured_cookie_;
  std::string checked_cookie_;
  std::future<CheckResult> check_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_BROWSER_SIGN_IN_H_
