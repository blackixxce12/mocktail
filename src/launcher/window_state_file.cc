#include "launcher/window_state_file.h"

#include "window/window_state_store.h"

namespace mocktail::launcher {
namespace {

constexpr int kMaximumWindowExtent = 16384;

// LoadWindowState reports a file the game would ignore (bad JSON, schema,
// range, not a regular file) as kInvalidArgument, and a path it refuses to
// open (symlink, permissions) or cannot read as a platform error.
bool IgnoredByGame(const Status& status) {
  return status.code() == StatusCode::kInvalidArgument;
}

}  // namespace

bool ReadRememberedWindowState(const std::filesystem::path& path,
                               RememberedWindowState* state,
                               std::string* error) {
  *state = RememberedWindowState();
  if (!path.is_absolute()) {
    *error = "window state path must be absolute";
    return false;
  }
  const window::WindowStateLoadResult loaded = window::LoadWindowState(path);
  if (!loaded) {
    if (IgnoredByGame(loaded.status)) {
      return true;
    }
    *error = loaded.status.message() + ": " + path.string();
    return false;
  }
  if (!loaded.found) {
    return true;
  }
  state->found = true;
  state->width = loaded.state.width;
  state->height = loaded.state.height;
  state->fullscreen = loaded.state.fullscreen;
  state->maximized = loaded.state.maximized;
  return true;
}

bool UpdateWindowedSize(const std::filesystem::path& path, int width,
                        int height, std::string* error,
                        WindowedSizeUpdate* outcome) {
  WindowedSizeUpdate ignored = WindowedSizeUpdate::kNoStateFile;
  if (outcome == nullptr) {
    outcome = &ignored;
  }
  *outcome = WindowedSizeUpdate::kNoStateFile;
  if (!path.is_absolute()) {
    *error = "window state path must be absolute";
    return false;
  }
  if (width < window::kMinimumWindowWidth || width > kMaximumWindowExtent ||
      height < window::kMinimumWindowHeight ||
      height > kMaximumWindowExtent) {
    *error = "window size must be between " +
             std::to_string(window::kMinimumWindowWidth) + "x" +
             std::to_string(window::kMinimumWindowHeight) + " and " +
             std::to_string(kMaximumWindowExtent) + "x" +
             std::to_string(kMaximumWindowExtent);
    return false;
  }
  const window::WindowStateLoadResult loaded = window::LoadWindowState(path);
  if (!loaded) {
    if (IgnoredByGame(loaded.status)) {
      *outcome = WindowedSizeUpdate::kInvalidStateFile;
      return true;
    }
    *error = loaded.status.message() + ": " + path.string();
    return false;
  }
  if (!loaded.found) {
    return true;
  }
  if (loaded.state.width == width && loaded.state.height == height) {
    *outcome = WindowedSizeUpdate::kUnchanged;
    return true;
  }
  window::PersistedWindowState state = loaded.state;
  state.width = width;
  state.height = height;
  const Status stored = window::StoreWindowState(path, state);
  if (!stored.ok()) {
    *error = stored.message() + ": " + path.string();
    return false;
  }
  *outcome = WindowedSizeUpdate::kUpdated;
  return true;
}

}  // namespace mocktail::launcher
