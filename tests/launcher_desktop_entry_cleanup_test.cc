#include "launcher/desktop_entry_cleanup.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

namespace mocktail::launcher {
namespace {

class MapEnvironment final : public runtime::Environment {
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

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_desktop_entry_XXXXXX";
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

void WriteFile(const std::filesystem::path& path, const std::string& bytes,
               mode_t mode = 0644) {
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
  }
  ASSERT_EQ(chmod(path.c_str(), mode), 0);
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

std::string PackagedEntry() {
  return ReadFile(std::filesystem::path(MOCKTAIL_TEST_SOURCE_DIR) /
                  "packaging/space.bigrat.mocktail.desktop");
}

// A user copy of the packaged entry from before the settings window: notes
// in Russian, a forced video driver, and no X-Mocktail-Managed marker.
constexpr char kUserEntry[] =
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=Mocktail\n"
    "GenericName=Roblox Player\n"
    "Comment=Play Roblox on Linux\n"
    "# На NVIDIA Mocktail сам выбирает XWayland, и мышь в играх\n"
    "# перестаёт захватываться. Поэтому принудительно Wayland:\n"
    "Exec=env SDL_VIDEODRIVER=wayland mocktail %u\n"
    "Icon=space.bigrat.mocktail\n"
    "StartupWMClass=space.bigrat.mocktail\n"
    "Categories=Game;\n"
    "Terminal=false\n"
    "MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;\n";

class LauncherDesktopEntryCleanupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(temporary_.path().empty());
    paths_.user_file =
        temporary_.path() / "home/applications" / kDesktopEntryFileName;
    system_file_ =
        temporary_.path() / "usr/share/applications" / kDesktopEntryFileName;
    paths_.system_files = {
        temporary_.path() / "usr/local/share/applications" /
            kDesktopEntryFileName,
        system_file_};
    options_.database_tool = "true";
  }

  std::string Replace(std::string text, const std::string& from,
                      const std::string& to) {
    const std::size_t position = text.find(from);
    EXPECT_NE(position, std::string::npos) << from;
    if (position != std::string::npos) {
      text.replace(position, from.size(), to);
    }
    return text;
  }

  TemporaryDirectory temporary_;
  DesktopEntryPaths paths_;
  std::filesystem::path system_file_;
  DesktopEntryApplyOptions options_;
};

TEST_F(LauncherDesktopEntryCleanupTest, DerivesXdgPaths) {
  DesktopEntryPaths paths = DefaultDesktopEntryPaths(MapEnvironment(
      {{"HOME", "/home/player"},
       {"XDG_DATA_DIRS", "/usr/share:relative:/opt/share:"}}));
  EXPECT_EQ(paths.user_file,
            "/home/player/.local/share/applications/"
            "space.bigrat.mocktail.desktop");
  ASSERT_EQ(paths.system_files.size(), 2U);
  EXPECT_EQ(paths.system_files[0],
            "/usr/share/applications/space.bigrat.mocktail.desktop");
  EXPECT_EQ(paths.system_files[1],
            "/opt/share/applications/space.bigrat.mocktail.desktop");

  paths = DefaultDesktopEntryPaths(MapEnvironment(
      {{"HOME", "/home/player"}, {"XDG_DATA_HOME", "/data"}}));
  EXPECT_EQ(paths.user_file,
            "/data/applications/space.bigrat.mocktail.desktop");
  ASSERT_EQ(paths.system_files.size(), 2U);
  EXPECT_EQ(paths.system_files[0],
            "/usr/local/share/applications/space.bigrat.mocktail.desktop");

  paths = DefaultDesktopEntryPaths(MapEnvironment(
      {{"HOME", "/home/player"}, {"XDG_DATA_HOME", "data"}}));
  EXPECT_EQ(paths.user_file,
            "/home/player/.local/share/applications/"
            "space.bigrat.mocktail.desktop");
  EXPECT_TRUE(DefaultDesktopEntryPaths(MapEnvironment()).user_file.empty());
}

TEST_F(LauncherDesktopEntryCleanupTest, PlansToDeleteACopyThatOnlyAddsEnv) {
  // The packaged entry has translations (Comment[ru], ...) the old copy
  // lacks; they do not make the copy custom.
  ASSERT_NE(PackagedEntry().find("\nComment[ru]="), std::string::npos);
  WriteFile(paths_.user_file, kUserEntry);
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_TRUE(inspection.found);
  EXPECT_EQ(inspection.path, paths_.user_file);
  EXPECT_EQ(inspection.system_path, system_file_);
  EXPECT_EQ(inspection.exec, "env SDL_VIDEODRIVER=wayland mocktail %u");
  ASSERT_EQ(inspection.env_assignments.size(), 1U);
  EXPECT_EQ(inspection.env_assignments[0].name, "SDL_VIDEODRIVER");
  EXPECT_EQ(inspection.env_assignments[0].value, "wayland");
  EXPECT_EQ(inspection.new_exec, "mocktail %u");
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kDeleteFile);
  EXPECT_TRUE(inspection.note.empty()) << inspection.note;
  // Inspecting changes nothing.
  EXPECT_EQ(ReadFile(paths_.user_file), kUserEntry);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(
                              paths_.user_file.parent_path()),
                          std::filesystem::directory_iterator()),
            1);

  // The packaged entry gaining desktop actions keeps the copy deletable.
  WriteFile(system_file_,
            PackagedEntry() +
                "Actions=play;settings;\n"
                "\n"
                "[Desktop Action play]\n"
                "Name=Play now\n"
                "Name[ru]=Играть сразу\n"
                "Exec=mocktail --play\n"
                "\n"
                "[Desktop Action settings]\n"
                "Name=Mocktail Settings\n"
                "Name[ru]=Настройки Mocktail\n"
                "Exec=mocktail --launcher\n");
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kDeleteFile);

  DesktopEntryApplyResult result;
  ASSERT_TRUE(ApplyDesktopEntryCleanup(inspection, options_, &result, &error))
      << error;
  EXPECT_FALSE(std::filesystem::exists(paths_.user_file));
  EXPECT_EQ(result.backup_path,
            paths_.user_file.string() + ".mocktail-backup");
  EXPECT_EQ(ReadFile(result.backup_path), kUserEntry);
  EXPECT_EQ(FileMode(result.backup_path), 0600);
  EXPECT_TRUE(result.database_refreshed) << result.database_warning;
}

TEST_F(LauncherDesktopEntryCleanupTest, RewritesOnlyTheExecLineOfACustomCopy) {
  const std::string custom =
      Replace(kUserEntry, "Name=Mocktail\n", "Name=Roblox (Mocktail)\n");
  WriteFile(paths_.user_file, custom, 0644);
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  EXPECT_EQ(inspection.rewritten_bytes,
            Replace(custom, "Exec=env SDL_VIDEODRIVER=wayland mocktail %u\n",
                    "Exec=mocktail %u\n"));

  // A fake database tool records what it was asked to refresh.
  const std::filesystem::path record = temporary_.path() / "refreshed";
  const std::filesystem::path tool = temporary_.path() / "fake-tool";
  WriteFile(tool, "#!/bin/sh\nprintf '%s' \"$1\" > '" + record.string() + "'\n",
            0700);
  options_.database_tool = tool.string();
  DesktopEntryApplyResult result;
  ASSERT_TRUE(ApplyDesktopEntryCleanup(inspection, options_, &result, &error))
      << error;
  EXPECT_EQ(ReadFile(paths_.user_file), inspection.rewritten_bytes);
  EXPECT_EQ(FileMode(paths_.user_file), 0644);
  EXPECT_EQ(ReadFile(result.backup_path), custom);
  EXPECT_TRUE(result.database_refreshed);
  EXPECT_EQ(ReadFile(record), paths_.user_file.parent_path().string());

  // Afterwards there is nothing left to clean up.
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_TRUE(inspection.found);
  EXPECT_TRUE(inspection.env_assignments.empty());
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kNone);
  EXPECT_FALSE(ApplyDesktopEntryCleanup(inspection, options_, &result,
                                        &error));
}

TEST_F(LauncherDesktopEntryCleanupTest, KeepsArgumentsAndQuotedAssignments) {
  const std::string entry = Replace(
      kUserEntry, "Exec=env SDL_VIDEODRIVER=wayland mocktail %u",
      "Exec=env MOCKTAIL_NATIVE_LOGIN=0 \"MOCKTAIL_WIN_TITLE=Roblox \\\\\\\\ "
      "Beta\" /usr/bin/mocktail --graphics opengl %u");
  WriteFile(paths_.user_file, entry);
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.env_assignments.size(), 2U);
  EXPECT_EQ(inspection.env_assignments[0].name, "MOCKTAIL_NATIVE_LOGIN");
  EXPECT_EQ(inspection.env_assignments[0].value, "0");
  EXPECT_EQ(inspection.env_assignments[1].name, "MOCKTAIL_WIN_TITLE");
  EXPECT_EQ(inspection.env_assignments[1].value, "Roblox \\ Beta");
  EXPECT_EQ(inspection.new_exec, "/usr/bin/mocktail --graphics opengl %u");
  // A different command line is the user's own; it is kept.
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  EXPECT_NE(inspection.rewritten_bytes.find(
                "\nExec=/usr/bin/mocktail --graphics opengl %u\n"),
            std::string::npos);
}

TEST_F(LauncherDesktopEntryCleanupTest, WithoutAPackagedEntryOnlyRewrites) {
  WriteFile(paths_.user_file, kUserEntry);
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_TRUE(inspection.system_path.empty());
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
}

// A hybrid-graphics laptop's shortcut also picks the GPU. Only the variable
// the settings window can take over goes; the rest keep working.
TEST_F(LauncherDesktopEntryCleanupTest, KeepsVariablesTheSettingsDoNotCover) {
  constexpr char kPrimeExec[] =
      "Exec=env DRI_PRIME=1 __NV_PRIME_RENDER_OFFLOAD=1 "
      "SDL_VIDEODRIVER=wayland mocktail %u";
  const std::string entry = Replace(
      kUserEntry, "Exec=env SDL_VIDEODRIVER=wayland mocktail %u", kPrimeExec);
  WriteFile(paths_.user_file, entry);
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.env_assignments.size(), 3U);
  EXPECT_FALSE(inspection.env_assignments[0].managed);
  EXPECT_FALSE(inspection.env_assignments[1].managed);
  EXPECT_TRUE(inspection.env_assignments[2].managed);
  EXPECT_EQ(inspection.new_exec,
            "env DRI_PRIME=1 __NV_PRIME_RENDER_OFFLOAD=1 mocktail %u");
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  EXPECT_EQ(inspection.rewritten_bytes,
            Replace(entry, kPrimeExec,
                    "Exec=env DRI_PRIME=1 __NV_PRIME_RENDER_OFFLOAD=1 "
                    "mocktail %u"));
  DesktopEntryApplyResult result;
  ASSERT_TRUE(ApplyDesktopEntryCleanup(inspection, options_, &result, &error))
      << error;
  EXPECT_EQ(ReadFile(paths_.user_file), inspection.rewritten_bytes);

  // What is left is the user's own, and is not cleaned up again.
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_EQ(inspection.env_assignments.size(), 2U);
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kNone);
  EXPECT_TRUE(inspection.note.empty()) << inspection.note;

  // An assignment that is kept keeps its quoting.
  const std::string quoted = Replace(
      kUserEntry, "Exec=env SDL_VIDEODRIVER=wayland mocktail %u",
      "Exec=env \"LD_PRELOAD=/opt/my overlay.so\" MOCKTAIL_VSYNC=on "
      "MANGOHUD=1 /usr/bin/mocktail --windowed %u");
  WriteFile(paths_.user_file, quoted);
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  EXPECT_EQ(inspection.new_exec,
            "env \"LD_PRELOAD=/opt/my overlay.so\" MANGOHUD=1 "
            "/usr/bin/mocktail --windowed %u");
}

// The Details dialog shows each assignment; these are shown redacted.
TEST_F(LauncherDesktopEntryCleanupTest, MarksValuesThatMayHoldSecrets) {
  WriteFile(paths_.user_file,
            Replace(kUserEntry, "Exec=env SDL_VIDEODRIVER=wayland mocktail %u",
                    "Exec=env MOCKTAIL_ROBLOX_COOKIES=.ROBLOSECURITY=_test "
                    "MOCKTAIL_COOKIE_FILE=/home/player/alt.cookie "
                    "https_proxy=http://player:hunter2@proxy.lan:3128 "
                    "HTTP_PROXY=http://proxy.lan:3128 "
                    "MOCKTAIL_HTTP_PROXY_HOST=player:hunter2@proxy.lan "
                    "STEAM_API_KEY=k GH_TOKEN=t DRI_PRIME=1 "
                    "SDL_VIDEODRIVER=wayland mocktail %u"));
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  const std::vector<std::pair<std::string, bool>> expected = {
      {"MOCKTAIL_ROBLOX_COOKIES", true},
      {"MOCKTAIL_COOKIE_FILE", true},
      {"https_proxy", true},
      {"HTTP_PROXY", false},
      {"MOCKTAIL_HTTP_PROXY_HOST", true},
      {"STEAM_API_KEY", true},
      {"GH_TOKEN", true},
      {"DRI_PRIME", false},
      {"SDL_VIDEODRIVER", false},
  };
  ASSERT_EQ(inspection.env_assignments.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(inspection.env_assignments[index].name, expected[index].first);
    EXPECT_EQ(inspection.env_assignments[index].sensitive,
              expected[index].second)
        << expected[index].first;
  }
  // A session override is not a setting: the shortcut keeps it.
  EXPECT_FALSE(inspection.env_assignments[0].managed);
  EXPECT_TRUE(inspection.env_assignments[4].managed);
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  EXPECT_EQ(inspection.new_exec.rfind(
                "env MOCKTAIL_ROBLOX_COOKIES=.ROBLOSECURITY=_test "
                "MOCKTAIL_COOKIE_FILE=",
                0),
            0U)
      << inspection.new_exec;
  EXPECT_EQ(inspection.new_exec.find("PROXY_HOST"), std::string::npos);
}

TEST_F(LauncherDesktopEntryCleanupTest, LeavesOtherShapesAlone) {
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_FALSE(inspection.found);

  WriteFile(system_file_, PackagedEntry());
  const struct {
    const char* exec;
    bool has_note;
    std::size_t assignments;
  } cases[] = {
      {"Exec=mocktail %u", false, 0},
      {"Exec=env A=1 -u DISPLAY mocktail %u", true, 1},
      {"Exec=env A=1 firefox %u", true, 0},
      {"Exec=env A=1", true, 0},
      {"Exec=env \"A=1 mocktail %u", true, 0},
  };
  for (const auto& entry_case : cases) {
    WriteFile(paths_.user_file,
              Replace(kUserEntry,
                      "Exec=env SDL_VIDEODRIVER=wayland mocktail %u",
                      entry_case.exec));
    ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error))
        << entry_case.exec << ": " << error;
    EXPECT_TRUE(inspection.found);
    EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kNone)
        << entry_case.exec;
    EXPECT_EQ(!inspection.note.empty(), entry_case.has_note)
        << entry_case.exec << ": " << inspection.note;
    EXPECT_EQ(inspection.env_assignments.size(), entry_case.assignments)
        << entry_case.exec;
  }

  WriteFile(paths_.user_file, "not a desktop entry");
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kNone);
  EXPECT_FALSE(inspection.note.empty());

  // A symlinked shortcut is reported but never touched.
  ASSERT_EQ(unlink(paths_.user_file.c_str()), 0);
  const std::filesystem::path target = temporary_.path() / "elsewhere.desktop";
  WriteFile(target, kUserEntry);
  ASSERT_EQ(symlink(target.c_str(), paths_.user_file.c_str()), 0);
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  EXPECT_TRUE(inspection.found);
  EXPECT_EQ(inspection.plan, DesktopEntryCleanupPlan::kNone);
  EXPECT_NE(inspection.note.find("symlink"), std::string::npos);
  EXPECT_EQ(ReadFile(target), kUserEntry);
}

TEST_F(LauncherDesktopEntryCleanupTest,
       RefusesAShortcutChangedSinceInspection) {
  WriteFile(paths_.user_file, kUserEntry);
  WriteFile(system_file_, PackagedEntry());
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kDeleteFile);
  const std::string edited =
      Replace(kUserEntry, "Terminal=false", "Terminal=true ");
  WriteFile(paths_.user_file, edited);
  DesktopEntryApplyResult result;
  EXPECT_FALSE(ApplyDesktopEntryCleanup(inspection, options_, &result, &error));
  EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(paths_.user_file), edited);
  EXPECT_FALSE(std::filesystem::exists(paths_.user_file.string() +
                                       ".mocktail-backup"));
}

TEST_F(LauncherDesktopEntryCleanupTest, KeepsEarlierBackupsAndToleratesTool) {
  WriteFile(paths_.user_file, kUserEntry);
  WriteFile(paths_.user_file.string() + ".mocktail-backup", "older backup");
  DesktopEntryInspection inspection;
  std::string error;
  ASSERT_TRUE(InspectDesktopEntry(paths_, &inspection, &error)) << error;
  ASSERT_EQ(inspection.plan, DesktopEntryCleanupPlan::kRewriteExec);
  options_.database_tool = "/nonexistent/update-desktop-database";
  DesktopEntryApplyResult result;
  ASSERT_TRUE(ApplyDesktopEntryCleanup(inspection, options_, &result, &error))
      << error;
  EXPECT_EQ(result.backup_path,
            paths_.user_file.string() + ".mocktail-backup.1");
  EXPECT_EQ(ReadFile(result.backup_path), kUserEntry);
  EXPECT_EQ(ReadFile(paths_.user_file.string() + ".mocktail-backup"),
            "older backup");
  EXPECT_FALSE(result.database_refreshed);
  EXPECT_FALSE(result.database_warning.empty());
}

}  // namespace
}  // namespace mocktail::launcher
