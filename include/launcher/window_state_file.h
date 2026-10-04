#ifndef MOCKTAIL_LAUNCHER_WINDOW_STATE_FILE_H_
#define MOCKTAIL_LAUNCHER_WINDOW_STATE_FILE_H_

#include <filesystem>
#include <string>

namespace mocktail::launcher {

// Once window-state.json exists (<state_root>/window-state.json), the game
// restores the windowed size from it and ignores window.width/height in
// config.yaml. A size chosen in the settings window must therefore be
// written to both files.

struct RememberedWindowState {
  bool found = false;  // false: no file, or one the game would ignore
  int width = 0;
  int height = 0;
  bool fullscreen = false;
  bool maximized = false;
};

// Reads the state the next game start would restore. A missing or invalid
// file yields found == false without an error, as the game ignores it too.
// Fails only for an unsafe or unreadable path (for example a symlink).
bool ReadRememberedWindowState(const std::filesystem::path& path,
                               RememberedWindowState* state,
                               std::string* error);

enum class WindowedSizeUpdate {
  kUpdated,
  kUnchanged,
  kNoStateFile,       // nothing to update; config.yaml's size applies
  kInvalidStateFile,  // left alone; the game ignores it and uses config.yaml
};

// Replaces only the remembered windowed width and height (logical units),
// keeping fullscreen, maximized and the position, through the game's own
// validating, atomic 0600 writer. Never creates the file. Fails for a size
// outside 160x120..16384x16384, a relative or unsafe path (symlink), or a
// failed write. Call it only while the game is not running: the game
// rewrites the file when it exits.
bool UpdateWindowedSize(const std::filesystem::path& path, int width,
                        int height, std::string* error,
                        WindowedSizeUpdate* outcome = nullptr);

}  // namespace mocktail::launcher

#endif  // MOCKTAIL_LAUNCHER_WINDOW_STATE_FILE_H_
