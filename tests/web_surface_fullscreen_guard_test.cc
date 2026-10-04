#include "window/web_surface_fullscreen_guard.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

TEST(WebSurfaceFullscreenGuardTest, RestoresAfterTheLastSurfaceCloses) {
  WebSurfaceFullscreenGuard guard;
  guard.SurfaceOpened();
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  EXPECT_FALSE(guard.TakeRestore());

  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());
  guard.SurfaceClosed();
  EXPECT_TRUE(guard.TakeRestore());
  EXPECT_FALSE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, IgnoresChangesWithoutAnOpenSurface) {
  WebSurfaceFullscreenGuard guard;
  guard.NoteFullscreenChanged(false);
  guard.SurfaceOpened();
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());

  // A surface that closed before the compositor acted no longer counts.
  guard.SurfaceOpened();
  guard.SurfaceClosed();
  guard.NoteFullscreenChanged(false);
  EXPECT_FALSE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, NeverUndoesWhatTheGameAskedFor) {
  WebSurfaceFullscreenGuard guard;
  guard.SurfaceOpened();
  guard.NoteGameRequest(false);
  guard.NoteFullscreenChanged(false);
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());

  // The user picks windowed after the compositor already left fullscreen.
  guard.NoteFullscreenChanged(true);
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  guard.NoteGameRequest(false);
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());

  // The game went back to fullscreen by itself meanwhile.
  guard.NoteFullscreenChanged(true);
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  guard.NoteGameRequest(true);
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, ForgetsAChangeTheCompositorUndid) {
  WebSurfaceFullscreenGuard guard;
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  guard.NoteFullscreenChanged(true);
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, FullscreenFromOutsideEndsAWindowedChoice) {
  WebSurfaceFullscreenGuard guard;
  guard.NoteGameRequest(false);
  // The user made the window fullscreen through the compositor.
  guard.NoteFullscreenChanged(true);
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  guard.SurfaceClosed();
  EXPECT_TRUE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, ExtraClosesDoNotHideLaterSurfaces) {
  WebSurfaceFullscreenGuard guard;
  guard.SurfaceClosed();
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  EXPECT_FALSE(guard.TakeRestore());
  guard.SurfaceClosed();
  EXPECT_TRUE(guard.TakeRestore());
}

TEST(WebSurfaceFullscreenGuardTest, ANewWindowStartsWithoutHistory) {
  WebSurfaceFullscreenGuard guard;
  guard.SurfaceOpened();
  guard.NoteFullscreenChanged(false);
  guard.ResetWindow();
  guard.SurfaceClosed();
  EXPECT_FALSE(guard.TakeRestore());

  // Surfaces still open across a new window keep counting.
  guard.NoteGameRequest(false);
  guard.SurfaceOpened();
  guard.ResetWindow();
  guard.NoteFullscreenChanged(false);
  EXPECT_FALSE(guard.TakeRestore());
  guard.SurfaceClosed();
  EXPECT_TRUE(guard.TakeRestore());
}

}  // namespace
}  // namespace window
}  // namespace mocktail
