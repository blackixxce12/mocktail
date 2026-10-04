#include "runtime/process_relaunch.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/command_line.h"
#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "runtime/session_log.h"
#include "runtime/single_instance_lock.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr char kLaunchUri[] = "roblox://experiences/start?placeId=17580461965";

std::vector<std::string> RelaunchArguments(
    const std::vector<const char*>& argv) {
  const CommandLineParseResult parsed =
      ParseCommandLine(static_cast<int>(argv.size()), argv.data());
  EXPECT_TRUE(parsed) << parsed.error;
  std::vector<std::string> arguments;
  std::string error;
  EXPECT_TRUE(BuildProcessRelaunchArguments(parsed.options,
                                            static_cast<int>(argv.size()),
                                            argv.data(), &arguments, &error))
      << error;
  return arguments;
}

TEST(ProcessRelaunchTest, KeepsOptionsAndDropsTheWebsiteLaunch) {
  EXPECT_EQ(RelaunchArguments({"mocktail", "--windowed", "--graphics",
                               "vulkan"}),
            (std::vector<std::string>{"mocktail", "--windowed", "--graphics",
                                      "vulkan"}));
  EXPECT_EQ(RelaunchArguments({"mocktail", "--windowed", "--launch-uri",
                               kLaunchUri}),
            (std::vector<std::string>{"mocktail", "--windowed"}));
  EXPECT_EQ(RelaunchArguments({"mocktail", kLaunchUri, "--windowed"}),
            (std::vector<std::string>{"mocktail", "--windowed"}));
  EXPECT_EQ(RelaunchArguments({"/usr/bin/mocktail"}),
            (std::vector<std::string>{"/usr/bin/mocktail"}));
}

TEST(ProcessRelaunchTest, RequestIsProcessWide) {
  ResetProcessRelaunchRequestForTesting();
  EXPECT_FALSE(ProcessRelaunchRequested());
  RequestProcessRelaunch();
  EXPECT_TRUE(ProcessRelaunchRequested());
  ResetProcessRelaunchRequestForTesting();
  EXPECT_FALSE(ProcessRelaunchRequested());
}

TEST(ProcessRelaunchTest, CapturesHowTheProcessStarted) {
  ASSERT_EQ(setenv("MOCKTAIL_RELAUNCH_TEST_MARKER", "captured", 1), 0);
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGUSR1);
  sigset_t previous_mask;
  ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask), 0);
  struct sigaction ignore{};
  sigemptyset(&ignore.sa_mask);
  ignore.sa_handler = SIG_IGN;
  struct sigaction previous_action{};
  ASSERT_EQ(sigaction(SIGUSR2, &ignore, &previous_action), 0);

  const ProcessStartState start = CaptureProcessStartState();

  ASSERT_EQ(sigaction(SIGUSR2, &previous_action, nullptr), 0);
  ASSERT_EQ(pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr), 0);
  ASSERT_EQ(unsetenv("MOCKTAIL_RELAUNCH_TEST_MARKER"), 0);
  std::error_code error;
  EXPECT_TRUE(std::filesystem::equivalent(start.executable, "/proc/self/exe",
                                          error))
      << start.executable;
  EXPECT_EQ(start.executable.front(), '/');
  EXPECT_EQ(start.working_directory,
            std::filesystem::current_path(error).string());
  EXPECT_NE(std::find(start.environment.begin(), start.environment.end(),
                      "MOCKTAIL_RELAUNCH_TEST_MARKER=captured"),
            start.environment.end());
  EXPECT_EQ(sigismember(&start.signal_mask, SIGUSR1), 1);
  EXPECT_EQ(sigismember(&start.ignored_signals, SIGUSR2), 1);
  EXPECT_EQ(sigismember(&start.ignored_signals, SIGKILL), 0);
}

ProcessStartState ShellStart(std::vector<std::string> environment) {
  ProcessStartState start;
  start.executable = "/bin/sh";
  start.environment = std::move(environment);
  sigemptyset(&start.signal_mask);
  sigemptyset(&start.ignored_signals);
  return start;
}

void IgnoreSignal(int) {}

// Runs |script| through ExecProcessRelaunch in a child after |prepare| and
// returns its exit status, or -1 if it did not exit normally.
template <typename Prepare>
int RunRelaunched(const ProcessStartState& start, const std::string& script,
                  Prepare prepare) {
  const pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    prepare();
    std::string error;
    (void)ExecProcessRelaunch(start, {"sh", "-c", script}, &error);
    _exit(99);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

TEST(ProcessRelaunchTest, ExecUsesTheStartEnvironmentAndSignalMask) {
  ProcessStartState start = ShellStart({"CAPTURED=yes", "PATH=/usr/bin:/bin"});
  // Exits 7 only if the captured variable arrived, the one exported later did
  // not, and the signal blocked since the start is no longer blocked.
  EXPECT_EQ(RunRelaunched(start,
                          "test \"$CAPTURED\" = yes && test -z "
                          "\"$MOCKTAIL_EXPORTED_BY_THIS_RUN\" && "
                          "! grep -q '^SigBlk:.*[1-9a-f]' /proc/self/status "
                          "&& exit 7; exit 3",
                          [] {
                            sigset_t blocked;
                            sigemptyset(&blocked);
                            sigaddset(&blocked, SIGTERM);
                            (void)pthread_sigmask(SIG_BLOCK, &blocked,
                                                  nullptr);
                            (void)setenv("MOCKTAIL_EXPORTED_BY_THIS_RUN", "1",
                                         1);
                          }),
            7);

  // A mask the caller started with is kept: SIGUSR1 is signal 10, bit 9.
  sigaddset(&start.signal_mask, SIGUSR1);
  EXPECT_EQ(RunRelaunched(start,
                          "grep -q '^SigBlk:.*200$' /proc/self/status && "
                          "exit 7; exit 3",
                          [] {}),
            7);
}

// The website sign-in saved its session to the managed cookie file. A
// session named in the environment the process started with would replace
// it, so the restart leaves those variables out, and only those.
TEST(ProcessRelaunchTest, ExecLeavesOutSessionOverrides) {
  const ProcessStartState start = ShellStart(
      {"PATH=/usr/bin:/bin", "MOCKTAIL_ROBLOX_COOKIES=placeholder",
       "MOCKTAIL_COOKIE_FILE=/nonexistent/cookie",
       "MOCKTAIL_COOKIE_FILES=kept", "MOCKTAIL_AUTH_ROOT=/nonexistent/auth"});
  EXPECT_EQ(RunRelaunched(start,
                          "test -z \"${MOCKTAIL_ROBLOX_COOKIES+set}\" && "
                          "test -z \"${MOCKTAIL_COOKIE_FILE+set}\" && "
                          "test \"$MOCKTAIL_COOKIE_FILES\" = kept && "
                          "test \"$MOCKTAIL_AUTH_ROOT\" = /nonexistent/auth "
                          "&& exit 7; exit 3",
                          [] {}),
            7);
}

// Exec keeps ignored signals ignored and resets handled ones to the default,
// so without help the restart would keep what the run or the game chose.
// SIGUSR1 is bit 9 (0x200) of SigIgn, SIGUSR2 bit 11 (0x800).
TEST(ProcessRelaunchTest, ExecRestoresTheStartSignalDispositions) {
  ProcessStartState start = ShellStart({"PATH=/usr/bin:/bin"});
  sigaddset(&start.ignored_signals, SIGUSR1);
  EXPECT_EQ(RunRelaunched(start,
                          "ignored=$(sed -n 's/^SigIgn:[[:space:]]*//p' "
                          "/proc/self/status) && "
                          "test $((0x$ignored & 0x200)) -ne 0 && "
                          "test $((0x$ignored & 0x800)) -eq 0 && exit 7; "
                          "exit 3",
                          [] {
                            (void)signal(SIGUSR1, IgnoreSignal);
                            (void)signal(SIGUSR2, SIG_IGN);
                          }),
            7);
}

// The run, or the game through its libc, may change directory; relative
// arguments and paths refer to the one Mocktail started in.
TEST(ProcessRelaunchTest, ExecStartsInTheStartWorkingDirectory) {
  char pattern[] = "/tmp/mocktail_relaunch_cwd_XXXXXX";
  const char* directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  std::error_code error;
  const std::string canonical =
      std::filesystem::canonical(directory, error).string();
  ProcessStartState start = ShellStart({"PATH=/usr/bin:/bin"});
  start.working_directory = directory;
  EXPECT_EQ(RunRelaunched(start,
                          "test \"$(pwd -P)\" = '" + canonical +
                              "' && exit 7; exit 3",
                          [] {
                            if (chdir("/") != 0) _exit(98);
                          }),
            7);
  std::filesystem::remove_all(directory, error);
}

// Descriptors this run opened, such as a database with its locks, must not
// reach the restarted process; stdin, stdout and stderr do.
TEST(ProcessRelaunchTest, ExecClosesDescriptorsAboveStderr) {
  const ProcessStartState start = ShellStart({"PATH=/usr/bin:/bin"});
  EXPECT_EQ(RunRelaunched(start,
                          "test -e /proc/self/fd/2 && "
                          "test ! -e /proc/self/fd/40 && "
                          "test ! -e /proc/self/fd/41 && exit 7; exit 3",
                          [] {
                            const int file = open("/dev/null", O_RDONLY);
                            (void)dup2(file, 40);
                            std::array<int, 2> pipe_ends{};
                            if (pipe(pipe_ends.data()) == 0) {
                              (void)dup2(pipe_ends[1], 41);
                            }
                          }),
            7);
}

// After a failed exec Mocktail reports the failure and exits as usual, so
// the process must be as it was: descriptors open, directory and signal
// dispositions unchanged.
TEST(ProcessRelaunchTest, FailedExecReportsTheErrorAndRestoresTheProcess) {
  ProcessStartState start = ShellStart({});
  start.executable = "/nonexistent/mocktail";
  start.working_directory = "/";
  std::array<int, 2> pipe_ends{};
  ASSERT_EQ(pipe(pipe_ends.data()), 0);
  std::error_code directory_error;
  const std::filesystem::path directory =
      std::filesystem::current_path(directory_error);
  ASSERT_NE(directory.string(), "/");
  struct sigaction ignore{};
  sigemptyset(&ignore.sa_mask);
  ignore.sa_handler = SIG_IGN;
  struct sigaction previous_action{};
  ASSERT_EQ(sigaction(SIGUSR2, &ignore, &previous_action), 0);

  std::string error;
  EXPECT_FALSE(ExecProcessRelaunch(start, {"mocktail"}, &error));
  EXPECT_NE(error.find("cannot restart Mocktail"), std::string::npos);
  EXPECT_EQ(write(pipe_ends[1], "x", 1), 1);
  EXPECT_EQ(std::filesystem::current_path(directory_error).string(),
            directory.string());
  struct sigaction after{};
  ASSERT_EQ(sigaction(SIGUSR2, &previous_action, &after), 0);
  EXPECT_TRUE(after.sa_handler == SIG_IGN);
  close(pipe_ends[0]);
  close(pipe_ends[1]);
  start.executable.clear();
  EXPECT_FALSE(ExecProcessRelaunch(start, {"mocktail"}, &error));
  EXPECT_FALSE(ExecProcessRelaunch(ShellStart({}), {}, &error));
}

// The session log writer is forked after the lock is taken and keeps a copy
// of its descriptor. Release must unlock, not only close our copy, or the
// restarted process finds Mocktail "already running".
TEST(ProcessRelaunchTest, ReleasedLockIsFreeWhileAForkedChildHoldsItsCopy) {
  char pattern[] = "/tmp/mocktail_relaunch_lock_XXXXXX";
  const char* directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path lock_file =
      std::filesystem::path(directory) / "instance.lock";
  SingleInstanceLock lock = SingleInstanceLock::Acquire(lock_file);
  ASSERT_TRUE(lock.acquired()) << lock.error();

  std::array<int, 2> hold{};
  ASSERT_EQ(pipe(hold.data()), 0);
  const pid_t writer = fork();
  ASSERT_GE(writer, 0);
  if (writer == 0) {
    close(hold[1]);
    char byte = 0;
    (void)read(hold[0], &byte, 1);
    _exit(0);
  }
  close(hold[0]);

  {
    // Closing alone leaves the forked copy holding the lock.
    const std::filesystem::path other =
        std::filesystem::path(directory) / "closed-only.lock";
    std::optional<SingleInstanceLock> closed_only(
        SingleInstanceLock::Acquire(other));
    ASSERT_TRUE(closed_only->acquired());
    std::array<int, 2> other_hold{};
    ASSERT_EQ(pipe(other_hold.data()), 0);
    const pid_t other_writer = fork();
    ASSERT_GE(other_writer, 0);
    if (other_writer == 0) {
      close(other_hold[1]);
      char byte = 0;
      (void)read(other_hold[0], &byte, 1);
      _exit(0);
    }
    close(other_hold[0]);
    closed_only.reset();
    EXPECT_TRUE(SingleInstanceLock::Acquire(other).already_running());
    close(other_hold[1]);
    int other_status = 0;
    ASSERT_EQ(waitpid(other_writer, &other_status, 0), other_writer);
  }

  lock.Release();
  EXPECT_FALSE(lock.acquired());
  SingleInstanceLock restarted = SingleInstanceLock::Acquire(lock_file);
  EXPECT_TRUE(restarted.acquired()) << restarted.error();

  close(hold[1]);
  int status = 0;
  ASSERT_EQ(waitpid(writer, &status, 0), writer);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

// main()'s restart after website sign-in, in its order: lock, session log,
// a helper that outlives the run, then finish the log, release the lock and
// exec. The helper keeps the log writer, and with it a copy of the lock
// descriptor, alive past the exec; the restarted process still gets the lock,
// writes to the console instead of the old log, and the old log keeps what
// the run printed.
TEST(ProcessRelaunchTest, RestartHandsTheLockAndConsoleToTheNewProcess) {
  char pattern[] = "/tmp/mocktail_relaunch_restart_XXXXXX";
  const char* directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path root(directory);
  const std::filesystem::path lock_file = root / "instance.lock";
  const MapEnvironment environment({
      {"HOME", (root / "home").string()},
      {"MOCKTAIL_STATE_ROOT", (root / "state").string()},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  int release[2] = {-1, -1};
  ASSERT_EQ(pipe(release), 0);

  const pid_t run = fork();
  ASSERT_GE(run, 0);
  if (run == 0) {
    close(release[1]);
    SingleInstanceLock lock = SingleInstanceLock::Acquire(lock_file);
    SessionLog log = SessionLog::Start(environment, paths);
    if (!lock.acquired() || !log) _exit(90);
    const pid_t helper = fork();
    if (helper < 0) _exit(91);
    if (helper == 0) {
      char byte = 0;
      const ssize_t released = read(release[0], &byte, 1);
      (void)released;
      _exit(0);
    }
    close(release[0]);
    std::cout << "old-run-marker\n" << std::flush;
    log.FinishBeforeExec(std::chrono::milliseconds(100));
    lock.Release();
    std::string error;
    (void)ExecProcessRelaunch(
        ShellStart({"PATH=/usr/bin:/bin"}),
        {"sh", "-c",
         "flock -n '" + lock_file.string() +
             "' true && echo new-run-marker && exit 7; exit 3"},
        &error);
    _exit(99);
  }
  close(release[0]);
  int status = 0;
  ASSERT_EQ(waitpid(run, &status, 0), run);
  EXPECT_TRUE(WIFEXITED(status)) << status;
  EXPECT_EQ(WEXITSTATUS(status), 7);

  std::string contents;
  std::error_code error;
  for (std::filesystem::directory_iterator entry(paths.logs_root() / "sessions",
                                                 error),
       end;
       !error && entry != end; entry.increment(error)) {
    std::ifstream input(entry->path());
    std::ostringstream text;
    text << input.rdbuf();
    contents += text.str();
  }
  EXPECT_NE(contents.find("old-run-marker"), std::string::npos);
  EXPECT_EQ(contents.find("new-run-marker"), std::string::npos);

  // The helper, then the old writer, end.
  close(release[1]);
  std::filesystem::remove_all(root, error);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
