#include "runtime/browser_sign_in.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "services/auth_service.h"
#include "services/http_client.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr char kFirstSession[] = "_|test-browser-sign-in-first";
constexpr char kSecondSession[] = "_|test-browser-sign-in-second";
constexpr char kIdentityBody[] =
    R"({"id":42,"name":"builder","displayName":"Builder"})";

// Thread-safe: the session asks Roblox from its worker.
class FakeHttpClient final : public services::HttpClient {
 public:
  services::HttpResponse Get(const services::HttpRequest& request) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++request_count_;
    cookies_.push_back(request.headers.size() > 1 ? request.headers[1] : "");
    released_.wait(lock, [this]() { return !held_; });
    if (!responses_.empty()) {
      services::HttpResponse response = responses_.front();
      responses_.erase(responses_.begin());
      return response;
    }
    return fallback_;
  }

  void Queue(services::HttpResponse response) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_.push_back(std::move(response));
  }
  void SetFallback(services::HttpResponse response) {
    std::lock_guard<std::mutex> lock(mutex_);
    fallback_ = std::move(response);
  }
  void Hold() {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = true;
  }
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_ = false;
    }
    released_.notify_all();
  }
  int request_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_count_;
  }
  std::vector<std::string> cookies() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cookies_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable released_;
  bool held_ = false;
  int request_count_ = 0;
  std::vector<services::HttpResponse> responses_;
  services::HttpResponse fallback_{false, 0, {}, "offline"};
  std::vector<std::string> cookies_;
};

struct FakeWindowState {
  std::mutex mutex;
  bool ready = true;
  bool exited = false;
  std::vector<WebViewHelperEvent> pending;
  std::vector<std::string> set_cookie_values;
  int clear_calls = 0;
  int close_calls = 0;
  std::string title;
  bool visible = false;

  void Report(std::string cookie) {
    std::lock_guard<std::mutex> lock(mutex);
    WebViewHelperEvent event;
    event.type = WebViewHelperEventType::kRobloxCookie;
    event.payload = std::move(cookie);
    pending.push_back(std::move(event));
  }
  void Exit() {
    std::lock_guard<std::mutex> lock(mutex);
    exited = true;
  }
};

class FakeWindow final : public BrowserSignInWindow {
 public:
  explicit FakeWindow(std::shared_ptr<FakeWindowState> state)
      : state_(std::move(state)) {}

  bool exited() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->exited;
  }
  bool WaitUntilReady(std::chrono::milliseconds) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->ready;
  }
  bool DrainEvents(std::vector<WebViewHelperEvent>* events) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (WebViewHelperEvent& event : state_->pending) {
      events->push_back(std::move(event));
    }
    state_->pending.clear();
    return true;
  }
  bool SetRobloxCookie(std::string_view value) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->set_cookie_values.emplace_back(value);
    return true;
  }
  bool ClearRobloxCookie() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->clear_calls;
    return true;
  }
  bool SetTitle(std::string_view title) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->title = std::string(title);
    return true;
  }
  bool SetVisible(bool visible) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->visible = visible;
    return true;
  }
  bool RequestClose() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->close_calls;
    return true;
  }

 private:
  std::shared_ptr<FakeWindowState> state_;
};

struct PersistRecord {
  std::mutex mutex;
  bool result = true;
  int calls = 0;
  services::AuthIdentity identity;
  std::string value;
};

class BrowserSignInTest : public ::testing::Test {
 protected:
  BrowserSignInTest()
      : auth_(http_),
        window_(std::make_shared<FakeWindowState>()),
        persisted_(std::make_shared<PersistRecord>()) {}

  std::unique_ptr<BrowserSignInSession> MakeSession(
      BrowserSignInOptions options = {std::chrono::milliseconds(5000),
                                      std::chrono::milliseconds(20)}) {
    std::shared_ptr<PersistRecord> record = persisted_;
    return std::make_unique<BrowserSignInSession>(
        auth_,
        [record](const services::AuthIdentity& identity,
                 std::string_view value) {
          std::lock_guard<std::mutex> lock(record->mutex);
          ++record->calls;
          record->identity = identity;
          record->value = std::string(value);
          return record->result;
        },
        options);
  }

  bool Open(BrowserSignInSession* session, bool clear_jar = false) {
    if (!session->Attach(std::make_unique<FakeWindow>(window_), "Sign in",
                         clear_jar)) {
      return false;
    }
    return session->Poll().empty() && session->active();
  }

  // Polls until an event of the given type arrives; returns everything seen.
  std::vector<BrowserSignInEvent> PollUntil(BrowserSignInSession* session,
                                            BrowserSignInEventType type) {
    std::vector<BrowserSignInEvent> seen;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      for (BrowserSignInEvent& event : session->Poll()) {
        const bool done = event.type == type;
        seen.push_back(std::move(event));
        if (done) {
          return seen;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ADD_FAILURE() << "timed out waiting for a sign-in event";
    return seen;
  }

  static std::vector<BrowserSignInEventType> Types(
      const std::vector<BrowserSignInEvent>& events) {
    std::vector<BrowserSignInEventType> types;
    for (const BrowserSignInEvent& event : events) {
      types.push_back(event.type);
    }
    return types;
  }

  FakeHttpClient http_;
  services::AuthService auth_;
  std::shared_ptr<FakeWindowState> window_;
  std::shared_ptr<PersistRecord> persisted_;
};

TEST_F(BrowserSignInTest, FirstLaunchKeepsThePersistentJar) {
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  std::lock_guard<std::mutex> lock(window_->mutex);
  EXPECT_EQ(window_->set_cookie_values, std::vector<std::string>{""});
  EXPECT_EQ(window_->clear_calls, 0);
  EXPECT_EQ(window_->title, "Sign in");
  EXPECT_TRUE(window_->visible);
}

TEST_F(BrowserSignInTest, AddingAnAccountClearsTheJarFirst) {
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get(), true));
  std::lock_guard<std::mutex> lock(window_->mutex);
  EXPECT_TRUE(window_->set_cookie_values.empty());
  EXPECT_EQ(window_->clear_calls, 1);
  EXPECT_TRUE(window_->visible);
}

TEST_F(BrowserSignInTest, AcceptedSessionIsStoredWithItsAccountAndCloses) {
  http_.Queue({true, 200, kIdentityBody, {}});
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);

  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kAccepted);
  ASSERT_EQ(Types(events), std::vector<BrowserSignInEventType>{
                               BrowserSignInEventType::kAccepted});
  EXPECT_EQ(events.back().identity.user_id, 42);
  EXPECT_EQ(events.back().identity.username, "builder");
  EXPECT_EQ(events.back().identity.display_name, "Builder");
  EXPECT_TRUE(session->accepted());
  {
    std::lock_guard<std::mutex> lock(persisted_->mutex);
    EXPECT_EQ(persisted_->calls, 1);
    EXPECT_EQ(persisted_->value, kFirstSession);
    EXPECT_EQ(persisted_->identity.user_id, 42);
  }
  {
    std::lock_guard<std::mutex> lock(window_->mutex);
    EXPECT_EQ(window_->close_calls, 1);
  }
  EXPECT_EQ(http_.cookies(),
            std::vector<std::string>{std::string("Cookie: .ROBLOSECURITY=") +
                                     kFirstSession});

  // A session reported while the window closes is not checked.
  window_->Report(kSecondSession);
  (void)session->Poll();
  window_->Exit();
  EXPECT_EQ(
      Types(PollUntil(session.get(), BrowserSignInEventType::kClosed)),
      std::vector<BrowserSignInEventType>{BrowserSignInEventType::kClosed});
  EXPECT_EQ(http_.request_count(), 1);
  EXPECT_FALSE(session->active());
  EXPECT_TRUE(session->Poll().empty());
}

TEST_F(BrowserSignInTest, RejectedSessionIsClearedAndTheWindowStaysOpen) {
  http_.Queue({true, 401, "{}", {}});
  http_.Queue({true, 200, kIdentityBody, {}});
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);

  EXPECT_EQ(
      Types(PollUntil(session.get(), BrowserSignInEventType::kRejected)),
      std::vector<BrowserSignInEventType>{BrowserSignInEventType::kRejected});
  {
    std::lock_guard<std::mutex> lock(window_->mutex);
    EXPECT_EQ(window_->clear_calls, 1);
    EXPECT_EQ(window_->close_calls, 0);
  }
  {
    std::lock_guard<std::mutex> lock(persisted_->mutex);
    EXPECT_EQ(persisted_->calls, 0);
  }
  EXPECT_FALSE(session->accepted());

  window_->Report(kSecondSession);
  (void)PollUntil(session.get(), BrowserSignInEventType::kAccepted);
  std::lock_guard<std::mutex> lock(persisted_->mutex);
  EXPECT_EQ(persisted_->value, kSecondSession);
}

TEST_F(BrowserSignInTest, UnverifiedSessionIsCheckedAgainLater) {
  http_.Queue({false, 0, {}, "offline"});
  http_.Queue({true, 200, kIdentityBody, {}});
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);

  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kAccepted);
  EXPECT_EQ(Types(events), (std::vector<BrowserSignInEventType>{
                               BrowserSignInEventType::kUnverified,
                               BrowserSignInEventType::kAccepted}));
  EXPECT_EQ(http_.request_count(), 2);
  std::lock_guard<std::mutex> lock(window_->mutex);
  EXPECT_EQ(window_->clear_calls, 0);
}

TEST_F(BrowserSignInTest, UnverifiedSessionWaitsForTheRecheckDelay) {
  http_.Queue({false, 0, {}, "offline"});
  auto session =
      MakeSession({std::chrono::milliseconds(5000), std::chrono::seconds(60)});
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);
  (void)PollUntil(session.get(), BrowserSignInEventType::kUnverified);
  for (int poll = 0; poll < 20; ++poll) {
    EXPECT_TRUE(session->Poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(http_.request_count(), 1);
}

TEST_F(BrowserSignInTest, StoreFailureClosesWithoutAccepting) {
  http_.Queue({true, 200, kIdentityBody, {}});
  persisted_->result = false;
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);

  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kFailed);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front().message,
            "could not save the browser sign-in session");
  EXPECT_FALSE(session->accepted());
  {
    std::lock_guard<std::mutex> lock(window_->mutex);
    EXPECT_EQ(window_->close_calls, 1);
  }
  window_->Exit();
  (void)PollUntil(session.get(), BrowserSignInEventType::kClosed);
  EXPECT_FALSE(session->accepted());
}

TEST_F(BrowserSignInTest, ANewerSessionReplacesTheOneBeingChecked) {
  http_.Hold();
  http_.Queue({true, 401, "{}", {}});
  http_.Queue({true, 200, kIdentityBody, {}});
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);
  (void)session->Poll();
  while (http_.request_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  window_->Report(kSecondSession);
  (void)session->Poll();
  http_.Release();

  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kAccepted);
  EXPECT_EQ(Types(events), (std::vector<BrowserSignInEventType>{
                               BrowserSignInEventType::kRejected,
                               BrowserSignInEventType::kAccepted}));
  {
    // The newer session is still to be checked, so WebKit is not cleared.
    std::lock_guard<std::mutex> lock(window_->mutex);
    EXPECT_EQ(window_->clear_calls, 0);
  }
  std::lock_guard<std::mutex> lock(persisted_->mutex);
  EXPECT_EQ(persisted_->value, kSecondSession);
}

TEST_F(BrowserSignInTest, ACheckRunningWhenTheWindowClosesStillCounts) {
  http_.Hold();
  http_.Queue({true, 200, kIdentityBody, {}});
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);
  (void)session->Poll();
  while (http_.request_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  window_->Exit();
  for (int poll = 0; poll < 5; ++poll) {
    EXPECT_TRUE(session->Poll().empty());
  }
  EXPECT_TRUE(session->active());
  http_.Release();

  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kClosed);
  EXPECT_EQ(Types(events), (std::vector<BrowserSignInEventType>{
                               BrowserSignInEventType::kAccepted,
                               BrowserSignInEventType::kClosed}));
  EXPECT_TRUE(session->accepted());
}

TEST_F(BrowserSignInTest, ClosingWithoutASessionPlaysAsGuest) {
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  session->Cancel();
  {
    std::lock_guard<std::mutex> lock(window_->mutex);
    EXPECT_EQ(window_->close_calls, 1);
  }
  window_->Exit();
  EXPECT_EQ(
      Types(PollUntil(session.get(), BrowserSignInEventType::kClosed)),
      std::vector<BrowserSignInEventType>{BrowserSignInEventType::kClosed});
  EXPECT_FALSE(session->accepted());
  EXPECT_EQ(http_.request_count(), 0);
}

TEST_F(BrowserSignInTest, AWindowThatNeverLoadsIsClosed) {
  window_->ready = false;
  auto session =
      MakeSession({std::chrono::milliseconds(10), std::chrono::seconds(5)});
  ASSERT_TRUE(
      session->Attach(std::make_unique<FakeWindow>(window_), "Sign in", false));
  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kClosed);
  EXPECT_EQ(Types(events), (std::vector<BrowserSignInEventType>{
                               BrowserSignInEventType::kFailed,
                               BrowserSignInEventType::kClosed}));
  std::lock_guard<std::mutex> lock(window_->mutex);
  EXPECT_EQ(window_->close_calls, 1);
  EXPECT_FALSE(window_->visible);
}

TEST_F(BrowserSignInTest, AWindowThatExitsWhileLoadingFails) {
  window_->ready = false;
  window_->exited = true;
  auto session = MakeSession();
  ASSERT_TRUE(
      session->Attach(std::make_unique<FakeWindow>(window_), "Sign in", false));
  EXPECT_EQ(Types(session->Poll()), (std::vector<BrowserSignInEventType>{
                                        BrowserSignInEventType::kFailed,
                                        BrowserSignInEventType::kClosed}));
  EXPECT_FALSE(session->active());
}

TEST_F(BrowserSignInTest, EventsAndErrorsNeverCarryTheSession) {
  http_.Queue({false, 0, {}, std::string("echo ") + kFirstSession});
  http_.Queue({true, 200, kIdentityBody, {}});
  persisted_->result = false;
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  window_->Report(kFirstSession);
  const std::vector<BrowserSignInEvent> events =
      PollUntil(session.get(), BrowserSignInEventType::kFailed);
  for (const BrowserSignInEvent& event : events) {
    EXPECT_EQ(event.message.find(kFirstSession), std::string::npos);
    EXPECT_EQ(event.identity.username.find(kFirstSession), std::string::npos);
  }
}

TEST_F(BrowserSignInTest, DestroyingAnOpenSessionClosesItsWindow) {
  http_.Hold();
  http_.Queue({true, 401, "{}", {}});
  {
    auto session = MakeSession();
    ASSERT_TRUE(Open(session.get()));
    window_->Report(kFirstSession);
    (void)session->Poll();
    while (http_.request_count() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::thread release([this]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      http_.Release();
    });
    session.reset();
    release.join();
  }
  std::lock_guard<std::mutex> lock(window_->mutex);
  EXPECT_EQ(window_->close_calls, 1);
}

TEST_F(BrowserSignInTest, AttachRefusesASecondWindow) {
  auto session = MakeSession();
  ASSERT_TRUE(Open(session.get()));
  EXPECT_FALSE(session->Attach(
      std::make_unique<FakeWindow>(std::make_shared<FakeWindowState>()), "",
      false));
  std::string error;
  EXPECT_FALSE(
      session->Start("/nonexistent", kBrowserSignInUrl, "", false, &error));
  EXPECT_FALSE(error.empty());
}

class ScopedEnvironmentVariable final {
 public:
  explicit ScopedEnvironmentVariable(const char* name) : name_(name) {
    const char* value = std::getenv(name);
    if (value != nullptr) {
      original_ = value;
    }
  }
  ~ScopedEnvironmentVariable() {
    if (original_.has_value()) {
      setenv(name_, original_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

 private:
  const char* name_;
  std::optional<std::string> original_;
};

TEST(BrowserSignInHelperTest, HelperOverrideMustExist) {
  const ScopedEnvironmentVariable restore("MOCKTAIL_WEBVIEW_HELPER");
  char pattern[] = "/tmp/mocktail_browser_sign_in_XXXXXX";
  const int descriptor = mkstemp(pattern);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  ASSERT_EQ(setenv("MOCKTAIL_WEBVIEW_HELPER", pattern, 1), 0);
  EXPECT_EQ(ResolveWebViewHelperPath(), std::filesystem::path(pattern));
  std::filesystem::remove(pattern);
  EXPECT_TRUE(ResolveWebViewHelperPath().empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
