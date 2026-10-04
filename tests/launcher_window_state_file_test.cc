#include "launcher/window_state_file.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "window/window_state_store.h"

namespace mocktail::launcher {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_launcher_window_state_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

// The shape the game writes after a fullscreen session on X11.
constexpr char kFullscreenState[] =
    R"({"fullscreen":true,"maximized":false,"schema_version":1,)"
    R"("windowed":{"has_position":true,"height":900,"width":1600,)"
    R"("x":16,"y":72}})";

class LauncherWindowStateFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(temporary_.path().empty());
    file_ = temporary_.path() / "window-state.json";
  }

  TemporaryDirectory temporary_;
  std::filesystem::path file_;
};

TEST_F(LauncherWindowStateFileTest, ChangesOnlyTheWindowedSize) {
  WriteFile(file_, kFullscreenState);
  ASSERT_EQ(chmod(file_.c_str(), 0644), 0);
  std::string error;
  WindowedSizeUpdate outcome = WindowedSizeUpdate::kNoStateFile;
  ASSERT_TRUE(UpdateWindowedSize(file_, 1920, 1080, &error, &outcome))
      << error;
  EXPECT_EQ(outcome, WindowedSizeUpdate::kUpdated);
  EXPECT_EQ(FileMode(file_), 0600);

  const window::WindowStateLoadResult loaded = window::LoadWindowState(file_);
  ASSERT_TRUE(loaded) << loaded.status.message();
  ASSERT_TRUE(loaded.found);
  EXPECT_EQ(loaded.state.width, 1920);
  EXPECT_EQ(loaded.state.height, 1080);
  EXPECT_TRUE(loaded.state.fullscreen);
  EXPECT_FALSE(loaded.state.maximized);
  EXPECT_TRUE(loaded.state.has_position);
  EXPECT_EQ(loaded.state.x, 16);
  EXPECT_EQ(loaded.state.y, 72);

  RememberedWindowState remembered;
  ASSERT_TRUE(ReadRememberedWindowState(file_, &remembered, &error)) << error;
  EXPECT_TRUE(remembered.found);
  EXPECT_EQ(remembered.width, 1920);
  EXPECT_EQ(remembered.height, 1080);
  EXPECT_TRUE(remembered.fullscreen);
  EXPECT_FALSE(remembered.maximized);
}

TEST_F(LauncherWindowStateFileTest, LeavesAnUnchangedSizeAlone) {
  WriteFile(file_, kFullscreenState);
  std::string error;
  WindowedSizeUpdate outcome = WindowedSizeUpdate::kUpdated;
  ASSERT_TRUE(UpdateWindowedSize(file_, 1600, 900, &error, &outcome)) << error;
  EXPECT_EQ(outcome, WindowedSizeUpdate::kUnchanged);
  EXPECT_EQ(ReadFile(file_), kFullscreenState);
}

TEST_F(LauncherWindowStateFileTest, NeverCreatesTheFile) {
  std::string error;
  WindowedSizeUpdate outcome = WindowedSizeUpdate::kUpdated;
  ASSERT_TRUE(UpdateWindowedSize(file_, 1920, 1080, &error, &outcome))
      << error;
  EXPECT_EQ(outcome, WindowedSizeUpdate::kNoStateFile);
  EXPECT_FALSE(std::filesystem::exists(file_));

  RememberedWindowState remembered;
  remembered.found = true;
  ASSERT_TRUE(ReadRememberedWindowState(file_, &remembered, &error)) << error;
  EXPECT_FALSE(remembered.found);
}

TEST_F(LauncherWindowStateFileTest, LeavesAFileTheGameIgnoresAlone) {
  const std::string invalid =
      R"({"schema_version":2,"fullscreen":false,"maximized":false,)"
      R"("windowed":{"has_position":false,"width":1280,"height":720}})";
  WriteFile(file_, invalid);
  std::string error;
  WindowedSizeUpdate outcome = WindowedSizeUpdate::kUpdated;
  ASSERT_TRUE(UpdateWindowedSize(file_, 1920, 1080, &error, &outcome))
      << error;
  EXPECT_EQ(outcome, WindowedSizeUpdate::kInvalidStateFile);
  EXPECT_EQ(ReadFile(file_), invalid);

  WriteFile(file_, "{not json");
  ASSERT_TRUE(UpdateWindowedSize(file_, 1920, 1080, &error, &outcome))
      << error;
  EXPECT_EQ(outcome, WindowedSizeUpdate::kInvalidStateFile);
  RememberedWindowState remembered;
  ASSERT_TRUE(ReadRememberedWindowState(file_, &remembered, &error)) << error;
  EXPECT_FALSE(remembered.found);
}

TEST_F(LauncherWindowStateFileTest, RefusesSizesTheGameWouldReject) {
  WriteFile(file_, kFullscreenState);
  std::string error;
  EXPECT_FALSE(UpdateWindowedSize(file_, window::kMinimumWindowWidth - 1, 900,
                                  &error));
  EXPECT_FALSE(UpdateWindowedSize(file_, 1600,
                                  window::kMinimumWindowHeight - 1, &error));
  EXPECT_FALSE(UpdateWindowedSize(file_, 16385, 900, &error));
  EXPECT_FALSE(UpdateWindowedSize(file_, 1600, 16385, &error));
  EXPECT_NE(error.find("160x120"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(file_), kFullscreenState);
  EXPECT_TRUE(UpdateWindowedSize(file_, window::kMinimumWindowWidth,
                                 window::kMinimumWindowHeight, &error))
      << error;
}

TEST_F(LauncherWindowStateFileTest, RefusesSymlinksAndRelativePaths) {
  const std::filesystem::path target = temporary_.path() / "elsewhere.json";
  WriteFile(target, kFullscreenState);
  ASSERT_EQ(symlink(target.c_str(), file_.c_str()), 0);
  std::string error;
  EXPECT_FALSE(UpdateWindowedSize(file_, 1920, 1080, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(ReadFile(target), kFullscreenState);
  RememberedWindowState remembered;
  EXPECT_FALSE(ReadRememberedWindowState(file_, &remembered, &error));

  EXPECT_FALSE(UpdateWindowedSize("window-state.json", 1920, 1080, &error));
  EXPECT_FALSE(
      ReadRememberedWindowState("window-state.json", &remembered, &error));
}

}  // namespace
}  // namespace mocktail::launcher
