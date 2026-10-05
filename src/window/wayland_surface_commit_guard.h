#ifndef MOCKTAIL_WINDOW_WAYLAND_SURFACE_COMMIT_GUARD_H_
#define MOCKTAIL_WINDOW_WAYLAND_SURFACE_COMMIT_GUARD_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

namespace mocktail {
namespace window {

// NVIDIA's Vulkan WSI (driver 555 and newer, on a compositor that offers
// wp_linux_drm_syncobj_manager_v1) presents on Wayland as separate requests
// from the presenting thread: set_acquire_point, set_release_point, then
// attach, damage and commit. A wl_surface.commit of the game surface from
// another thread in between applies the timeline points without a buffer;
// the compositor answers with a fatal wp_linux_drm_syncobj_surface_v1 error
// (no_buffer or no_release_point) and closes the display connection, so every
// later present fails with VK_ERROR_SURFACE_LOST_KHR.
//
// SDL commits the game surface from the main thread when it changes
// fullscreen, maximizes, shows or hides the window, warps the pointer on a
// compositor without wp_pointer_warp_v1, and for some events it dispatches.
// Roblox presents from its own render thread. This guard serializes the two:
// the host present and swapchain creation hold it on the presenting thread,
// and those SDL calls hold it on the main thread.
//
// The main thread never blocks behind a stalled present: it waits at most
// `budget` from the moment the current present began, then the caller
// defers its request (fullscreen) or goes ahead unguarded (event pumping, so
// input keeps flowing). The presenting thread waits at most its own timeout
// for the main thread, so a call that unexpectedly waits on the render thread
// can slow frames but never deadlock them.
class SurfaceCommitGuard final {
 public:
  using Clock = std::chrono::steady_clock;

  SurfaceCommitGuard() = default;
  SurfaceCommitGuard(const SurfaceCommitGuard&) = delete;
  SurfaceCommitGuard& operator=(const SurfaceCommitGuard&) = delete;

  // On only while the game window presents through a Wayland WSI. A holder
  // that entered while it was on still leaves normally.
  void SetActive(bool active);
  bool active() const { return active_.load(std::memory_order_acquire); }

  // Presenting thread, around vkQueuePresentKHR and swapchain creation or
  // destruction. True when the guard is now held and LeaveHostWsi() must
  // follow; false when it is off or `timeout` passed while the main thread
  // held it (the call then goes ahead unguarded).
  bool EnterHostWsi(std::chrono::milliseconds timeout);
  void LeaveHostWsi();

  // Main thread, around an SDL call that may commit the game surface. True
  // when the guard is now held and LeaveCommit() must follow. False when it
  // is off (`*may_commit` true) or when a host call has held it for `budget`
  // (`*may_commit` false).
  bool EnterCommit(std::chrono::milliseconds budget, bool* may_commit);
  void LeaveCommit();

  // Host calls that went ahead unguarded after their timeout.
  std::uint64_t unguarded_host_calls() const {
    return unguarded_host_calls_.load(std::memory_order_relaxed);
  }

 private:
  std::timed_mutex mutex_;
  std::atomic<bool> active_{false};
  // Clock time at which the current host call took the guard; 0 while the
  // main thread holds it or nobody does.
  std::atomic<std::int64_t> host_since_ns_{0};
  std::atomic<std::uint64_t> unguarded_host_calls_{0};
};

// How long a main-thread surface change waits for an ongoing present.
inline constexpr std::chrono::milliseconds kSurfaceCommitBudget{100};
// How long a present waits for an ongoing main-thread surface change.
inline constexpr std::chrono::milliseconds kHostWsiGuardTimeout{250};
// At most one log line per this interval for each side of the guard: a
// present stalled on a hidden window keeps every tick waiting past the
// budget, many times a second.
inline constexpr std::chrono::seconds kSurfaceCommitReportInterval{5};

// Lets a recurring event through to the log at most once per `interval`,
// and counts the events in between, so the line it lets through can say
// how many there were. Thread-safe.
class LogRateLimiter final {
 public:
  using Clock = std::chrono::steady_clock;

  struct Decision {
    // Log this event.
    bool log = false;
    // Events noted so far, this one included.
    std::uint64_t total = 0;
    // When `log`: events since the previous line, this one included.
    std::uint64_t since_last_report = 0;
  };

  explicit LogRateLimiter(std::chrono::nanoseconds interval)
      : interval_ns_(interval.count()) {}
  LogRateLimiter(const LogRateLimiter&) = delete;
  LogRateLimiter& operator=(const LogRateLimiter&) = delete;

  // Notes one event at `now`. The first event is always logged.
  Decision Note(Clock::time_point now);

 private:
  const std::int64_t interval_ns_;
  std::atomic<std::uint64_t> total_{0};
  std::atomic<std::uint64_t> reported_total_{0};
  // Clock time from which the next line may be logged; the lowest value
  // before the first line, so the first event is always due.
  std::atomic<std::int64_t> next_report_ns_{
      std::numeric_limits<std::int64_t>::min()};
};

// The guard for the game window's surface.
SurfaceCommitGuard& GameSurfaceCommitGuard();

// MOCKTAIL_WAYLAND_COMMIT_GUARD=0 turns the guard off (for A/B checks); the
// automatic display server then keeps NVIDIA's direct Vulkan on XWayland.
bool SurfaceCommitGuardAllowed(const char* value);
bool SurfaceCommitGuardAllowed();

// Holds the guard around one main-thread SDL call. `operation` names it in
// the log when a present kept it busy past the budget.
class ScopedSurfaceCommit final {
 public:
  explicit ScopedSurfaceCommit(
      const char* operation, SurfaceCommitGuard* guard = nullptr,
      std::chrono::milliseconds budget = kSurfaceCommitBudget);
  ~ScopedSurfaceCommit();
  ScopedSurfaceCommit(const ScopedSurfaceCommit&) = delete;
  ScopedSurfaceCommit& operator=(const ScopedSurfaceCommit&) = delete;

  // The SDL call may commit now: the guard is held, or it is off.
  bool ready() const { return ready_; }

 private:
  SurfaceCommitGuard* guard_ = nullptr;
  bool held_ = false;
  bool ready_ = true;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WAYLAND_SURFACE_COMMIT_GUARD_H_
