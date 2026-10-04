#ifndef MOCKTAIL_RUNTIME_LAUNCHER_POLICY_H_
#define MOCKTAIL_RUNTIME_LAUNCHER_POLICY_H_

#include <filesystem>
#include <string_view>

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

// What decides whether the settings window opens before Roblox starts.
struct LauncherInputs {
  // CommandMode::kRun.
  bool run_mode = false;
  // A roblox: or roblox-player: link, or --launch-uri: a website join.
  bool external_launch_request = false;
  // MOCKTAIL_ISOLATED_CANARY=1: an updater readiness canary.
  bool isolated_canary = false;
  // MOCKTAIL_UNSAFE_LATEST=1: the game run that --force-run-latest starts.
  bool unsafe_latest = false;
  // --headless, or MOCKTAIL_HEADLESS enabled.
  bool headless = false;
  // DISPLAY or WAYLAND_DISPLAY is set.
  bool display_available = false;
  // --launcher.
  bool cli_force_show = false;
  // --play or --no-launcher.
  bool cli_skip = false;
  // launcher.show_on_start, from ReadLauncherShowOnStart().
  bool show_on_start = true;
  // The mocktail_launcher_ui helper was found.
  bool helper_present = false;
};

enum class LauncherDecision {
  kShow,
  kSkip,
};

// The window never opens for website joins, canaries, the unsafe-latest
// run, headless runs, without a display, or without its helper. Otherwise
// --play skips it, --launcher shows it, and launcher.show_on_start decides.
LauncherDecision DecideLauncher(const LauncherInputs& inputs);

// The rule DecideLauncher applied, for the session log ("website join",
// "--play", "launcher.show_on_start", ...).
std::string_view DescribeLauncherDecision(const LauncherInputs& inputs);

// Fills the inputs that come from the environment: isolated_canary,
// unsafe_latest, headless and display_available. The command line has
// already been applied to the environment by then, so --headless shows up
// as MOCKTAIL_HEADLESS=1.
void ReadLauncherEnvironment(const Environment& environment,
                             LauncherInputs* inputs);

// launcher.show_on_start without failing. MOCKTAIL_LAUNCHER_SHOW_ON_START
// wins, as it does in the config loader (empty means the default, true).
// Otherwise the file is loaded on its own, ignoring every other variable,
// and anything that keeps it from loading (a YAML error, an invalid value
// anywhere, a symlink) gives true, so a broken config still opens the window
// where it can be fixed. A missing file gives the default, true.
bool ReadLauncherShowOnStart(const std::filesystem::path& config_file,
                             const Environment& environment);
bool ReadLauncherShowOnStart(const std::filesystem::path& config_file);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_LAUNCHER_POLICY_H_
