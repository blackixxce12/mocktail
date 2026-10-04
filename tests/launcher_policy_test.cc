#include "runtime/launcher_policy.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "runtime/runtime_config_bootstrap.h"

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

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_launcher_policy_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path Write(std::string_view contents) const {
    const std::filesystem::path file = path_ / "config.yaml";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << contents;
    return file;
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// An ordinary desktop-icon start: everything allows the window.
LauncherInputs DesktopStart() {
  LauncherInputs inputs;
  inputs.run_mode = true;
  inputs.display_available = true;
  inputs.helper_present = true;
  inputs.show_on_start = true;
  return inputs;
}

TEST(LauncherPolicyTest, DesktopStartFollowsShowOnStart) {
  LauncherInputs inputs = DesktopStart();
  EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kShow);
  EXPECT_EQ(DescribeLauncherDecision(inputs), "launcher.show_on_start");
  inputs.show_on_start = false;
  EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kSkip);
  EXPECT_EQ(DescribeLauncherDecision(inputs), "launcher.show_on_start");
}

TEST(LauncherPolicyTest, CommandLineOverridesShowOnStart) {
  LauncherInputs inputs = DesktopStart();
  inputs.cli_skip = true;
  EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kSkip);
  EXPECT_EQ(DescribeLauncherDecision(inputs), "--play");

  inputs = DesktopStart();
  inputs.show_on_start = false;
  inputs.cli_force_show = true;
  EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kShow);
  EXPECT_EQ(DescribeLauncherDecision(inputs), "--launcher");

  // The parser rejects both together; if both still arrive, skipping is
  // the choice that can never keep the game from starting.
  inputs.cli_skip = true;
  EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kSkip);
}

TEST(LauncherPolicyTest, NeverShowsWhereTheWindowCannotOrMustNotAppear) {
  struct Case {
    const char* reason;
    void (*apply)(LauncherInputs*);
  };
  for (const Case& entry : {
           Case{"not an interactive run",
                [](LauncherInputs* inputs) { inputs->run_mode = false; }},
           Case{"website join",
                [](LauncherInputs* inputs) {
                  inputs->external_launch_request = true;
                }},
           Case{"update canary",
                [](LauncherInputs* inputs) { inputs->isolated_canary = true; }},
           Case{"unsafe latest run",
                [](LauncherInputs* inputs) { inputs->unsafe_latest = true; }},
           Case{"headless run",
                [](LauncherInputs* inputs) { inputs->headless = true; }},
           Case{"no display",
                [](LauncherInputs* inputs) {
                  inputs->display_available = false;
                }},
           Case{"settings window helper missing",
                [](LauncherInputs* inputs) { inputs->helper_present = false; }},
       }) {
    for (const bool force_show : {false, true}) {
      LauncherInputs inputs = DesktopStart();
      inputs.cli_force_show = force_show;
      entry.apply(&inputs);
      EXPECT_EQ(DecideLauncher(inputs), LauncherDecision::kSkip)
          << entry.reason << " force_show=" << force_show;
      EXPECT_EQ(DescribeLauncherDecision(inputs), entry.reason);
    }
  }
}

TEST(LauncherPolicyTest, DefaultInputsNeverShowTheWindow) {
  EXPECT_EQ(DecideLauncher(LauncherInputs{}), LauncherDecision::kSkip);
}

TEST(LauncherPolicyTest, ReadsEnvironmentInputs) {
  LauncherInputs inputs;
  ReadLauncherEnvironment(MapEnvironment({{"WAYLAND_DISPLAY", "wayland-1"},
                                          {"MOCKTAIL_HEADLESS", "0"}}),
                          &inputs);
  EXPECT_TRUE(inputs.display_available);
  EXPECT_FALSE(inputs.headless);
  EXPECT_FALSE(inputs.isolated_canary);
  EXPECT_FALSE(inputs.unsafe_latest);

  ReadLauncherEnvironment(MapEnvironment({{"DISPLAY", ":0"},
                                          {"MOCKTAIL_HEADLESS", "1"},
                                          {"MOCKTAIL_ISOLATED_CANARY", "1"},
                                          {"MOCKTAIL_UNSAFE_LATEST", "1"}}),
                          &inputs);
  EXPECT_TRUE(inputs.display_available);
  EXPECT_TRUE(inputs.headless);
  EXPECT_TRUE(inputs.isolated_canary);
  EXPECT_TRUE(inputs.unsafe_latest);

  ReadLauncherEnvironment(MapEnvironment({{"DISPLAY", ""},
                                          {"WAYLAND_DISPLAY", ""},
                                          {"MOCKTAIL_HEADLESS", ""},
                                          {"MOCKTAIL_ISOLATED_CANARY", "0"},
                                          {"MOCKTAIL_UNSAFE_LATEST", "yes"}}),
                          &inputs);
  EXPECT_FALSE(inputs.display_available);
  EXPECT_FALSE(inputs.headless);
  EXPECT_FALSE(inputs.isolated_canary);
  // Like the instance lock, these only count the exact marker value.
  EXPECT_FALSE(inputs.unsafe_latest);

  ReadLauncherEnvironment(MapEnvironment(), nullptr);
}

TEST(LauncherPolicyTest, ReadsShowOnStartFromTheConfigFile) {
  TemporaryDirectory temporary;
  EXPECT_TRUE(ReadLauncherShowOnStart(temporary.path() / "missing.yaml",
                                      MapEnvironment()));
  EXPECT_TRUE(ReadLauncherShowOnStart(
      temporary.Write(DefaultRuntimeConfigYaml()), MapEnvironment()));
  EXPECT_FALSE(ReadLauncherShowOnStart(
      temporary.Write("version: 1\nlauncher:\n  show_on_start: false\n"),
      MapEnvironment()));
  EXPECT_TRUE(ReadLauncherShowOnStart(
      temporary.Write("version: 1\nlauncher:\n  show_on_start: true\n"),
      MapEnvironment()));
  // A config without the section, written before it existed.
  EXPECT_TRUE(ReadLauncherShowOnStart(
      temporary.Write("version: 1\ngraphics:\n  vsync: off\n"),
      MapEnvironment()));
}

TEST(LauncherPolicyTest, BrokenConfigOpensTheWindow) {
  TemporaryDirectory temporary;
  for (const char* broken : {
           "launcher: [\n",
           "version: 1\nlauncher:\n  show_on_start: maybe\n",
           "version: 1\nlauncher:\n  show_on_start: false\n  extra: 1\n",
           // Valid launcher section, broken elsewhere: still opened, so the
           // error can be shown and fixed before Roblox would refuse it.
           "version: 1\nlauncher:\n  show_on_start: false\n"
           "graphics:\n  vsync: sometimes\n",
           "version: 2\nlauncher:\n  show_on_start: false\n",
           "",
       }) {
    EXPECT_TRUE(ReadLauncherShowOnStart(temporary.Write(broken),
                                        MapEnvironment()))
        << broken;
  }
  const std::filesystem::path target =
      temporary.Write("version: 1\nlauncher:\n  show_on_start: false\n");
  const std::filesystem::path link = temporary.path() / "linked.yaml";
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);
  EXPECT_TRUE(ReadLauncherShowOnStart(link, MapEnvironment()));
}

TEST(LauncherPolicyTest, ShowOnStartVariableWins) {
  TemporaryDirectory temporary;
  const std::filesystem::path shown =
      temporary.path() / "shown.yaml";
  std::ofstream(shown) << "version: 1\nlauncher:\n  show_on_start: true\n";
  const std::filesystem::path hidden = temporary.Write(
      "version: 1\nlauncher:\n  show_on_start: false\n");

  for (const char* off : {"0", "false", "off"}) {
    EXPECT_FALSE(ReadLauncherShowOnStart(
        shown, MapEnvironment({{"MOCKTAIL_LAUNCHER_SHOW_ON_START", off}})))
        << off;
  }
  for (const char* on : {"1", "true", "on"}) {
    EXPECT_TRUE(ReadLauncherShowOnStart(
        hidden, MapEnvironment({{"MOCKTAIL_LAUNCHER_SHOW_ON_START", on}})))
        << on;
  }
  // Empty hides the YAML value and means the default; garbage opens the
  // window rather than guessing.
  EXPECT_TRUE(ReadLauncherShowOnStart(
      hidden, MapEnvironment({{"MOCKTAIL_LAUNCHER_SHOW_ON_START", ""}})));
  EXPECT_TRUE(ReadLauncherShowOnStart(
      hidden, MapEnvironment({{"MOCKTAIL_LAUNCHER_SHOW_ON_START", "maybe"}})));
  // An explicit choice holds even when the file would not load.
  EXPECT_FALSE(ReadLauncherShowOnStart(
      temporary.Write("launcher: [\n"),
      MapEnvironment({{"MOCKTAIL_LAUNCHER_SHOW_ON_START", "0"}})));
  // Other variables never decide whether the file is readable here.
  EXPECT_FALSE(ReadLauncherShowOnStart(
      temporary.Write("version: 1\nlauncher:\n  show_on_start: false\n"),
      MapEnvironment({{"MOCKTAIL_VSYNC", "sometimes"}})));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
