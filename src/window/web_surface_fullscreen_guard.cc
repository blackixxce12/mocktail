#include "window/web_surface_fullscreen_guard.h"

namespace mocktail {
namespace window {

void WebSurfaceFullscreenGuard::SurfaceOpened() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++open_surfaces_;
}

void WebSurfaceFullscreenGuard::SurfaceClosed() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_surfaces_ > 0) {
    --open_surfaces_;
  }
}

void WebSurfaceFullscreenGuard::NoteGameRequest(bool fullscreen) {
  std::lock_guard<std::mutex> lock(mutex_);
  game_requested_windowed_ = !fullscreen;
  restore_pending_ = false;
}

void WebSurfaceFullscreenGuard::NoteFullscreenChanged(bool fullscreen) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fullscreen) {
    game_requested_windowed_ = false;
    restore_pending_ = false;
    return;
  }
  if (open_surfaces_ > 0 && !game_requested_windowed_) {
    restore_pending_ = true;
  }
}

bool WebSurfaceFullscreenGuard::TakeRestore() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!restore_pending_ || open_surfaces_ > 0) {
    return false;
  }
  restore_pending_ = false;
  return true;
}

void WebSurfaceFullscreenGuard::ResetWindow() {
  std::lock_guard<std::mutex> lock(mutex_);
  game_requested_windowed_ = false;
  restore_pending_ = false;
}

}  // namespace window
}  // namespace mocktail
