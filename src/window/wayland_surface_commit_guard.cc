#include "window/wayland_surface_commit_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace mocktail {
namespace window {

namespace {

// The guard this thread holds, so a nested Enter never relocks the
// non-recursive mutex.
thread_local const SurfaceCommitGuard* t_held_guard = nullptr;

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             SurfaceCommitGuard::Clock::now().time_since_epoch())
      .count();
}

std::atomic<std::uint64_t> g_busy_reports{0};

void ReportBusy(const char* operation, std::chrono::milliseconds budget) {
  const std::uint64_t count =
      g_busy_reports.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 8 && count % 256 != 0) {
    return;
  }
  std::fprintf(stderr,
               "  [window] surface commit guard: a host present held the "
               "game surface over %lld ms (%s; %llu so far)\n",
               static_cast<long long>(budget.count()),
               operation != nullptr ? operation : "surface change",
               static_cast<unsigned long long>(count));
}

}  // namespace

void SurfaceCommitGuard::SetActive(bool active) {
  active_.store(active, std::memory_order_release);
}

bool SurfaceCommitGuard::EnterHostWsi(std::chrono::milliseconds timeout) {
  if (!active() || t_held_guard == this) {
    return false;
  }
  if (!mutex_.try_lock_for(timeout)) {
    unguarded_host_calls_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  t_held_guard = this;
  host_since_ns_.store(NowNs(), std::memory_order_release);
  return true;
}

void SurfaceCommitGuard::LeaveHostWsi() {
  host_since_ns_.store(0, std::memory_order_release);
  t_held_guard = nullptr;
  mutex_.unlock();
}

bool SurfaceCommitGuard::EnterCommit(std::chrono::milliseconds budget,
                                     bool* may_commit) {
  *may_commit = true;
  if (!active() || t_held_guard == this) {
    return false;
  }
  if (!mutex_.try_lock()) {
    // Wait for the present in progress, but never longer than `budget` after
    // it began: a present stalled on a hidden window must not stall input.
    std::int64_t since = host_since_ns_.load(std::memory_order_acquire);
    const std::int64_t now = NowNs();
    if (since == 0 || since > now) {
      since = now;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(budget) -
        std::chrono::nanoseconds(now - since);
    if (remaining <= std::chrono::nanoseconds::zero() ||
        !mutex_.try_lock_for(remaining)) {
      *may_commit = false;
      return false;
    }
  }
  t_held_guard = this;
  return true;
}

void SurfaceCommitGuard::LeaveCommit() {
  t_held_guard = nullptr;
  mutex_.unlock();
}

SurfaceCommitGuard& GameSurfaceCommitGuard() {
  static SurfaceCommitGuard guard;
  return guard;
}

bool SurfaceCommitGuardAllowed(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  for (const char* off : {"0", "false", "off", "no"}) {
    if (std::strcmp(value, off) == 0) {
      return false;
    }
  }
  return true;
}

bool SurfaceCommitGuardAllowed() {
  return SurfaceCommitGuardAllowed(
      std::getenv("MOCKTAIL_WAYLAND_COMMIT_GUARD"));
}

ScopedSurfaceCommit::ScopedSurfaceCommit(const char* operation,
                                         SurfaceCommitGuard* guard,
                                         std::chrono::milliseconds budget)
    : guard_(guard != nullptr ? guard : &GameSurfaceCommitGuard()) {
  held_ = guard_->EnterCommit(budget, &ready_);
  if (!ready_) {
    ReportBusy(operation, budget);
  }
}

ScopedSurfaceCommit::~ScopedSurfaceCommit() {
  if (held_) {
    guard_->LeaveCommit();
  }
}

}  // namespace window
}  // namespace mocktail
