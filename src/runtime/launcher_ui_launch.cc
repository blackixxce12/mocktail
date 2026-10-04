#include "runtime/launcher_ui_launch.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "runtime/host_launch_environment.h"

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kPlayLine = "play";
constexpr std::string_view kPlayIgnoringEnvironmentLine = "play ignore-env";
constexpr std::string_view kQuitLine = "quit";
// Longer than any valid line; anything past it is not a result.
constexpr std::size_t kMaximumLineBytes = 64;
// How often the wait checks whether the helper has exited while a process
// it started may still hold the pipe open.
constexpr int kExitPollMilliseconds = 200;

constexpr std::string_view kMocktailPrefix = "MOCKTAIL_";

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

std::string_view NameOf(std::string_view entry) {
  const std::size_t equals = entry.find('=');
  return equals == std::string_view::npos ? std::string_view()
                                          : entry.substr(0, equals);
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (!joined.empty()) joined += ',';
    joined += name;
  }
  return joined;
}

// Variables mocktail sets for the game process only (graphics driver
// choice, GPU tuning, loader state). The MOCKTAIL_* ones are kept: they
// hold paths the window needs and the user's own overrides.
bool IsGameOnlyVariable(std::string_view name) {
  return name.substr(0, kMocktailPrefix.size()) != kMocktailPrefix &&
         IsMocktailOnlyEnvironmentVariable(name, false);
}

void CloseDescriptor(int* descriptor) {
  if (*descriptor >= 0) {
    close(*descriptor);
    *descriptor = -1;
  }
}

// Reaps the child if it has exited; true once it has.
bool ReapIfExited(pid_t child, int* wait_status, bool block) {
  for (;;) {
    const pid_t reaped = waitpid(child, wait_status, block ? 0 : WNOHANG);
    if (reaped == child) return true;
    if (reaped < 0 && errno == EINTR) continue;
    return reaped < 0;  // ECHILD: nothing left to wait for
  }
}

}  // namespace

std::string_view LauncherUiResultLine(LauncherUiResult result) {
  switch (result) {
    case LauncherUiResult::kPlay:
      return kPlayLine;
    case LauncherUiResult::kPlayIgnoringEnvironment:
      return kPlayIgnoringEnvironmentLine;
    case LauncherUiResult::kQuit:
      return kQuitLine;
  }
  return kPlayLine;
}

std::optional<LauncherUiResult> ParseLauncherUiResultLine(
    std::string_view line) {
  if (!line.empty() && line.back() == '\n') {
    line.remove_suffix(1);
  }
  if (line == kPlayLine) return LauncherUiResult::kPlay;
  if (line == kPlayIgnoringEnvironmentLine) {
    return LauncherUiResult::kPlayIgnoringEnvironment;
  }
  if (line == kQuitLine) return LauncherUiResult::kQuit;
  return std::nullopt;
}

std::filesystem::path ResolveLauncherUiHelperPath(
    const Environment& environment) {
  const std::optional<std::string> helper_override =
      environment.Get(kLauncherUiHelperVariable);
  if (helper_override.has_value() && !helper_override->empty()) {
    const std::filesystem::path helper(*helper_override);
    return helper.is_absolute() && IsExecutableRegularFile(helper)
               ? helper
               : std::filesystem::path{};
  }
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path directory = executable.parent_path();
  const std::filesystem::path name{std::string(kLauncherUiHelperName)};
  for (const std::filesystem::path& candidate : {
           directory / name,
           directory.parent_path() / MOCKTAIL_INSTALL_LIBDIR / "mocktail" /
               name,
           directory.parent_path() / "libexec" / "mocktail" / name,
       }) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::vector<std::string> BuildLauncherUiEnvironment(
    const char* const* current_environment,
    const LauncherUiLaunchOptions& options) {
  std::unordered_map<std::string, std::string> original;
  for (const std::string& entry : options.original_environment) {
    const std::string_view name = NameOf(entry);
    if (!name.empty()) {
      original.emplace(std::string(name), entry);
    }
  }
  const std::string_view replaced[] = {
      kLauncherUiConfigFileVariable,
      kLauncherUiEnvOverridesVariable,
      kLauncherUiConfigCreatedVariable,
      kLauncherUiResultFdVariable,
  };
  std::vector<std::string> environment;
  for (const char* const* entry = current_environment;
       entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view text(*entry);
    const std::string_view name = NameOf(text);
    if (name.empty() ||
        std::find(std::begin(replaced), std::end(replaced), name) !=
            std::end(replaced)) {
      continue;
    }
    if (IsGameOnlyVariable(name)) {
      // Only what mocktail added or changed is left out; a value the user
      // started with is theirs (a pinned Vulkan driver, LD_LIBRARY_PATH of
      // a portable bundle).
      const auto found = original.find(std::string(name));
      if (found == original.end() || found->second != text) {
        continue;
      }
    }
    environment.emplace_back(text);
  }
  environment.push_back(std::string(kLauncherUiConfigFileVariable) + "=" +
                        options.config_file.string());
  environment.push_back(std::string(kLauncherUiEnvOverridesVariable) + "=" +
                        JoinNames(options.user_managed_environment));
  environment.push_back(std::string(kLauncherUiConfigCreatedVariable) + "=" +
                        (options.config_created ? "1" : "0"));
  environment.push_back(std::string(kLauncherUiResultFdVariable) + "=" +
                        std::to_string(kLauncherUiResultDescriptor));
  return environment;
}

LauncherUiRun RunLauncherUi(const std::filesystem::path& helper,
                            const std::vector<std::string>& environment) {
  LauncherUiRun run;
  int pipe_ends[2] = {-1, -1};
  if (pipe2(pipe_ends, O_CLOEXEC) != 0) {
    run.error = std::string("cannot create the result pipe: ") +
                std::strerror(errno);
    return run;
  }
  int read_end = pipe_ends[0];
  int write_end = pipe_ends[1];
  // dup2 onto the same number would keep close-on-exec, so the write end
  // must not already be the descriptor the helper expects.
  if (write_end == kLauncherUiResultDescriptor) {
    const int moved = fcntl(write_end, F_DUPFD_CLOEXEC,
                            kLauncherUiResultDescriptor + 1);
    if (moved < 0) {
      run.error = std::string("cannot move the result pipe: ") +
                  std::strerror(errno);
      CloseDescriptor(&read_end);
      CloseDescriptor(&write_end);
      return run;
    }
    CloseDescriptor(&write_end);
    write_end = moved;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, write_end,
                                   kLauncherUiResultDescriptor);
  std::string helper_string = helper.string();
  char* arguments[] = {helper_string.data(), nullptr};
  std::vector<std::string> owned_environment = environment;
  std::vector<char*> environment_pointers;
  environment_pointers.reserve(owned_environment.size() + 1);
  for (std::string& entry : owned_environment) {
    environment_pointers.push_back(entry.data());
  }
  environment_pointers.push_back(nullptr);
  pid_t child = -1;
  const int spawn_status =
      posix_spawn(&child, helper.c_str(), &actions, nullptr, arguments,
                  environment_pointers.data());
  posix_spawn_file_actions_destroy(&actions);
  CloseDescriptor(&write_end);
  if (spawn_status != 0) {
    CloseDescriptor(&read_end);
    run.error = std::string("cannot start ") + helper.string() + ": " +
                std::strerror(spawn_status);
    return run;
  }
  run.started = true;

  std::string received;
  bool line_complete = false;
  bool pipe_closed = false;
  bool exited = false;
  int wait_status = 0;
  while (!line_complete && !pipe_closed && !exited) {
    pollfd poll_entry{read_end, POLLIN, 0};
    const int ready = poll(&poll_entry, 1, kExitPollMilliseconds);
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (ready > 0) {
      char buffer[kMaximumLineBytes];
      const ssize_t count = read(read_end, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        pipe_closed = true;
        continue;
      }
      received.append(buffer, static_cast<std::size_t>(count));
      if (received.find('\n') != std::string::npos ||
          received.size() > kMaximumLineBytes) {
        line_complete = true;
      }
      continue;
    }
    // Nothing to read: a process the helper started may hold the pipe
    // although the helper itself is gone.
    exited = ReapIfExited(child, &wait_status, false);
  }
  if (exited) {
    // Whatever the helper wrote just before it exited is in the pipe.
    const int flags = fcntl(read_end, F_GETFL);
    if (flags >= 0) fcntl(read_end, F_SETFL, flags | O_NONBLOCK);
    char buffer[kMaximumLineBytes];
    ssize_t count = 0;
    while (received.size() <= kMaximumLineBytes &&
           (count = read(read_end, buffer, sizeof(buffer))) > 0) {
      received.append(buffer, static_cast<std::size_t>(count));
    }
  }
  CloseDescriptor(&read_end);
  if (!exited) {
    exited = ReapIfExited(child, &wait_status, true);
  }
  if (exited && WIFEXITED(wait_status)) {
    run.exit_status = WEXITSTATUS(wait_status);
  } else if (exited && WIFSIGNALED(wait_status)) {
    run.signal = WTERMSIG(wait_status);
  }

  const std::size_t newline = received.find('\n');
  const std::string line =
      newline == std::string::npos ? received : received.substr(0, newline);
  if (newline != std::string::npos) {
    run.result = ParseLauncherUiResultLine(line);
  }
  if (!run.result.has_value()) {
    if (received.empty()) {
      run.error = "the settings window reported no result";
    } else {
      run.error = "the settings window reported an unknown result";
    }
  }
  return run;
}

std::vector<std::string> RemoveUserManagedEnvironment(
    const std::vector<std::string>& user_managed_environment,
    const std::vector<std::string>& command_line_names,
    std::vector<std::string>* restart_environment) {
  std::vector<std::string> removed;
  for (const std::string& name : user_managed_environment) {
    if (name.empty() ||
        std::find(command_line_names.begin(), command_line_names.end(),
                  name) != command_line_names.end()) {
      continue;
    }
    unsetenv(name.c_str());
    if (restart_environment != nullptr) {
      restart_environment->erase(
          std::remove_if(restart_environment->begin(),
                         restart_environment->end(),
                         [&name](const std::string& entry) {
                           return NameOf(entry) == name;
                         }),
          restart_environment->end());
    }
    removed.push_back(name);
  }
  return removed;
}

}  // namespace runtime
}  // namespace mocktail
