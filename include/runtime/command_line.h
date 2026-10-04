#ifndef MOCKTAIL_RUNTIME_COMMAND_LINE_H_
#define MOCKTAIL_RUNTIME_COMMAND_LINE_H_

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

enum class CommandMode {
  kRun,
  kHelp,
};

enum class WindowMode {
  kUnspecified,
  kHeadless,
  kWindowed,
};

// Whether the settings window should open before Roblox starts on this run.
// kDefault leaves it to launcher.show_on_start.
enum class LauncherRequest {
  kDefault,
  // --launcher
  kShow,
  // --play or --no-launcher
  kSkip,
};

struct CommandLineOptions {
  CommandMode mode = CommandMode::kRun;
  WindowMode window_mode = WindowMode::kUnspecified;
  std::string program_name = "mocktail";
  std::string roblox_library_path;
  std::string graphics_backend;
  // Raw browser/internal input exists only until the composition root creates
  // safe re-exec arguments, then it is overwritten in this string and argv.
  std::string raw_launch_argument;
  std::string launch_request_json;
  int launch_argument_index = -1;
  bool allow_unverified_build = false;
  // Requests one explicit launch of the provider latest without promoting it
  // into the managed payload state.
  bool force_run_latest = false;
  LauncherRequest launcher = LauncherRequest::kDefault;
};

struct CommandLineParseResult {
  CommandLineOptions options;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

CommandLineParseResult ParseCommandLine(int argc, const char* const argv[]);
// Produces argv[1..] for a possible cgroup re-exec. A raw website URI is
// replaced with the already-normalized internal request, so browser gameinfo
// tickets do not cross the exec boundary.
bool BuildCommandLineReexecArguments(const CommandLineOptions& options,
                                     int argc, const char* const argv[],
                                     std::vector<std::string>* arguments,
                                     std::string* error);
// Overwrites the original raw launch argument and both owned launch strings
// after the request and sanitized re-exec argv have been created.
void ScrubCommandLineLaunchArguments(CommandLineOptions* options, int argc,
                                     char* argv[]);
bool ApplyCommandLineEnvironment(const CommandLineOptions& options,
                                 std::string* error);
// The variables ApplyCommandLineEnvironment sets for `options`. When the
// settings window asks main to ignore the user's environment for a launch,
// these still hold the command line's values and must stay.
std::vector<std::string> CommandLineEnvironmentNames(
    const CommandLineOptions& options);
std::string CommandLineUsage(const std::string& program_name);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_COMMAND_LINE_H_
