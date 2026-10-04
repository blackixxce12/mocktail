#include "runtime/launcher_ui_launch.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

// A stand-in for mocktail_launcher_ui. FAKE_MODE picks what it does; it
// records its environment in <root>/environment, one entry per line.
class FakeLauncherHelper final {
 public:
  FakeLauncherHelper() {
    char pattern[] = "/tmp/mocktail_launcher_ui_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created == nullptr) {
      return;
    }
    root_ = created;
    helper_ = root_ / "helper.py";
    std::ofstream output(helper_);
    output << "#!/usr/bin/python3\n"
              "import os, signal, sys, time\n"
              "root = '"
           << root_.string()
           << "'\n"
              "with open(root + '/environment', 'w') as stream:\n"
              "    for name, value in sorted(os.environ.items()):\n"
              "        stream.write(name + '=' + value + '\\n')\n"
              "mode = os.environ.get('FAKE_MODE', '')\n"
              "fd = int(os.environ.get('MOCKTAIL_LAUNCHER_RESULT_FD', '-1'))\n"
              "def answer(text):\n"
              "    os.write(fd, text.encode())\n"
              "if mode == 'play':\n"
              "    answer('play\\n')\n"
              "elif mode == 'ignore':\n"
              "    answer('play ignore-env\\n')\n"
              "elif mode == 'quit':\n"
              "    answer('quit\\n')\n"
              "elif mode == 'garbage':\n"
              "    answer('launch the rockets\\n')\n"
              "elif mode == 'two-lines':\n"
              "    answer('quit\\nplay\\n')\n"
              "elif mode == 'unterminated':\n"
              "    answer('quit')\n"
              "elif mode == 'exit-1':\n"
              "    sys.exit(1)\n"
              "elif mode == 'crash':\n"
              "    os.kill(os.getpid(), signal.SIGKILL)\n"
              "elif mode == 'linger-after-answer':\n"
              "    answer('quit\\n')\n"
              "    time.sleep(0.5)\n"
              "elif mode == 'orphan-holds-pipe':\n"
              "    if os.fork() == 0:\n"
              "        time.sleep(6)\n"
              "        os._exit(0)\n"
              "    sys.exit(0)\n"
              "elif mode == 'slow':\n"
              "    time.sleep(0.6)\n"
              "    answer('play\\n')\n"
              "sys.exit(0)\n";
    output.close();
    std::filesystem::permissions(helper_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  ~FakeLauncherHelper() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& helper() const { return helper_; }

  LauncherUiRun Run(std::string_view mode) const {
    return RunLauncherUi(helper_,
                         {"PATH=/usr/bin:/bin",
                          "FAKE_MODE=" + std::string(mode),
                          "MOCKTAIL_LAUNCHER_RESULT_FD=3"});
  }

  std::vector<std::string> RecordedEnvironment() const {
    std::ifstream input(root_ / "environment");
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
      lines.push_back(line);
    }
    return lines;
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path helper_;
};

bool Contains(const std::vector<std::string>& entries,
              std::string_view entry) {
  return std::find(entries.begin(), entries.end(), entry) != entries.end();
}

bool HasName(const std::vector<std::string>& entries, std::string_view name) {
  return std::any_of(entries.begin(), entries.end(),
                     [name](const std::string& entry) {
                       return entry.size() > name.size() &&
                              entry.compare(0, name.size(), name) == 0 &&
                              entry[name.size()] == '=';
                     });
}

TEST(LauncherUiLaunchTest, ResultLinesRoundTrip) {
  for (const LauncherUiResult result :
       {LauncherUiResult::kPlay, LauncherUiResult::kPlayIgnoringEnvironment,
        LauncherUiResult::kQuit}) {
    const std::string line(LauncherUiResultLine(result));
    EXPECT_EQ(ParseLauncherUiResultLine(line), result);
    EXPECT_EQ(ParseLauncherUiResultLine(line + "\n"), result);
  }
  EXPECT_EQ(LauncherUiResultLine(LauncherUiResult::kPlay), "play");
  EXPECT_EQ(LauncherUiResultLine(LauncherUiResult::kPlayIgnoringEnvironment),
            "play ignore-env");
  EXPECT_EQ(LauncherUiResultLine(LauncherUiResult::kQuit), "quit");
  for (const char* invalid :
       {"", "\n", "PLAY", "play ", " play", "play\n\n", "quit now",
        "play ignore-env extra", "play\r\n"}) {
    EXPECT_FALSE(ParseLauncherUiResultLine(invalid).has_value()) << invalid;
  }
}

TEST(LauncherUiLaunchTest, ReadsEachResult) {
  FakeLauncherHelper fake;
  LauncherUiRun run = fake.Run("play");
  EXPECT_TRUE(run.started);
  EXPECT_EQ(run.result, LauncherUiResult::kPlay);
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_TRUE(run.error.empty()) << run.error;

  run = fake.Run("ignore");
  EXPECT_EQ(run.result, LauncherUiResult::kPlayIgnoringEnvironment);
  EXPECT_EQ(run.decision(), LauncherUiResult::kPlayIgnoringEnvironment);

  run = fake.Run("quit");
  EXPECT_EQ(run.result, LauncherUiResult::kQuit);
  EXPECT_EQ(run.decision(), LauncherUiResult::kQuit);

  run = fake.Run("slow");
  EXPECT_EQ(run.result, LauncherUiResult::kPlay);

  // The first line is the answer; the helper must not write two.
  run = fake.Run("two-lines");
  EXPECT_EQ(run.result, LauncherUiResult::kQuit);

  run = fake.Run("linger-after-answer");
  EXPECT_EQ(run.result, LauncherUiResult::kQuit);
  EXPECT_EQ(run.exit_status, 0);
}

TEST(LauncherUiLaunchTest, AnythingButAValidLineMeansPlay) {
  FakeLauncherHelper fake;
  for (const char* mode : {"garbage", "unterminated", "exit-1", "crash", ""}) {
    const LauncherUiRun run = fake.Run(mode);
    EXPECT_TRUE(run.started) << mode;
    EXPECT_FALSE(run.result.has_value()) << mode;
    EXPECT_EQ(run.decision(), LauncherUiResult::kPlay) << mode;
    EXPECT_FALSE(run.error.empty()) << mode;
  }
  const LauncherUiRun exited = fake.Run("exit-1");
  EXPECT_EQ(exited.exit_status, 1);
  const LauncherUiRun crashed = fake.Run("crash");
  EXPECT_EQ(crashed.signal, SIGKILL);
  EXPECT_EQ(crashed.exit_status, -1);
}

TEST(LauncherUiLaunchTest, DoesNotWaitForAProcessThatInheritedThePipe) {
  FakeLauncherHelper fake;
  const auto started = std::chrono::steady_clock::now();
  const LauncherUiRun run = fake.Run("orphan-holds-pipe");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_TRUE(run.started);
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_FALSE(run.result.has_value());
  EXPECT_LT(elapsed, std::chrono::seconds(4));
}

TEST(LauncherUiLaunchTest, MissingHelperFailsToStart) {
  const LauncherUiRun run =
      RunLauncherUi("/nonexistent/mocktail_launcher_ui", {"PATH=/usr/bin"});
  EXPECT_FALSE(run.started);
  EXPECT_FALSE(run.error.empty());
  EXPECT_EQ(run.decision(), LauncherUiResult::kPlay);
}

TEST(LauncherUiLaunchTest, HelperFindsThePipeWhereverItWasOpened) {
  FakeLauncherHelper fake;
  // With stdin closed, pipe2() hands out 0 and 3: the write end is already
  // number 3 and must still reach the helper without close-on-exec.
  if (fcntl(3, F_GETFD) != -1) {
    GTEST_SKIP() << "descriptor 3 is in use by the test process";
  }
  const int saved_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 100);
  ASSERT_GE(saved_stdin, 0);
  close(STDIN_FILENO);
  const LauncherUiRun run = fake.Run("quit");
  ASSERT_EQ(dup2(saved_stdin, STDIN_FILENO), STDIN_FILENO);
  close(saved_stdin);
  EXPECT_EQ(run.result, LauncherUiResult::kQuit);

  // And the usual case, with the pipe above 3.
  const int spare = dup(STDERR_FILENO);
  ASSERT_GE(spare, 0);
  const LauncherUiRun again = fake.Run("play");
  close(spare);
  EXPECT_EQ(again.result, LauncherUiResult::kPlay);
}

TEST(LauncherUiLaunchTest, BuildsTheHelperEnvironment) {
  const char* current[] = {
      "PATH=/usr/bin",
      "HOME=/home/player",
      // Added by mocktail for the game process: left out.
      "__GL_YIELD=USLEEP",
      "SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR=1",
      "ROBLOX_LIB_PATH=/opt/roblox/libroblox.so",
      // The user's own values: kept.
      "SDL_VIDEODRIVER=wayland",
      "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json",
      // Changed by mocktail: left out.
      "__GL_THREADED_OPTIMIZATIONS=1",
      "MOCKTAIL_WEBVIEW_HELPER=/usr/lib/mocktail/mocktail_webview_helper",
      "MOCKTAIL_VSYNC=on",
      // Stale protocol variables are replaced.
      "MOCKTAIL_LAUNCHER_RESULT_FD=7",
      "MOCKTAIL_CONFIG_FILE=/elsewhere/config.yaml",
      nullptr,
  };
  LauncherUiLaunchOptions options;
  options.config_file = "/home/player/.config/mocktail/config.yaml";
  options.user_managed_environment = {"MOCKTAIL_VSYNC", "SDL_VIDEODRIVER"};
  options.config_created = true;
  options.original_environment = {
      "PATH=/usr/bin",
      "SDL_VIDEODRIVER=wayland",
      "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json",
      "__GL_THREADED_OPTIMIZATIONS=0",
      "MOCKTAIL_VSYNC=on",
  };
  const std::vector<std::string> environment =
      BuildLauncherUiEnvironment(current, options);
  EXPECT_TRUE(Contains(environment, "PATH=/usr/bin"));
  EXPECT_TRUE(Contains(environment, "HOME=/home/player"));
  EXPECT_TRUE(Contains(environment, "SDL_VIDEODRIVER=wayland"));
  EXPECT_TRUE(Contains(
      environment,
      "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json"));
  EXPECT_TRUE(Contains(
      environment,
      "MOCKTAIL_WEBVIEW_HELPER=/usr/lib/mocktail/mocktail_webview_helper"));
  EXPECT_TRUE(Contains(environment, "MOCKTAIL_VSYNC=on"));
  EXPECT_FALSE(HasName(environment, "__GL_YIELD"));
  EXPECT_FALSE(HasName(environment, "SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR"));
  EXPECT_FALSE(HasName(environment, "ROBLOX_LIB_PATH"));
  EXPECT_FALSE(HasName(environment, "__GL_THREADED_OPTIMIZATIONS"));
  EXPECT_TRUE(Contains(
      environment,
      "MOCKTAIL_CONFIG_FILE=/home/player/.config/mocktail/config.yaml"));
  EXPECT_TRUE(Contains(environment,
                       "MOCKTAIL_LAUNCHER_ENV_OVERRIDES=MOCKTAIL_VSYNC,"
                       "SDL_VIDEODRIVER"));
  EXPECT_TRUE(Contains(environment, "MOCKTAIL_LAUNCHER_CONFIG_CREATED=1"));
  EXPECT_TRUE(Contains(environment, "MOCKTAIL_LAUNCHER_RESULT_FD=3"));
  EXPECT_FALSE(Contains(environment, "MOCKTAIL_LAUNCHER_RESULT_FD=7"));
  EXPECT_EQ(std::count_if(environment.begin(), environment.end(),
                          [](const std::string& entry) {
                            return entry.rfind("MOCKTAIL_CONFIG_FILE=", 0) ==
                                   0;
                          }),
            1);

  options.user_managed_environment.clear();
  options.config_created = false;
  const std::vector<std::string> plain =
      BuildLauncherUiEnvironment(current, options);
  EXPECT_TRUE(Contains(plain, "MOCKTAIL_LAUNCHER_ENV_OVERRIDES="));
  EXPECT_TRUE(Contains(plain, "MOCKTAIL_LAUNCHER_CONFIG_CREATED=0"));
}

TEST(LauncherUiLaunchTest, HelperReceivesTheEnvironment) {
  FakeLauncherHelper fake;
  const char* current[] = {"PATH=/usr/bin:/bin", "FAKE_MODE=quit", nullptr};
  LauncherUiLaunchOptions options;
  options.config_file = "/tmp/config.yaml";
  options.user_managed_environment = {"MOCKTAIL_THEME"};
  const LauncherUiRun run =
      RunLauncherUi(fake.helper(), BuildLauncherUiEnvironment(current, options));
  EXPECT_EQ(run.result, LauncherUiResult::kQuit);
  const std::vector<std::string> recorded = fake.RecordedEnvironment();
  EXPECT_TRUE(Contains(recorded, "MOCKTAIL_CONFIG_FILE=/tmp/config.yaml"));
  EXPECT_TRUE(Contains(recorded, "MOCKTAIL_LAUNCHER_ENV_OVERRIDES=MOCKTAIL_THEME"));
  EXPECT_TRUE(Contains(recorded, "MOCKTAIL_LAUNCHER_RESULT_FD=3"));
}

TEST(LauncherUiLaunchTest, ResolvesTheHelperFromTheOverride) {
  FakeLauncherHelper fake;
  EXPECT_EQ(ResolveLauncherUiHelperPath(MapEnvironment(
                {{"MOCKTAIL_LAUNCHER_UI_HELPER", fake.helper().string()}})),
            fake.helper());
  // An override must be absolute and executable; nothing else is tried.
  EXPECT_TRUE(ResolveLauncherUiHelperPath(
                  MapEnvironment({{"MOCKTAIL_LAUNCHER_UI_HELPER",
                                   "relative/mocktail_launcher_ui"}}))
                  .empty());
  EXPECT_TRUE(ResolveLauncherUiHelperPath(
                  MapEnvironment({{"MOCKTAIL_LAUNCHER_UI_HELPER",
                                   "/nonexistent/mocktail_launcher_ui"}}))
                  .empty());
  const std::filesystem::path data = fake.helper().parent_path() / "data";
  std::ofstream(data) << "not executable";
  EXPECT_TRUE(ResolveLauncherUiHelperPath(
                  MapEnvironment(
                      {{"MOCKTAIL_LAUNCHER_UI_HELPER", data.string()}}))
                  .empty());
}

TEST(LauncherUiLaunchTest, RemovesTheUsersVariablesButNotTheCommandLines) {
  setenv("MOCKTAIL_TEST_LAUNCHER_A", "a", 1);
  setenv("MOCKTAIL_TEST_LAUNCHER_B", "b", 1);
  setenv("MOCKTAIL_TEST_LAUNCHER_CLI", "cli", 1);
  std::vector<std::string> restart = {
      "PATH=/usr/bin",
      "MOCKTAIL_TEST_LAUNCHER_A=a",
      "MOCKTAIL_TEST_LAUNCHER_B=b",
      "MOCKTAIL_TEST_LAUNCHER_CLI=user",
      "MOCKTAIL_TEST_LAUNCHER_AB=keep",
  };
  const std::vector<std::string> removed = RemoveUserManagedEnvironment(
      {"MOCKTAIL_TEST_LAUNCHER_A", "MOCKTAIL_TEST_LAUNCHER_B",
       "MOCKTAIL_TEST_LAUNCHER_CLI"},
      {"MOCKTAIL_TEST_LAUNCHER_CLI"}, &restart);
  EXPECT_EQ(removed, (std::vector<std::string>{"MOCKTAIL_TEST_LAUNCHER_A",
                                               "MOCKTAIL_TEST_LAUNCHER_B"}));
  EXPECT_EQ(std::getenv("MOCKTAIL_TEST_LAUNCHER_A"), nullptr);
  EXPECT_EQ(std::getenv("MOCKTAIL_TEST_LAUNCHER_B"), nullptr);
  ASSERT_NE(std::getenv("MOCKTAIL_TEST_LAUNCHER_CLI"), nullptr);
  EXPECT_STREQ(std::getenv("MOCKTAIL_TEST_LAUNCHER_CLI"), "cli");
  EXPECT_EQ(restart,
            (std::vector<std::string>{"PATH=/usr/bin",
                                      "MOCKTAIL_TEST_LAUNCHER_CLI=user",
                                      "MOCKTAIL_TEST_LAUNCHER_AB=keep"}));
  unsetenv("MOCKTAIL_TEST_LAUNCHER_CLI");

  // Without a restart environment only the process changes.
  setenv("MOCKTAIL_TEST_LAUNCHER_A", "a", 1);
  EXPECT_EQ(RemoveUserManagedEnvironment({"MOCKTAIL_TEST_LAUNCHER_A"}, {},
                                         nullptr),
            (std::vector<std::string>{"MOCKTAIL_TEST_LAUNCHER_A"}));
  EXPECT_EQ(std::getenv("MOCKTAIL_TEST_LAUNCHER_A"), nullptr);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
