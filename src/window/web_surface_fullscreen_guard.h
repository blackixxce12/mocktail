#ifndef MOCKTAIL_WINDOW_WEB_SURFACE_FULLSCREEN_GUARD_H_
#define MOCKTAIL_WINDOW_WEB_SURFACE_FULLSCREEN_GUARD_H_

#include <mutex>

namespace mocktail {
namespace window {

// Web surfaces (sign-in, verification) open as children of the game window, so
// compositors float them over a fullscreen game. When the compositor cannot be
// told that (no xdg-foreign, a helper on another display, a refused handle,
// MOCKTAIL_WEBVIEW_PARENT=0), Hyprland tiles the surface beside the game and
// takes the game out of fullscreen to do so. This remembers such a change and
// asks for fullscreen again once the last web surface has closed. A change the
// game asked for itself (Roblox menu, settings, F11) is never undone.
//
// Surfaces open and close on any thread; the rest runs on SDL's main thread.
class WebSurfaceFullscreenGuard final {
 public:
  void SurfaceOpened();
  void SurfaceClosed();
  // The game itself asked for this fullscreen state.
  void NoteGameRequest(bool fullscreen);
  // SDL reports that the window entered or left fullscreen.
  void NoteFullscreenChanged(bool fullscreen);
  // True once, when the last surface has closed after the compositor took the
  // game out of fullscreen while one was open.
  bool TakeRestore();
  // A new game window: forgets the old one's fullscreen history. Surfaces
  // still open stay counted; they belong to helper processes, not the window.
  void ResetWindow();

 private:
  std::mutex mutex_;
  int open_surfaces_ = 0;
  bool game_requested_windowed_ = false;
  bool restore_pending_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WEB_SURFACE_FULLSCREEN_GUARD_H_
