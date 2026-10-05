#include "window/wayland_surface_commit_guard.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "window/window.h"
#include "window/window_fullscreen_request_gate.h"

extern "C" bool mocktail_window_host_wsi_enter();
extern "C" void mocktail_window_host_wsi_leave();

namespace mocktail {
namespace window {
namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

milliseconds Since(Clock::time_point start) {
  return std::chrono::duration_cast<milliseconds>(Clock::now() - start);
}

// Holds the guard on its own thread, as the render thread does around a
// present, until Release().
class HostWsiHolder final {
 public:
  explicit HostWsiHolder(SurfaceCommitGuard* guard) {
    std::promise<bool> entered;
    std::future<bool> entered_future = entered.get_future();
    thread_ = std::thread([this, guard, entered = std::move(entered)]() mutable {
      const bool held = guard != nullptr
                            ? guard->EnterHostWsi(milliseconds(1000))
                            : mocktail_window_host_wsi_enter();
      entered.set_value(held);
      release_.get_future().wait();
      if (held) {
        if (guard != nullptr) {
          guard->LeaveHostWsi();
        } else {
          mocktail_window_host_wsi_leave();
        }
      }
    });
    held_ = entered_future.get();
  }
  ~HostWsiHolder() { Release(); }

  void Release() {
    if (thread_.joinable()) {
      release_.set_value();
      thread_.join();
    }
  }
  bool held() const { return held_; }

 private:
  std::promise<void> release_;
  std::thread thread_;
  bool held_ = false;
};

TEST(SurfaceCommitGuardTest, InactiveGuardNeverHoldsAnything) {
  SurfaceCommitGuard guard;
  bool may_commit = false;
  EXPECT_FALSE(guard.EnterCommit(milliseconds(10), &may_commit));
  EXPECT_TRUE(may_commit);
  EXPECT_FALSE(guard.EnterHostWsi(milliseconds(10)));
  EXPECT_EQ(guard.unguarded_host_calls(), 0U);

  ScopedSurfaceCommit commit("test", &guard);
  EXPECT_TRUE(commit.ready());
}

TEST(SurfaceCommitGuardTest, MainThreadTakesAnIdleGuard) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  bool may_commit = false;
  ASSERT_TRUE(guard.EnterCommit(milliseconds(10), &may_commit));
  EXPECT_TRUE(may_commit);
  // A present now waits for the main thread, and gives up after its timeout.
  std::thread present([&guard] {
    EXPECT_FALSE(guard.EnterHostWsi(milliseconds(20)));
  });
  present.join();
  EXPECT_EQ(guard.unguarded_host_calls(), 1U);
  guard.LeaveCommit();

  HostWsiHolder holder(&guard);
  EXPECT_TRUE(holder.held());
}

TEST(SurfaceCommitGuardTest, CommitWaitsForAShortPresent) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  std::atomic<bool> entered{false};
  std::thread present([&] {
    ASSERT_TRUE(guard.EnterHostWsi(milliseconds(1000)));
    entered.store(true);
    std::this_thread::sleep_for(milliseconds(30));
    guard.LeaveHostWsi();
  });
  while (!entered.load()) {
    std::this_thread::yield();
  }
  const Clock::time_point start = Clock::now();
  bool may_commit = false;
  const bool held = guard.EnterCommit(milliseconds(2000), &may_commit);
  const milliseconds waited = Since(start);
  present.join();
  ASSERT_TRUE(held);
  EXPECT_TRUE(may_commit);
  EXPECT_GE(waited.count(), 10);
  EXPECT_LT(waited.count(), 1000);
  guard.LeaveCommit();
}

TEST(SurfaceCommitGuardTest, StalledPresentDefersTheCommitAfterTheBudget) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  HostWsiHolder holder(&guard);
  ASSERT_TRUE(holder.held());

  Clock::time_point start = Clock::now();
  bool may_commit = true;
  EXPECT_FALSE(guard.EnterCommit(milliseconds(50), &may_commit));
  EXPECT_FALSE(may_commit);
  EXPECT_GE(Since(start).count(), 25);

  // The budget runs from the start of the present: once it is spent, later
  // ticks do not wait again.
  start = Clock::now();
  EXPECT_FALSE(guard.EnterCommit(milliseconds(50), &may_commit));
  EXPECT_FALSE(may_commit);
  EXPECT_LT(Since(start).count(), 25);

  {
    ScopedSurfaceCommit commit("fullscreen change deferred", &guard,
                               milliseconds(50));
    EXPECT_FALSE(commit.ready());
  }

  holder.Release();
  ScopedSurfaceCommit commit("fullscreen change", &guard, milliseconds(50));
  EXPECT_TRUE(commit.ready());
}

TEST(SurfaceCommitGuardTest, NestedEntryOnOneThreadDoesNotRelock) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  bool may_commit = false;
  ASSERT_TRUE(guard.EnterCommit(milliseconds(10), &may_commit));
  {
    ScopedSurfaceCommit nested("nested", &guard, milliseconds(10));
    EXPECT_TRUE(nested.ready());
  }
  EXPECT_FALSE(guard.EnterHostWsi(milliseconds(10)));
  EXPECT_EQ(guard.unguarded_host_calls(), 0U);
  guard.LeaveCommit();

  // Released for real: another thread takes it at once.
  HostWsiHolder holder(&guard);
  EXPECT_TRUE(holder.held());
}

TEST(SurfaceCommitGuardTest, TurningOffKeepsCurrentHoldersBalanced) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  bool may_commit = false;
  ASSERT_TRUE(guard.EnterCommit(milliseconds(10), &may_commit));
  guard.SetActive(false);
  guard.LeaveCommit();
  EXPECT_FALSE(guard.EnterCommit(milliseconds(10), &may_commit));
  EXPECT_TRUE(may_commit);

  guard.SetActive(true);
  HostWsiHolder holder(&guard);
  EXPECT_TRUE(holder.held());
}

TEST(SurfaceCommitGuardTest, EnvironmentSwitchTurnsTheGuardOffOnlyWhenAsked) {
  EXPECT_TRUE(SurfaceCommitGuardAllowed(nullptr));
  EXPECT_TRUE(SurfaceCommitGuardAllowed(""));
  EXPECT_TRUE(SurfaceCommitGuardAllowed("1"));
  EXPECT_TRUE(SurfaceCommitGuardAllowed("on"));
  for (const char* off : {"0", "false", "off", "no"}) {
    EXPECT_FALSE(SurfaceCommitGuardAllowed(off)) << off;
  }
}

TEST(LogRateLimiterTest, LetsOneLineThroughPerIntervalAndCounts) {
  using std::chrono::seconds;
  LogRateLimiter limiter(seconds(5));
  const LogRateLimiter::Clock::time_point start{seconds(1000)};

  LogRateLimiter::Decision decision = limiter.Note(start);
  EXPECT_TRUE(decision.log);
  EXPECT_EQ(decision.total, 1U);
  EXPECT_EQ(decision.since_last_report, 1U);

  // A stalled present misses the budget on every tick.
  for (int tick = 1; tick <= 300; ++tick) {
    decision = limiter.Note(start + milliseconds(16 * tick));
    EXPECT_FALSE(decision.log) << tick;
  }
  EXPECT_EQ(decision.total, 301U);

  decision = limiter.Note(start + seconds(5));
  EXPECT_TRUE(decision.log);
  EXPECT_EQ(decision.total, 302U);
  EXPECT_EQ(decision.since_last_report, 301U);

  // The next interval runs from that line, not from the first one.
  EXPECT_FALSE(limiter.Note(start + seconds(9)).log);
  decision = limiter.Note(start + seconds(10));
  EXPECT_TRUE(decision.log);
  EXPECT_EQ(decision.since_last_report, 2U);
}

TEST(LogRateLimiterTest, OneOfManyThreadsLogsADueLine) {
  LogRateLimiter limiter(std::chrono::seconds(5));
  const LogRateLimiter::Clock::time_point now = LogRateLimiter::Clock::now();
  std::atomic<int> logged{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&] {
      for (int note = 0; note < 1000; ++note) {
        if (limiter.Note(now).log) logged.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(logged.load(), 1);
  EXPECT_EQ(limiter.Note(now).total, 8001U);
}

// How many lines of `log` contain `text`.
int CountLines(const std::string& log, const std::string& text) {
  int count = 0;
  for (std::size_t at = log.find(text); at != std::string::npos;
       at = log.find(text, at + text.size())) {
    ++count;
  }
  return count;
}

// A present stalled on a hidden window makes every main-thread surface
// change miss its budget, many times a second: one line in the log, not
// one per tick.
TEST(SurfaceCommitGuardTest, ReportsAStalledPresentAtMostOncePerInterval) {
  SurfaceCommitGuard guard;
  guard.SetActive(true);
  HostWsiHolder holder(&guard);
  ASSERT_TRUE(holder.held());
  testing::internal::CaptureStderr();
  for (int tick = 0; tick < 20; ++tick) {
    ScopedSurfaceCommit commit("event pump went ahead", &guard,
                               milliseconds(1));
    EXPECT_FALSE(commit.ready());
  }
  const std::string log = testing::internal::GetCapturedStderr();
  // The first line is due unless another test of this process logged one
  // within the interval.
  EXPECT_LE(CountLines(log, "a host present held the game surface"), 1)
      << log;
}

// The other side: the render thread waits out its timeout behind a
// main-thread surface change.
TEST(SurfaceCommitGuardTest, ReportsUnguardedHostCallsAtMostOncePerInterval) {
  SurfaceCommitGuard& guard = GameSurfaceCommitGuard();
  guard.SetActive(true);
  bool may_commit = false;
  ASSERT_TRUE(guard.EnterCommit(milliseconds(10), &may_commit));
  testing::internal::CaptureStderr();
  std::thread present([] {
    // kHostWsiGuardTimeout (250 ms) each.
    for (int call = 0; call < 3; ++call) {
      EXPECT_FALSE(mocktail_window_host_wsi_enter());
    }
  });
  present.join();
  const std::string log = testing::internal::GetCapturedStderr();
  guard.LeaveCommit();
  guard.SetActive(false);
  EXPECT_LE(CountLines(log, "host WSI call went ahead"), 1) << log;
}

// The game window defers a fullscreen change while a present holds the
// surface past the budget, and makes it on a later tick.
class FullscreenDeferralTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const char* name :
         {"SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER", "MOCKTAIL_GRAPHICS_BACKEND",
          "MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK",
          "MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS"}) {
      const char* value = std::getenv(name);
      environment_.emplace_back(name, value != nullptr
                                          ? std::optional<std::string>(value)
                                          : std::nullopt);
    }
    ASSERT_EQ(unsetenv("SDL_VIDEODRIVER"), 0);
    ASSERT_EQ(unsetenv("MOCKTAIL_GRAPHICS_BACKEND"), 0);
    ASSERT_EQ(setenv("SDL_VIDEO_DRIVER", "dummy", 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);
  }

  void TearDown() override {
    Shutdown();
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    for (const auto& [name, value] : environment_) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

  static bool WindowIsFullscreen() {
    SDL_Window* window = static_cast<SDL_Window*>(GetBackendWindow());
    return window != nullptr &&
           (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> environment_;
};

TEST_F(FullscreenDeferralTest, ReplaysTheRequestOnceThePresentIsDone) {
  bool has_dummy_driver = false;
  for (int index = 0; index < SDL_GetNumVideoDrivers(); ++index) {
    const char* driver = SDL_GetVideoDriver(index);
    has_dummy_driver = has_dummy_driver ||
                       (driver != nullptr && std::strcmp(driver, "dummy") == 0);
  }
  if (!has_dummy_driver) {
    GTEST_SKIP() << "linked SDL does not provide its non-display dummy driver";
  }
  ASSERT_TRUE(Init(320, 180, "surface commit guard test")) << SDL_GetError();
  if (GetBackendWindow() == nullptr) {
    GTEST_SKIP() << "no SDL window behind the test graphics stubs";
  }
  // The dummy driver is not Wayland, so the window left the guard off.
  ASSERT_FALSE(GameSurfaceCommitGuard().active());
  GameSurfaceCommitGuard().SetActive(true);
  ASSERT_FALSE(WindowIsFullscreen());

  {
    HostWsiHolder present(nullptr);
    ASSERT_TRUE(present.held());
    ASSERT_TRUE(RequestFullscreenFromAndroidWindowFlags(
        static_cast<int>(kAndroidWindowFlagFullscreen),
        static_cast<int>(kAndroidWindowFlagFullscreen)));
    const Clock::time_point start = Clock::now();
    EXPECT_TRUE(PumpEvents());
    // One budget, not one per surface change in the tick.
    EXPECT_LT(Since(start).count(), 1000);
    EXPECT_FALSE(WindowIsFullscreen());
    EXPECT_TRUE(PumpEvents());
    EXPECT_FALSE(WindowIsFullscreen());
  }

  EXPECT_TRUE(PumpEvents());
  EXPECT_TRUE(WindowIsFullscreen());
  GameSurfaceCommitGuard().SetActive(false);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
