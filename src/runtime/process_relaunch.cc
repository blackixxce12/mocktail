#include "runtime/process_relaunch.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

extern char** environ;

namespace mocktail {
namespace runtime {
namespace {

std::atomic<bool> g_relaunch_requested{false};

constexpr char kSelfExecutable[] = "/proc/self/exe";
constexpr std::string_view kDeletedSuffix = " (deleted)";

// The restart follows a website sign-in whose session the credential sink
// saved to the managed cookie file. Each of these names a session that
// would be loaded in its place.
constexpr std::string_view kSessionOverrideVariables[] = {
    "MOCKTAIL_ROBLOX_COOKIES",
    "MOCKTAIL_COOKIE_FILE",
};

bool IsLaunchOption(std::string_view argument) {
  return argument == "--launch-uri" || argument == "--launch-request-json";
}

bool IsSessionOverride(std::string_view entry) {
  for (const std::string_view name : kSessionOverrideVariables) {
    if (entry.size() > name.size() && entry.substr(0, name.size()) == name &&
        entry[name.size()] == '=') {
      return true;
    }
  }
  return false;
}

// The path Mocktail was started from. A package upgrade during the run
// replaces the file there, and the restart should run the new build with
// the helpers installed beside it, as opening Mocktail again would.
std::string InstalledExecutable() {
  std::array<char, 4097> path{};
  const ssize_t size = readlink(kSelfExecutable, path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) {
    return kSelfExecutable;
  }
  const std::string_view resolved(path.data(), static_cast<std::size_t>(size));
  if (resolved.front() != '/' ||
      (resolved.size() > kDeletedSuffix.size() &&
       resolved.substr(resolved.size() - kDeletedSuffix.size()) ==
           kDeletedSuffix)) {
    return kSelfExecutable;
  }
  return std::string(resolved);
}

std::string WorkingDirectory() {
  std::array<char, 4097> path{};
  return getcwd(path.data(), path.size()) != nullptr ? std::string(path.data())
                                                     : std::string();
}

bool IgnoredDisposition(const struct sigaction& action) {
  return (action.sa_flags & SA_SIGINFO) == 0 && action.sa_handler == SIG_IGN;
}

void CloseDescriptorsOnExec() {
#if defined(SYS_close_range)
  // CLOSE_RANGE_CLOEXEC (Linux 5.11) marks the range without closing it, so
  // a failed exec leaves this process able to report the failure.
  constexpr unsigned int kCloseRangeCloexec = 1U << 2;
  if (syscall(SYS_close_range, 3U, ~0U, kCloseRangeCloexec) == 0) {
    return;
  }
#endif
  DIR* directory = opendir("/proc/self/fd");
  if (directory == nullptr) {
    return;
  }
  const int own_descriptor = dirfd(directory);
  for (const dirent* entry = readdir(directory); entry != nullptr;
       entry = readdir(directory)) {
    char* end = nullptr;
    const long descriptor = std::strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0' || descriptor < 3 ||
        descriptor == own_descriptor) {
      continue;
    }
    const int flags = fcntl(static_cast<int>(descriptor), F_GETFD);
    if (flags >= 0) {
      (void)fcntl(static_cast<int>(descriptor), F_SETFD, flags | FD_CLOEXEC);
    }
  }
  closedir(directory);
}

// Exec resets handled signals to the default but keeps ignored ones ignored,
// so a signal the run or the game chose to ignore would stay ignored in the
// restart, and one ignored at the start but handled since would not be.
// Returns each disposition changed here so a failed exec can restore it.
std::vector<std::pair<int, struct sigaction>> RestoreIgnoredSignals(
    const sigset_t& ignored_at_start) {
  std::vector<std::pair<int, struct sigaction>> changed;
  for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
    if (signal_number == SIGKILL || signal_number == SIGSTOP) {
      continue;
    }
    struct sigaction current{};
    // glibc refuses the real-time signals it reserves for itself.
    if (sigaction(signal_number, nullptr, &current) != 0) {
      continue;
    }
    const bool ignore = sigismember(&ignored_at_start, signal_number) == 1;
    if (ignore == IgnoredDisposition(current)) {
      continue;
    }
    struct sigaction start_action{};
    sigemptyset(&start_action.sa_mask);
    start_action.sa_handler = ignore ? SIG_IGN : SIG_DFL;
    if (sigaction(signal_number, &start_action, nullptr) == 0) {
      changed.emplace_back(signal_number, current);
    }
  }
  return changed;
}

}  // namespace

void RequestProcessRelaunch() {
  g_relaunch_requested.store(true, std::memory_order_release);
}

bool ProcessRelaunchRequested() {
  return g_relaunch_requested.load(std::memory_order_acquire);
}

void ResetProcessRelaunchRequestForTesting() {
  g_relaunch_requested.store(false, std::memory_order_release);
}

bool BuildProcessRelaunchArguments(const CommandLineOptions& options, int argc,
                                   const char* const argv[],
                                   std::vector<std::string>* arguments,
                                   std::string* error) {
  if (arguments == nullptr || argc < 1 || argv == nullptr ||
      argv[0] == nullptr) {
    if (error != nullptr) *error = "cannot prepare relaunch arguments";
    return false;
  }
  arguments->clear();
  arguments->reserve(static_cast<std::size_t>(argc));
  arguments->emplace_back(argv[0]);
  const int launch_index = options.launch_request_json.empty()
                               ? -1
                               : options.launch_argument_index;
  if (!options.launch_request_json.empty() &&
      (launch_index <= 0 || launch_index >= argc)) {
    arguments->clear();
    if (error != nullptr) *error = "cannot locate the website launch request";
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == nullptr) {
      arguments->clear();
      if (error != nullptr) *error = "cannot prepare relaunch arguments";
      return false;
    }
    if (index == launch_index ||
        (index + 1 == launch_index && IsLaunchOption(argv[index]))) {
      continue;
    }
    arguments->emplace_back(argv[index]);
  }
  if (error != nullptr) error->clear();
  return true;
}

ProcessStartState CaptureProcessStartState() {
  ProcessStartState start;
  start.executable = InstalledExecutable();
  start.working_directory = WorkingDirectory();
  for (char** entry = environ; entry != nullptr && *entry != nullptr;
       ++entry) {
    start.environment.emplace_back(*entry);
  }
  sigemptyset(&start.signal_mask);
  (void)pthread_sigmask(SIG_BLOCK, nullptr, &start.signal_mask);
  sigemptyset(&start.ignored_signals);
  for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
    struct sigaction action{};
    if (sigaction(signal_number, nullptr, &action) == 0 &&
        IgnoredDisposition(action)) {
      sigaddset(&start.ignored_signals, signal_number);
    }
  }
  return start;
}

bool ExecProcessRelaunch(const ProcessStartState& start,
                         const std::vector<std::string>& arguments,
                         std::string* error) {
  if (start.executable.empty() || arguments.empty()) {
    if (error != nullptr) *error = "relaunch has no executable or arguments";
    return false;
  }
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<char*> envp;
  envp.reserve(start.environment.size() + 1);
  for (const std::string& entry : start.environment) {
    if (!IsSessionOverride(entry)) {
      envp.push_back(const_cast<char*>(entry.c_str()));
    }
  }
  envp.push_back(nullptr);

  CloseDescriptorsOnExec();
  // The run, or the game through its libc, may have changed directory;
  // relative arguments and paths refer to the one Mocktail started in.
  const int previous_directory = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (!start.working_directory.empty() &&
      chdir(start.working_directory.c_str()) != 0) {
    // Removed since: the restart starts where this run is.
  }
  const std::vector<std::pair<int, struct sigaction>> changed_signals =
      RestoreIgnoredSignals(start.ignored_signals);
  // The mask survives exec, and the runtime may have blocked signals since.
  sigset_t current;
  sigemptyset(&current);
  (void)pthread_sigmask(SIG_SETMASK, &start.signal_mask, &current);
  execve(start.executable.c_str(), argv.data(), envp.data());
  const int exec_error = errno;
  (void)pthread_sigmask(SIG_SETMASK, &current, nullptr);
  for (const auto& [signal_number, action] : changed_signals) {
    (void)sigaction(signal_number, &action, nullptr);
  }
  if (previous_directory >= 0) {
    if (fchdir(previous_directory) != 0) {
      // Stays in the start directory, which the failure report does not use.
    }
    close(previous_directory);
  }
  if (error != nullptr) {
    *error =
        std::string("cannot restart Mocktail: ") + std::strerror(exec_error);
  }
  return false;
}

}  // namespace runtime
}  // namespace mocktail
