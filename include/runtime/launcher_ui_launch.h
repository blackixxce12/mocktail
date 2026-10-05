#ifndef MOCKTAIL_RUNTIME_LAUNCHER_UI_LAUNCH_H_
#define MOCKTAIL_RUNTIME_LAUNCHER_UI_LAUNCH_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"

// The settings window (mocktail_launcher_ui) is a GTK helper that mocktail
// runs before the game starts, while it holds the single-instance lock and
// before the first config load. GTK never enters the game process.
//
// Protocol: the helper inherits the write end of a pipe as file descriptor
// MOCKTAIL_LAUNCHER_RESULT_FD (3) and writes exactly one line before it
// exits: "play", "play ignore-env" or "quit". Everything the user changed
// (config.yaml, window-state.json, the account selection) is already saved
// when it reports "play". A helper that crashes, exits without a line or
// writes anything else counts as "play": a broken settings window must
// never keep the game from starting.
namespace mocktail {
namespace runtime {

inline constexpr std::string_view kLauncherUiHelperName =
    "mocktail_launcher_ui";
inline constexpr std::string_view kLauncherUiHelperVariable =
    "MOCKTAIL_LAUNCHER_UI_HELPER";
inline constexpr std::string_view kLauncherUiResultFdVariable =
    "MOCKTAIL_LAUNCHER_RESULT_FD";
inline constexpr int kLauncherUiResultDescriptor = 3;
// config.yaml the window edits (the path mocktail loads next).
inline constexpr std::string_view kLauncherUiConfigFileVariable =
    "MOCKTAIL_CONFIG_FILE";
// Comma-separated names of the managed variables the user's own environment
// set (CaptureUserManagedEnvironment); the window reads their values from
// the environment it inherits.
inline constexpr std::string_view kLauncherUiEnvOverridesVariable =
    "MOCKTAIL_LAUNCHER_ENV_OVERRIDES";
// "1" when mocktail created config.yaml on this start.
inline constexpr std::string_view kLauncherUiConfigCreatedVariable =
    "MOCKTAIL_LAUNCHER_CONFIG_CREATED";

enum class LauncherUiResult {
  kPlay,
  // Play, and leave out the user's managed variables for this launch: the
  // window moved their values into the settings.
  kPlayIgnoringEnvironment,
  kQuit,
};

// The line for a result, without the trailing newline.
std::string_view LauncherUiResultLine(LauncherUiResult result);

// Accepts exactly one of the three lines, with or without one trailing
// newline.
std::optional<LauncherUiResult> ParseLauncherUiResultLine(
    std::string_view line);

// MOCKTAIL_LAUNCHER_UI_HELPER when set (it must then be an absolute path to
// an executable regular file, else nothing is found), otherwise
// mocktail_launcher_ui next to the running executable, in
// ../<libdir>/mocktail or in ../libexec/mocktail. Empty when there is none.
std::filesystem::path ResolveLauncherUiHelperPath(
    const Environment& environment);

struct LauncherUiLaunchOptions {
  std::filesystem::path helper;
  std::filesystem::path config_file;
  // The user's managed variables, from CaptureUserManagedEnvironment().
  std::vector<std::string> user_managed_environment;
  bool config_created = false;
  // "NAME=value" entries mocktail started with (ProcessStartState).
  std::vector<std::string> original_environment;
};

// The helper's environment: `current_environment` (an environ-style array)
// without the game-only tuning mocktail itself added for the game process
// (the video driver and GPU variables that host_launch_environment lists,
// when the original environment did not already hold the same value), plus
// the MOCKTAIL_CONFIG_FILE, MOCKTAIL_LAUNCHER_ENV_OVERRIDES,
// MOCKTAIL_LAUNCHER_CONFIG_CREATED and MOCKTAIL_LAUNCHER_RESULT_FD entries.
// MOCKTAIL_* paths and the user's own variables stay: the window needs them
// to find helpers and to show what overrides a setting. Two never reach it:
// MOCKTAIL_LAUNCHER_DONE (launcher_policy.h), which only mocktail's own
// re-executions may inherit, and the session in a non-empty
// MOCKTAIL_ROBLOX_COOKIES, which becomes "1" (the window only checks that
// one is given).
std::vector<std::string> BuildLauncherUiEnvironment(
    const char* const* current_environment,
    const LauncherUiLaunchOptions& options);

struct LauncherUiRun {
  // The helper process started.
  bool started = false;
  // The line it wrote, if it was a valid one.
  std::optional<LauncherUiResult> result;
  // The exit status when it exited normally, else -1.
  int exit_status = -1;
  // The signal that ended it, else 0.
  int signal = 0;
  // Why it did not start, or what was wrong with its answer. Never
  // contains anything the user typed into the window.
  std::string error;

  // What mocktail does next: quit only when the window said so.
  LauncherUiResult decision() const {
    return result.value_or(LauncherUiResult::kPlay);
  }
};

// Runs the helper with `environment` ("NAME=value" entries) and the pipe as
// descriptor 3, and waits until it exits. Returns as soon as the helper has
// exited even when a process it started still holds the pipe.
LauncherUiRun RunLauncherUi(const std::filesystem::path& helper,
                            const std::vector<std::string>& environment);

// For "play ignore-env": unsets every name in `user_managed_environment`
// except those the command line set (`command_line_names`, from
// CommandLineEnvironmentNames: those carry the command line's values now),
// and removes the same names from `restart_environment` (the environment a
// restart after website sign-in starts from) when it is given. Returns the
// names removed.
std::vector<std::string> RemoveUserManagedEnvironment(
    const std::vector<std::string>& user_managed_environment,
    const std::vector<std::string>& command_line_names,
    std::vector<std::string>* restart_environment);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_LAUNCHER_UI_LAUNCH_H_
