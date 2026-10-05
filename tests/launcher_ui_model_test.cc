// GTK-free parts of the settings window: the staged config.yaml draft, the
// environment overrides, the machine profile, recommendations and search.

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "launcher_ui/env_overrides.h"
#include "launcher_ui/machine_profile.h"
#include "launcher_ui/recommendations.h"
#include "launcher_ui/roblox_overrides.h"
#include "launcher_ui/search_index.h"
#include "launcher_ui/setting_kinds.h"
#include "launcher_ui/settings_draft.h"
#include "runtime/runtime_config_bootstrap.h"

namespace mocktail::launcher_ui {
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

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_launcher_ui_model_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) path_ = created;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

  std::filesystem::path Write(const std::filesystem::path& relative,
                              std::string_view contents) const {
    const std::filesystem::path file = path_ / relative;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << contents;
    return file;
  }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

// Files in `directory` whose names start with `prefix`.
std::vector<std::filesystem::path> FilesStartingWith(
    const std::filesystem::path& directory, std::string_view prefix) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().rfind(prefix, 0) == 0) {
      files.push_back(entry.path());
    }
  }
  return files;
}

std::filesystem::perms Permissions(const std::filesystem::path& path) {
  return std::filesystem::status(path).permissions() &
         std::filesystem::perms::all;
}

constexpr char kUserConfig[] =
    "version: 1\n"
    "device: pc-windows-11\n"
    "graphics:\n"
    "  # Мой выбор: Vulkan\n"
    "  backend: direct-vulkan\n"
    "  frame_rate_limit: 165\n"
    "  vsync: off\n"
    "window:\n"
    "  width: 1600\n"
    "  height: 900\n"
    "  title: Roblox\n"
    "  high_dpi: true\n";

// ---- SettingsDraft ---------------------------------------------------------

TEST(SettingsDraftTest, StagesChangesUntilSave) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.Write("config.yaml", kUserConfig);
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  EXPECT_FALSE(draft.read_only()) << draft.load_error();
  EXPECT_EQ(draft.Get("graphics.vsync"), "off");
  EXPECT_EQ(draft.Get("window.width"), "1600");
  EXPECT_FALSE(draft.HasChanges());

  std::string error;
  ASSERT_TRUE(
      draft.Set("graphics.vsync", "on", launcher::ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(
      draft.Set("window.width", "1280", launcher::ScalarKind::kInteger, &error))
      << error;
  EXPECT_EQ(draft.Get("graphics.vsync"), "on");
  EXPECT_EQ(draft.Saved("graphics.vsync"), "off");
  EXPECT_TRUE(draft.IsChanged("graphics.vsync"));
  EXPECT_FALSE(draft.IsChanged("graphics.backend"));
  EXPECT_EQ(draft.ChangedKeys(),
            (std::vector<std::string>{"graphics.vsync", "window.width"}));
  EXPECT_TRUE(draft.HasChanges());
  // Nothing on disk yet.
  EXPECT_EQ(ReadFile(file), kUserConfig);

  // Back to the saved value: no longer a change.
  ASSERT_TRUE(draft.Set("window.width", "1600", launcher::ScalarKind::kInteger,
                        &error));
  EXPECT_EQ(draft.ChangedKeys(), (std::vector<std::string>{"graphics.vsync"}));

  ASSERT_TRUE(draft.Save(&error)) << error;
  EXPECT_FALSE(draft.HasChanges());
  EXPECT_TRUE(draft.ChangedKeys().empty());
  EXPECT_EQ(draft.Saved("graphics.vsync"), "on");
  const std::string saved = ReadFile(file);
  EXPECT_NE(saved.find("  vsync: on\n"), std::string::npos);
  // The user's comment survives.
  EXPECT_NE(saved.find("# Мой выбор: Vulkan"), std::string::npos);
  struct stat status = {};
  ASSERT_EQ(stat(file.c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & 0777, 0600U);
  EXPECT_FALSE(draft.ChangedOnDisk());
}

TEST(SettingsDraftTest, DiscardAndReset) {
  SettingsDraft draft;
  draft.LoadBytes(kUserConfig);
  std::string error;
  ASSERT_TRUE(draft.Set("graphics.backend", "opengl",
                        launcher::ScalarKind::kEnum, &error));
  draft.Discard();
  EXPECT_EQ(draft.Get("graphics.backend"), "direct-vulkan");
  EXPECT_FALSE(draft.HasChanges());

  // The template comments frame_rate_limit out: resetting removes it.
  EXPECT_FALSE(draft.TemplateValue("graphics.frame_rate_limit").has_value());
  ASSERT_TRUE(draft.ResetToTemplate("graphics.frame_rate_limit",
                                    launcher::ScalarKind::kInteger, &error))
      << error;
  EXPECT_FALSE(draft.Get("graphics.frame_rate_limit").has_value());
  EXPECT_TRUE(draft.IsChanged("graphics.frame_rate_limit"));
  // window.width has a template value.
  EXPECT_EQ(draft.TemplateValue("window.width"), "1280");
  ASSERT_TRUE(draft.ResetToTemplate("window.width",
                                    launcher::ScalarKind::kInteger, &error));
  EXPECT_EQ(draft.Get("window.width"), "1280");
  EXPECT_TRUE(draft.Validate(&error)) << error;
}

TEST(SettingsDraftTest, BrokenFileIsReadOnlyWithTheLine) {
  SettingsDraft draft;
  draft.LoadBytes("version: 1\ngraphics:\n  vsync: sometimes\n");
  EXPECT_TRUE(draft.read_only());
  EXPECT_NE(draft.load_error().find("vsync"), std::string::npos)
      << draft.load_error();
  EXPECT_EQ(draft.load_error_line(), 3) << draft.load_error();
  // Rows show defaults.
  EXPECT_EQ(draft.Get("graphics.backend"), "direct-vulkan");
  EXPECT_FALSE(draft.Get("graphics.vsync").has_value());
  std::string error;
  EXPECT_FALSE(
      draft.Set("graphics.vsync", "on", launcher::ScalarKind::kEnum, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(draft.HasChanges());

  draft.LoadBytes("graphics: [\n");
  EXPECT_TRUE(draft.read_only());
  EXPECT_GT(draft.load_error_line(), 0) << draft.load_error();
}

TEST(SettingsDraftTest, TellsAnEmptyFileFromABrokenOne) {
  SettingsDraft draft;
  // The loader refuses both: no root mapping.
  draft.LoadBytes("");
  EXPECT_TRUE(draft.read_only());
  EXPECT_TRUE(draft.file_is_blank());
  draft.LoadBytes(" \n\t\n");
  EXPECT_TRUE(draft.read_only());
  EXPECT_TRUE(draft.file_is_blank());
  // Comments are the user's: not blank.
  draft.LoadBytes("# my settings\n");
  EXPECT_TRUE(draft.read_only());
  EXPECT_FALSE(draft.file_is_blank());
  draft.LoadBytes("graphics: [\n");
  EXPECT_FALSE(draft.file_is_blank());
  draft.LoadBytes(kUserConfig);
  EXPECT_FALSE(draft.read_only());
  EXPECT_FALSE(draft.file_is_blank());

  // An empty file on disk can be replaced by the template.
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write("config.yaml", "");
  ASSERT_TRUE(draft.Load(file));
  ASSERT_TRUE(draft.file_is_blank());
  std::string error;
  ASSERT_TRUE(draft.RestoreBytes(
      std::string(runtime::DefaultRuntimeConfigYaml()), &error))
      << error;
  EXPECT_FALSE(draft.read_only());
  EXPECT_FALSE(draft.file_is_blank());
}

TEST(SettingsDraftTest, RestoresABackupOverABrokenFile) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(
      "config.yaml", "version: 1\ngraphics:\n  vsync: sometimes\n");
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  ASSERT_TRUE(draft.read_only());
  std::string error;
  EXPECT_FALSE(draft.RestoreBytes("graphics: [\n", &error));
  ASSERT_TRUE(draft.RestoreBytes(kUserConfig, &error)) << error;
  EXPECT_FALSE(draft.read_only());
  EXPECT_EQ(draft.Get("graphics.vsync"), "off");
  EXPECT_EQ(ReadFile(file), kUserConfig);
}

// Restore Backup writes the copy kept before the window first saved over
// config.yaml, possibly weeks old. The broken file it replaces still holds
// the user's later edits and comments, so it is kept whole next to it.
TEST(SettingsDraftTest, KeepsTheFileARestoreReplaces) {
  TemporaryDirectory temporary;
  const std::string edited =
      std::string(kUserConfig) + "# Мои заметки\nnetwork: [\n";
  const std::filesystem::path file = temporary.Write("config.yaml", edited);
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  ASSERT_TRUE(draft.read_only());
  std::string error;
  ASSERT_TRUE(draft.RestoreBytes(kUserConfig, &error)) << error;
  EXPECT_EQ(ReadFile(file), kUserConfig);
  const std::vector<std::filesystem::path> kept =
      FilesStartingWith(temporary.path(), "config.yaml.before-restore-");
  ASSERT_EQ(kept.size(), 1U);
  EXPECT_EQ(ReadFile(kept.front()), edited);
  EXPECT_EQ(Permissions(kept.front()), std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);

  // An empty file holds nothing to keep (Use Defaults).
  TemporaryDirectory empty;
  const std::filesystem::path blank = empty.Write("config.yaml", "\n");
  ASSERT_TRUE(draft.Load(blank));
  ASSERT_TRUE(draft.RestoreBytes(
      std::string(runtime::DefaultRuntimeConfigYaml()), &error))
      << error;
  EXPECT_TRUE(FilesStartingWith(empty.path(), "config.yaml.before-").empty());
}

// Reset All replaces the whole file on Save. The one-time launcher backup
// is from before the window first saved; the file as it is now is kept.
TEST(SettingsDraftTest, KeepsTheFileAResetReplaces) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.Write("config.yaml", kUserConfig);
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  std::string error;
  ASSERT_TRUE(draft.Set("graphics.vsync", "on", launcher::ScalarKind::kEnum,
                        &error));
  ASSERT_TRUE(draft.Save(&error)) << error;
  const std::string before_reset = ReadFile(file);
  ASSERT_TRUE(draft.ReplaceAll(std::string(runtime::DefaultRuntimeConfigYaml()),
                               &error));
  ASSERT_TRUE(draft.Save(&error)) << error;
  EXPECT_EQ(ReadFile(file), runtime::DefaultRuntimeConfigYaml());
  std::vector<std::filesystem::path> kept =
      FilesStartingWith(temporary.path(), "config.yaml.before-reset-");
  ASSERT_EQ(kept.size(), 1U);
  EXPECT_EQ(ReadFile(kept.front()), before_reset);

  // Later saves of single settings keep nothing more, and neither does a
  // reset that was undone.
  ASSERT_TRUE(draft.Set("graphics.vsync", "off", launcher::ScalarKind::kEnum,
                        &error));
  ASSERT_TRUE(draft.Save(&error)) << error;
  ASSERT_TRUE(draft.ReplaceAll(kUserConfig, &error));
  draft.Discard();
  ASSERT_TRUE(draft.Set("graphics.vsync", "on", launcher::ScalarKind::kEnum,
                        &error));
  ASSERT_TRUE(draft.Save(&error)) << error;
  kept = FilesStartingWith(temporary.path(), "config.yaml.before-");
  EXPECT_EQ(kept.size(), 1U);
}

TEST(SettingsDraftTest, NoticesAFileChangedOnDisk) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.Write("config.yaml", kUserConfig);
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  EXPECT_FALSE(draft.ChangedOnDisk());
  std::string error;
  ASSERT_TRUE(
      draft.Set("graphics.vsync", "on", launcher::ScalarKind::kEnum, &error));
  // Someone else edits the file: Save refuses, Reload picks it up.
  temporary.Write("config.yaml",
                  std::string(kUserConfig) + "# edited elsewhere\n");
  EXPECT_TRUE(draft.ChangedOnDisk());
  EXPECT_FALSE(draft.Save(&error));
  ASSERT_TRUE(draft.Reload(&error)) << error;
  EXPECT_FALSE(draft.ChangedOnDisk());
  EXPECT_EQ(draft.Get("graphics.vsync"), "off");
  EXPECT_FALSE(draft.HasChanges());
}

TEST(SettingsDraftTest, MissingFileIsCreatedFromTheTemplate) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.path() / "config.yaml";
  SettingsDraft draft;
  ASSERT_TRUE(draft.Load(file));
  EXPECT_FALSE(draft.read_only()) << draft.load_error();
  std::string error;
  ASSERT_TRUE(draft.Set("launcher.show_on_start", "false",
                        launcher::ScalarKind::kBool, &error));
  ASSERT_TRUE(draft.Save(&error)) << error;
  EXPECT_NE(ReadFile(file).find("show_on_start: false"), std::string::npos);
}

TEST(SettingsDraftTest, FindsLineNumbersInMessages) {
  EXPECT_EQ(LineNumberFromMessage("invalid YAML at line 12, column 3"), 12);
  EXPECT_EQ(LineNumberFromMessage("bad value (line 7)"), 7);
  EXPECT_EQ(LineNumberFromMessage("timeline 4"), 0);
  EXPECT_EQ(LineNumberFromMessage("no number"), 0);
  EXPECT_EQ(LineNumberFromMessage("line 0"), 0);
}

TEST(SettingKindsTest, WritesEachKeyAsTheLoaderReadsIt) {
  using launcher::ScalarKind;
  EXPECT_EQ(ScalarKindFor("window.high_dpi", "true"), ScalarKind::kBool);
  EXPECT_EQ(ScalarKindFor("window.width", "1600"), ScalarKind::kInteger);
  EXPECT_EQ(ScalarKindFor("window.title", "1600"), ScalarKind::kString);
  EXPECT_EQ(ScalarKindFor("graphics.frame_rate_limit", "-1"),
            ScalarKind::kInteger);
  EXPECT_EQ(ScalarKindFor("graphics.frame_rate_limit", "display"),
            ScalarKind::kEnum);
  EXPECT_EQ(ScalarKindFor("engine.graphics_quality", "7"),
            ScalarKind::kInteger);
  EXPECT_EQ(ScalarKindFor("audio.output_device", "default"),
            ScalarKind::kString);
  EXPECT_EQ(ScalarKindFor("integrations.discord_rpc.application_id",
                          "123456789012345678"),
            ScalarKind::kString);
  EXPECT_EQ(ScalarKindFor("graphics.backend", "direct-vulkan"),
            ScalarKind::kEnum);
  EXPECT_EQ(ScalarKindFor("engine.gpu", "integrated"), ScalarKind::kEnum);
  EXPECT_EQ(ScalarKindFor("engine.nvidia_shader_mt", "false"),
            ScalarKind::kBool);
  EXPECT_TRUE(IsTextKey("integrations.discord_rpc.text.playing"));
  EXPECT_FALSE(IsTextKey("graphics.vsync"));
}

// ---- EnvOverrides ----------------------------------------------------------

TEST(EnvOverridesTest, ReadsTheNamesMocktailPassed) {
  const MapEnvironment environment({
      {"MOCKTAIL_LAUNCHER_ENV_OVERRIDES",
       "MOCKTAIL_DISPLAY_SERVER,SDL_VIDEODRIVER,MOCKTAIL_VSYNC,NOT_MANAGED,"
       "MOCKTAIL_THEME,MOCKTAIL_VSYNC"},
      {"MOCKTAIL_DISPLAY_SERVER", "x11"},
      {"SDL_VIDEODRIVER", "wayland"},
      {"MOCKTAIL_VSYNC", "sometimes"},
      {"NOT_MANAGED", "1"},
      // MOCKTAIL_THEME was named but is not set any more: dropped.
  });
  const EnvOverrides overrides = EnvOverrides::FromEnvironment(environment);
  ASSERT_EQ(overrides.all().size(), 3U);
  EXPECT_EQ(overrides.SettingCount(), 2);
  // SDL_VIDEODRIVER wins at startup over MOCKTAIL_DISPLAY_SERVER.
  const EnvOverride* display = overrides.Effective("display.server");
  ASSERT_NE(display, nullptr);
  EXPECT_EQ(display->name, "SDL_VIDEODRIVER");
  EXPECT_EQ(display->imported, "wayland");
  EXPECT_FALSE(display->shadowed);
  const std::vector<const EnvOverride*> both =
      overrides.ForKey("display.server");
  ASSERT_EQ(both.size(), 2U);
  EXPECT_EQ(both[1]->name, "MOCKTAIL_DISPLAY_SERVER");
  EXPECT_TRUE(both[1]->shadowed);
  // A value the runtime would reject has no config.yaml form.
  const EnvOverride* vsync = overrides.Effective("graphics.vsync");
  ASSERT_NE(vsync, nullptr);
  EXPECT_FALSE(vsync->imported.has_value());
  EXPECT_EQ(overrides.Effective("appearance.theme"), nullptr);

  EXPECT_TRUE(EnvOverrides::FromEnvironment(MapEnvironment()).empty());
}

TEST(EnvOverridesTest, MovesValuesIntoTheDraft) {
  SettingsDraft draft;
  draft.LoadBytes(kUserConfig);
  const MapEnvironment environment({
      {"SDL_VIDEODRIVER", "wayland"},
      {"MOCKTAIL_DISPLAY_SERVER", "x11"},
      {"MOCKTAIL_NATIVE_LOGIN", "0"},
      {"MOCKTAIL_WIN_TITLE", "Roblox: Mocktail #1"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "display"},
      {"MOCKTAIL_VSYNC", "sometimes"},
  });
  const EnvOverrides overrides = EnvOverrides::FromNames(
      "SDL_VIDEODRIVER,MOCKTAIL_DISPLAY_SERVER,MOCKTAIL_NATIVE_LOGIN,"
      "MOCKTAIL_WIN_TITLE,MOCKTAIL_FRAME_RATE_LIMIT,MOCKTAIL_VSYNC",
      environment);
  const EnvImportReport report = ImportEnvOverrides(overrides, &draft);
  EXPECT_TRUE(report.errors.empty())
      << (report.errors.empty() ? "" : report.errors.front());
  EXPECT_EQ(report.imported.size(), 4U);
  EXPECT_EQ(report.skipped.size(), 2U);
  EXPECT_EQ(draft.Get("display.server"), "wayland");
  EXPECT_EQ(draft.Get("account.sign_in"), "browser");
  EXPECT_EQ(draft.Get("window.title"), "Roblox: Mocktail #1");
  EXPECT_EQ(draft.Get("graphics.frame_rate_limit"), "display");
  EXPECT_EQ(draft.Get("graphics.vsync"), "off");
  std::string error;
  EXPECT_TRUE(draft.Validate(&error)) << error;
}

// engine.gpu and engine.nvidia_shader_mt have no row, but their variables
// still count as overrides and move into an engine: section the draft adds.
TEST(EnvOverridesTest, MovesTheEngineVariablesIntoTheDraft) {
  SettingsDraft draft;
  draft.LoadBytes(kUserConfig);
  const MapEnvironment environment({
      {"MOCKTAIL_GPU", "integrated"},
      {"MOCKTAIL_NVIDIA_SHADER_MT", "off"},
  });
  const EnvOverrides overrides = EnvOverrides::FromNames(
      "MOCKTAIL_GPU,MOCKTAIL_NVIDIA_SHADER_MT", environment);
  EXPECT_EQ(overrides.SettingCount(), 2);
  const EnvOverride* gpu = overrides.Effective("engine.gpu");
  ASSERT_NE(gpu, nullptr);
  EXPECT_EQ(gpu->imported, "integrated");
  const EnvOverride* shader = overrides.Effective("engine.nvidia_shader_mt");
  ASSERT_NE(shader, nullptr);
  EXPECT_EQ(shader->imported, "false");

  const EnvImportReport report = ImportEnvOverrides(overrides, &draft);
  EXPECT_TRUE(report.errors.empty())
      << (report.errors.empty() ? "" : report.errors.front());
  EXPECT_EQ(report.imported.size(), 2U);
  EXPECT_EQ(draft.Get("engine.gpu"), "integrated");
  EXPECT_EQ(draft.Get("engine.nvidia_shader_mt"), "false");
  // Plain YAML, as the template writes them.
  const std::string& bytes = draft.working_bytes();
  EXPECT_NE(bytes.find("\n  gpu: integrated\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("\n  nvidia_shader_mt: false\n"), std::string::npos)
      << bytes;
  std::string error;
  EXPECT_TRUE(draft.Validate(&error)) << error;
}

// After "Move into settings", Discard and Reload drop the moved values
// again; only while the draft still holds them may mocktail leave the
// variables out of the launch.
TEST(EnvOverridesTest, TellsWhetherTheDraftStillHoldsTheMovedValues) {
  SettingsDraft draft;
  draft.LoadBytes(kUserConfig);
  const MapEnvironment environment({
      {"MOCKTAIL_WIN_TITLE", "Roblox: Mocktail #1"},
      // No config.yaml form: only left out of the launch, never moved.
      {"SDL_VIDEODRIVER", "kmsdrm"},
  });
  const EnvOverrides overrides = EnvOverrides::FromNames(
      "MOCKTAIL_WIN_TITLE,SDL_VIDEODRIVER", environment);
  EXPECT_FALSE(DraftHoldsEnvOverrides(overrides, draft));
  ImportEnvOverrides(overrides, &draft);
  EXPECT_TRUE(DraftHoldsEnvOverrides(overrides, draft));
  draft.Discard();
  EXPECT_FALSE(DraftHoldsEnvOverrides(overrides, draft));
  EXPECT_TRUE(DraftHoldsEnvOverrides(EnvOverrides(), draft));
}

TEST(EnvOverridesTest, RedactsCredentialsInUrls) {
  EXPECT_EQ(RedactEnvironmentValue("http://user:secret@proxy:8080/x"),
            "http://***@proxy:8080/x");
  EXPECT_EQ(RedactEnvironmentValue("wayland"), "wayland");
  EXPECT_EQ(RedactEnvironmentValue("http://proxy/a@b"), "http://proxy/a@b");
}

// ---- MachineProfile --------------------------------------------------------

class MachineProfileTest : public ::testing::Test {
 protected:
  MachineProbe Probe() const {
    MachineProbe probe;
    probe.root = root_.path();
    probe.icd_directories = {"/usr/share/vulkan/icd.d"};
    probe.library_directories = {"/usr/lib"};
    probe.physical_cores = [] { return 8; };
    probe.logical_cpus = [] { return 16; };
    return probe;
  }

  void AddGpu(int card, std::string_view vendor) {
    root_.Write("sys/class/drm/card" + std::to_string(card) + "/device/vendor",
                std::string(vendor) + "\n");
  }

  // A card whose device links into a PCI tree under sys/devices, as
  // /sys/class/drm/cardN/device does; the topology tells integrated
  // graphics from discrete cards (graphics_launch_policy.cc).
  void AddPciGpu(int card, const std::string& pci_path, std::string_view vendor,
                 std::string_view device) {
    const std::string function = "sys/devices/" + pci_path;
    root_.Write(function + "/vendor", std::string(vendor) + "\n");
    root_.Write(function + "/device", std::string(device) + "\n");
    const std::filesystem::path card_directory =
        root_.path() / ("sys/class/drm/card" + std::to_string(card));
    std::filesystem::create_directories(card_directory);
    std::filesystem::create_directory_symlink(root_.path() / function,
                                              card_directory / "device");
  }

  TemporaryDirectory root_;
};

TEST_F(MachineProfileTest, DetectsAnNvidiaWaylandLaptop) {
  AddGpu(1, "0x10de");
  root_.Write("proc/driver/nvidia/version", "NVRM version: 615.71\n");
  root_.Write("proc/meminfo", "MemTotal:       15728640 kB\n");
  root_.Write("usr/share/vulkan/icd.d/nvidia_icd.json", "{}");
  root_.Write("usr/share/vulkan/icd.d/radeon_icd.x86_64.json", "{}");
  root_.Write("usr/lib/libgamemode.so.0", "");
  root_.Write("usr/lib/electron43/libEGL.so", "egl");
  root_.Write("usr/lib/electron43/libGLESv2.so",
              std::string(5000, 'x') + "EGL_ANGLE_platform_angle_vulkan");
  root_.Write("usr/lib/electron39/libEGL.so", "egl");
  root_.Write("usr/lib/electron39/libGLESv2.so", "gles");

  const MapEnvironment environment({
      {"XDG_CURRENT_DESKTOP", "Hyprland"},
      {"XDG_SESSION_TYPE", "wayland"},
      {"WAYLAND_DISPLAY", "wayland-1"},
      {"XDG_RUNTIME_DIR", "/run/user/1000"},
      {"DISPLAY", ":0"},
  });
  const MachineProfile profile = DetectMachineProfile(environment, Probe());
  EXPECT_TRUE(profile.detected);
  EXPECT_TRUE(profile.gpu.nvidia);
  EXPECT_FALSE(profile.gpu.amd);
  EXPECT_TRUE(profile.gpu.nvidia_kernel_driver);
  EXPECT_EQ(profile.GpuVendorsLabel(), "NVIDIA");
  EXPECT_EQ(profile.desktop, "Hyprland");
  EXPECT_EQ(profile.session, SessionType::kWayland);
  EXPECT_TRUE(profile.wayland_available);
  EXPECT_TRUE(profile.x11_available);
  EXPECT_EQ(profile.physical_cores, 8);
  EXPECT_EQ(profile.logical_cpus, 16);
  EXPECT_EQ(profile.memory_bytes, 15728640ULL * 1024ULL);
  EXPECT_TRUE(profile.gamemode_library);
  ASSERT_TRUE(profile.angle.has_value());
  EXPECT_EQ(profile.angle->directory, "/usr/lib/electron43");
  EXPECT_EQ(profile.angle->label, "Electron 43");
  EXPECT_TRUE(profile.angle->looks_like_angle);
  EXPECT_FALSE(profile.angle->bundled);
  EXPECT_NE(profile.VulkanDriver("auto").icd.find("nvidia_icd.json"),
            std::string::npos);
  EXPECT_EQ(profile.VulkanDriver("auto").FileNames(), "nvidia_icd.json");
  EXPECT_TRUE(profile.has_vulkan_driver());
  EXPECT_EQ(profile.gpu.nvidia_driver_version, "615.71");
  // NVIDIA + direct Vulkan + both displays: the NVIDIA rule decides, and
  // without an answer from the compositor it keeps XWayland.
  EXPECT_EQ(profile.wayland_explicit_sync,
            window::WaylandExplicitSync::kUnknown);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(false),
            window::NvidiaWaylandBlocker::kExplicitSyncUnknown);
  EXPECT_TRUE(profile.NvidiaDirectVulkanUsesX11("auto", false));
  EXPECT_TRUE(profile.NvidiaDirectVulkanUsesX11("auto", true));
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", false),
            "x11");
  EXPECT_EQ(profile.AutomaticDisplayServer("vulkan", "auto", false), "x11");
  EXPECT_EQ(profile.AutomaticDisplayServer("opengl", "auto", false),
            "wayland");
  EXPECT_EQ(profile.AutomaticDisplayServer("angle-vulkan", "auto", false),
            "wayland");
  EXPECT_EQ(RecommendGraphicsBackend(profile).value, "direct-vulkan");
  EXPECT_EQ(RecommendGraphicsBackend(profile).reason,
            BackendRecommendationReason::kVulkanDriver);
}

TEST_F(MachineProfileTest, DetectsAMachineWithoutVulkan) {
  AddGpu(0, "0x8086");
  const MachineProfile profile = DetectMachineProfile(
      MapEnvironment({{"DISPLAY", ":1"}, {"XDG_CURRENT_DESKTOP", "XFCE"}}),
      Probe());
  EXPECT_TRUE(profile.gpu.intel);
  EXPECT_TRUE(profile.gpu.integrated_only());
  // Without a Vulkan driver no card is picked, and an Intel-only computer
  // counts as Intel integrated graphics.
  EXPECT_TRUE(profile.RendersOnIntelIntegratedGraphics("auto"));
  EXPECT_FALSE(profile.gpu.nvidia_kernel_driver);
  EXPECT_EQ(profile.session, SessionType::kX11);
  EXPECT_FALSE(profile.wayland_available);
  EXPECT_EQ(profile.desktop, "Xfce");
  EXPECT_FALSE(profile.has_vulkan_driver());
  EXPECT_FALSE(profile.angle.has_value());
  EXPECT_FALSE(profile.gamemode_library);
  EXPECT_FALSE(profile.NvidiaDirectVulkanUsesX11("auto", false));
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", false),
            "x11");
  EXPECT_EQ(RecommendGraphicsBackend(profile).value, "opengl");
  EXPECT_EQ(RecommendGraphicsBackend(profile).reason,
            BackendRecommendationReason::kNoVulkanDriver);
  EXPECT_EQ(RecommendGraphicsBackend(MachineProfile{}).reason,
            BackendRecommendationReason::kUnknown);
}

// A Vulkan driver manifest in the loader's format naming `library`.
std::string VulkanManifest(std::string_view library) {
  return "{\"file_format_version\": \"1.0.1\", \"ICD\": {\"library_path\": \"" +
         std::string(library) + "\", \"api_version\": \"1.4.328\"}}\n";
}

// window.cc ResolveNvidiaWaylandEvidence: native Wayland for NVIDIA's
// direct Vulkan with driver 555 or newer, the commit guard, NVIDIA alone, a
// compositor that offers explicit sync, and Hyprland or frames that do not
// wait for the display; XWayland otherwise.
TEST_F(MachineProfileTest, FollowsTheNvidiaWaylandRule) {
  AddGpu(1, "0x10de");
  root_.Write("proc/driver/nvidia/version",
              "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  "
              "615.71.09  Release Build\nGCC version:  gcc 16.1.1\n");
  root_.Write("usr/share/vulkan/icd.d/nvidia_icd.json",
              VulkanManifest("libGLX_nvidia.so.0"));
  std::unordered_map<std::string, std::string> variables = {
      {"WAYLAND_DISPLAY", "wayland-1"},
      {"XDG_RUNTIME_DIR", "/run/user/1000"},
      {"DISPLAY", ":0"},
  };
  int probes = 0;
  window::WaylandGlobals globals;
  globals.listed = true;
  globals.drm_syncobj = true;
  MachineProbe probe = Probe();
  probe.wayland_globals = [&probes, &globals] {
    ++probes;
    return globals;
  };

  // KWin or GNOME: explicit sync, but not Hyprland. Native Wayland only
  // for frames that do not wait for the display.
  MachineProfile profile =
      DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(probes, 1);
  EXPECT_EQ(profile.gpu.nvidia_driver_version, "615.71.09");
  EXPECT_EQ(profile.wayland_explicit_sync,
            window::WaylandExplicitSync::kOffered);
  EXPECT_FALSE(profile.hyprland_compositor);
  EXPECT_TRUE(profile.NvidiaRuleApplies("direct-vulkan", "auto"));
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kNone);
  EXPECT_FALSE(profile.NvidiaDirectVulkanUsesX11("auto", true));
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", true),
            "wayland");
  DisplayServerChoice choice = ResolveDisplayServer(
      profile, "auto", "direct-vulkan", "auto", Presentation::kUnthrottled);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanWayland);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(false),
            window::NvidiaWaylandBlocker::kVsyncOutsideHyprland);
  EXPECT_TRUE(profile.NvidiaDirectVulkanUsesX11("auto", false));
  for (const Presentation presentation :
       {Presentation::kSynchronized, Presentation::kDriverDefault}) {
    choice = ResolveDisplayServer(profile, "auto", "direct-vulkan", "auto",
                                  presentation);
    EXPECT_EQ(choice.server, "x11");
    EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);
  }
  // display.server: wayland still gets Wayland, with the risk named.
  choice = ResolveDisplayServer(profile, "wayland", "direct-vulkan", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kChosen);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(false, true),
            window::NvidiaWaylandBlocker::kVsyncOutsideHyprland);

  // Hyprland: native Wayland whatever the presentation.
  globals.hyprland = true;
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_TRUE(profile.hyprland_compositor);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(false),
            window::NvidiaWaylandBlocker::kNone);
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", false),
            "wayland");
  choice = ResolveDisplayServer(profile, "auto", "direct-vulkan", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanWayland);

  // A compositor without explicit sync, or none that answers.
  globals.drm_syncobj = false;
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kCompositorWithoutExplicitSync);
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", true),
            "x11");
  choice = ResolveDisplayServer(profile, "auto", "direct-vulkan", "auto",
                                Presentation::kUnthrottled);
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);
  globals.listed = false;
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kExplicitSyncUnknown);
  EXPECT_FALSE(profile.hyprland_compositor);
  globals.listed = true;
  globals.drm_syncobj = true;
  globals.hyprland = false;

  // __NV_DISABLE_EXPLICIT_SYNC: XWayland, without asking the compositor.
  probes = 0;
  variables["__NV_DISABLE_EXPLICIT_SYNC"] = "1";
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(probes, 0);
  EXPECT_TRUE(profile.nvidia_explicit_sync_disabled);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kExplicitSyncDisabled);
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", true),
            "x11");
  variables["__NV_DISABLE_EXPLICIT_SYNC"] = "0";
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_FALSE(profile.nvidia_explicit_sync_disabled);
  EXPECT_EQ(probes, 1);
  variables.erase("__NV_DISABLE_EXPLICIT_SYNC");

  // The commit guard turned off: XWayland, without asking the compositor.
  probes = 0;
  variables["MOCKTAIL_WAYLAND_COMMIT_GUARD"] = "off";
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(probes, 0);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kCommitGuardOff);
  variables.erase("MOCKTAIL_WAYLAND_COMMIT_GUARD");

  // The Wayland preference off: XWayland under Automatic; display.server:
  // wayland would still have everything it needs.
  variables["MOCKTAIL_PREFER_WAYLAND"] = "0";
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kWaylandNotPreferred);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true, true),
            window::NvidiaWaylandBlocker::kNone);
  variables.erase("MOCKTAIL_PREFER_WAYLAND");

  // A driver older than 555, read from /sys/module when /proc does not say.
  probes = 0;
  root_.Write("proc/driver/nvidia/version", "NVRM version: unknown\n");
  root_.Write("sys/module/nvidia/version", "550.144.03\n");
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(probes, 0);
  EXPECT_EQ(profile.gpu.nvidia_driver_version, "550.144.03");
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kDriverWithoutExplicitSync);
  root_.Write("sys/module/nvidia/version", "560.35.03\n");

  // Intel graphics beside the NVIDIA card: XWayland, unless engine.gpu puts
  // the game on the Intel card, whose manifest names ANV: the NVIDIA rule
  // does not apply there.
  AddGpu(0, "0x8086");
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json",
              VulkanManifest("libvulkan_intel.so"));
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_EQ(profile.NvidiaNativeWaylandBlocker(true),
            window::NvidiaWaylandBlocker::kOtherGpu);
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan", "auto", true),
            "x11");
  EXPECT_FALSE(profile.NvidiaRuleApplies("direct-vulkan", "integrated"));
  EXPECT_EQ(
      profile.AutomaticDisplayServer("direct-vulkan", "integrated", false),
      "wayland");
  choice = ResolveDisplayServer(profile, "", "direct-vulkan", "integrated",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.reason, DisplayServerReason::kWaylandSession);

  // A manifest that does not say which driver it loads may be NVIDIA's.
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{}");
  profile = DetectMachineProfile(MapEnvironment(variables), probe);
  EXPECT_TRUE(profile.NvidiaRuleApplies("direct-vulkan", "integrated"));
  EXPECT_EQ(
      profile.AutomaticDisplayServer("direct-vulkan", "integrated", true),
      "x11");
}

TEST_F(MachineProfileTest, TellsIntegratedGraphicsByTheirPciPlace) {
  // A Zen APU behind the root bus's device-8 bridge and Intel's integrated
  // graphics at 00:02.0.
  AddPciGpu(0, "pci0000:00/0000:00:08.1/0000:05:00.0", "0x1002", "0x1681");
  AddPciGpu(1, "pci0000:00/0000:00:02.0", "0x8086", "0x9a49");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  ASSERT_EQ(profile.gpu.cards.size(), 2U);
  EXPECT_TRUE(profile.gpu.cards[0].integrated);
  EXPECT_EQ(profile.gpu.cards[0].pci_address, "0000:05:00.0");
  EXPECT_FALSE(profile.gpu.amd_discrete);
  EXPECT_FALSE(profile.gpu.intel_discrete);
  EXPECT_TRUE(profile.gpu.integrated_only());
  EXPECT_EQ(GpuCardName(profile.gpu.cards[1]), "Intel");
}

TEST_F(MachineProfileTest, TellsDiscreteCardsByTheirPciPlace) {
  // A Radeon behind a PCIe switch and an Arc card behind a PCIe port.
  AddPciGpu(0, "pci0000:00/0000:00:01.1/0000:0a:00.0/0000:0b:00.0/0000:0c:00.0",
            "0x1002", "0x73df");
  AddPciGpu(1, "pci0000:00/0000:00:01.0/0000:01:00.0/0000:02:01.0/0000:03:00.0",
            "0x8086", "0x56a0");
  const MachineProfile profile =
      DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_TRUE(profile.gpu.amd_discrete);
  EXPECT_TRUE(profile.gpu.intel_discrete);
  EXPECT_FALSE(profile.gpu.integrated_only());
  EXPECT_EQ(profile.DiscreteGpuLabel(), "AMD + Intel Arc");
  EXPECT_EQ(GpuCardName(profile.gpu.cards[1]), "Intel Arc");
}

TEST_F(MachineProfileTest, FollowsTheVulkanLoader) {
  AddGpu(0, "0x1002");
  // AMDVLK only: Mocktail pins nothing, the loader still finds it.
  root_.Write("usr/share/vulkan/icd.d/amd_icd64.json", "{}");
  root_.Write("usr/share/vulkan/icd.d/lvp_icd.x86_64.json", "{}");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(profile.vulkan_source, VulkanDriverSource::kLoader);
  EXPECT_NE(profile.VulkanDriver("auto").icd.find("amd_icd64.json"),
            std::string::npos);
  EXPECT_FALSE(profile.VulkanDriver("auto").gpu.has_value());
  EXPECT_TRUE(profile.has_vulkan_driver());
  EXPECT_EQ(RecommendGraphicsBackend(profile).value, "direct-vulkan");

  // A driver list the user set is kept as it is.
  profile = DetectMachineProfile(
      MapEnvironment({{"VK_DRIVER_FILES", "/opt/vk/my_icd.json"}}), Probe());
  EXPECT_EQ(profile.vulkan_source, VulkanDriverSource::kUser);
  EXPECT_EQ(profile.VulkanDriver("integrated").icd, "/opt/vk/my_icd.json");

  // Without a PCI path an AMD card counts as discrete and an Intel one as
  // integrated. With PRIME offloading off, Mocktail pins the integrated
  // GPU's driver, unless engine.gpu names a kind.
  AddGpu(1, "0x8086");
  root_.Write("usr/share/vulkan/icd.d/radeon_icd.x86_64.json", "{}");
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{}");
  profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_NE(profile.VulkanDriver("auto").icd.find("radeon_icd"),
            std::string::npos);
  profile = DetectMachineProfile(MapEnvironment({{"DRI_PRIME", "0"}}), Probe());
  EXPECT_EQ(profile.vulkan_source, VulkanDriverSource::kPinned);
  EXPECT_NE(profile.VulkanDriver("auto").icd.find("intel_icd"),
            std::string::npos);
  EXPECT_NE(profile.VulkanDriver("discrete").icd.find("radeon_icd"),
            std::string::npos);
}

// A hybrid laptop: engine.gpu (or PRIME offloading under auto) picks the
// card, and with it graphics quality level 1 for Intel integrated graphics
// (graphics_launch_policy.cc ApplyVulkanIcdPolicy).
TEST_F(MachineProfileTest, FollowsEngineGpuOnAHybridLaptop) {
  AddPciGpu(0, "pci0000:00/0000:00:02.0", "0x8086", "0x9a49");
  AddPciGpu(1, "pci0000:00/0000:00:01.0/0000:01:00.0", "0x10de", "0x2520");
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{}");
  root_.Write("usr/share/vulkan/icd.d/nvidia_icd.json", "{}");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_TRUE(profile.gpu.nvidia);
  EXPECT_TRUE(profile.gpu.intel);
  EXPECT_FALSE(profile.gpu.intel_discrete);

  VulkanDriverSelection driver = profile.VulkanDriver("auto");
  ASSERT_TRUE(driver.gpu.has_value());
  EXPECT_EQ(driver.gpu->vendor, kNvidiaPciVendor);
  EXPECT_TRUE(driver.preferred);
  EXPECT_FALSE(profile.RendersOnIntelIntegratedGraphics("auto"));
  EXPECT_FALSE(profile.RendersOnIntelIntegratedGraphics("discrete"));

  driver = profile.VulkanDriver("integrated");
  ASSERT_TRUE(driver.gpu.has_value());
  EXPECT_EQ(driver.gpu->vendor, kIntelPciVendor);
  EXPECT_EQ(driver.FileNames(), "intel_icd.x86_64.json");
  EXPECT_TRUE(profile.RendersOnIntelIntegratedGraphics("integrated"));
  // A value the loader would refuse reads as auto.
  EXPECT_FALSE(profile.RendersOnIntelIntegratedGraphics("igpu"));

  profile = DetectMachineProfile(
      MapEnvironment({{"__NV_PRIME_RENDER_OFFLOAD", "0"}}), Probe());
  EXPECT_EQ(profile.ResolvedGpuPreference("auto"),
            runtime::GpuPreference::kIntegrated);
  EXPECT_TRUE(profile.RendersOnIntelIntegratedGraphics("auto"));
  EXPECT_FALSE(profile.RendersOnIntelIntegratedGraphics("discrete"));

  // Drivers the user pinned stay; VK_LOADER_DEVICE_SELECT names the card.
  profile = DetectMachineProfile(
      MapEnvironment({{"VK_DRIVER_FILES", "/opt/intel_icd.json"},
                      {"VK_LOADER_DEVICE_SELECT", "0x8086:0x9a49"}}),
      Probe());
  EXPECT_EQ(profile.vulkan_source, VulkanDriverSource::kUser);
  ASSERT_TRUE(profile.VulkanDriver("discrete").gpu.has_value());
  EXPECT_TRUE(profile.RendersOnIntelIntegratedGraphics("discrete"));

  // No integrated card with a driver: the NVIDIA card stands in.
  std::filesystem::remove(root_.path() /
                          "usr/share/vulkan/icd.d/intel_icd.x86_64.json");
  profile = DetectMachineProfile(MapEnvironment(), Probe());
  driver = profile.VulkanDriver("integrated");
  ASSERT_TRUE(driver.gpu.has_value());
  EXPECT_EQ(driver.gpu->vendor, kNvidiaPciVendor);
  EXPECT_FALSE(driver.preferred);
}

TEST_F(MachineProfileTest, CannotSeeFlatpakDrivers) {
  AddGpu(0, "0x10de");
  root_.Write(".flatpak-info", "[Application]\n");
  const MachineProfile profile =
      DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_TRUE(profile.flatpak);
  EXPECT_EQ(profile.vulkan_source, VulkanDriverSource::kUnknown);
  EXPECT_FALSE(profile.has_vulkan_driver());
  EXPECT_EQ(RecommendGraphicsBackend(profile).reason,
            BackendRecommendationReason::kUnknown);
}

TEST_F(MachineProfileTest, SkipsLibrariesThatAreNotAngle) {
  root_.Write("usr/lib/chromium/libEGL.so", "egl");
  root_.Write("usr/lib/chromium/libGLESv2.so", "plain gles");
  root_.Write("usr/lib/electron43/libEGL.so", "egl");
  root_.Write("usr/lib/electron43/libGLESv2.so", "EGL_ANGLE_platform_angle");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  ASSERT_TRUE(profile.angle.has_value());
  EXPECT_EQ(profile.angle->directory, "/usr/lib/electron43");

  // MOCKTAIL_ANGLE_LIB_DIR is used as it is set, like the game does.
  profile = DetectMachineProfile(
      MapEnvironment({{"MOCKTAIL_ANGLE_LIB_DIR", "/usr/lib/chromium"}}),
      Probe());
  ASSERT_TRUE(profile.angle.has_value());
  EXPECT_TRUE(profile.angle->from_environment);
  EXPECT_FALSE(profile.angle->looks_like_angle);
  profile = DetectMachineProfile(
      MapEnvironment({{"MOCKTAIL_ANGLE_LIB_DIR", "/srv/missing"}}), Probe());
  EXPECT_FALSE(profile.angle.has_value());
}

TEST_F(MachineProfileTest, FindsTheBundledAngleFirst) {
  root_.Write("opt/mocktail/lib/angle/libEGL.so", "egl");
  root_.Write("opt/mocktail/lib/angle/libGLESv2.so",
              "EGL_ANGLE_platform_angle");
  root_.Write("usr/lib/chromium/libEGL.so", "egl");
  root_.Write("usr/lib/chromium/libGLESv2.so", "EGL_ANGLE_platform_angle");
  const MapEnvironment environment(
      {{"MOCKTAIL_RUNTIME_LIBRARY_DIR", "/opt/mocktail/lib"}});
  const MachineProfile profile = DetectMachineProfile(environment, Probe());
  ASSERT_TRUE(profile.angle.has_value());
  EXPECT_TRUE(profile.angle->bundled);
  EXPECT_EQ(profile.angle->directory, "/opt/mocktail/lib/angle");

  const std::vector<std::filesystem::path> directories = AngleSearchDirectories(
      MapEnvironment({{"MOCKTAIL_ANGLE_LIB_DIR", "/srv/angle"},
                      {"MOCKTAIL_RUNTIME_LIBRARY_DIR", "/usr/lib/mocktail"}}));
  ASSERT_GE(directories.size(), 3U);
  EXPECT_EQ(directories[0], "/srv/angle");
  EXPECT_EQ(directories[1], "/usr/lib/mocktail/angle");
  EXPECT_EQ(directories[2], "/usr/lib64/chromium");
}

TEST(MachineProfileLabelTest, NamesBrowsersAndDesktops) {
  EXPECT_EQ(AngleLibraryLabel("/usr/lib/electron39"), "Electron 39");
  EXPECT_EQ(AngleLibraryLabel("/usr/lib64/chromium-browser"), "Chromium");
  EXPECT_EQ(AngleLibraryLabel("/opt/google/chrome"), "Google Chrome");
  EXPECT_EQ(AngleLibraryLabel("/usr/lib/cef"), "CEF");
  EXPECT_EQ(AngleLibraryLabel("/srv/angle"), "/srv/angle");
  EXPECT_EQ(DescribeDesktop(
                MapEnvironment({{"XDG_CURRENT_DESKTOP", "ubuntu:GNOME"}})),
            "GNOME");
  EXPECT_EQ(DescribeDesktop(MapEnvironment({{"XDG_CURRENT_DESKTOP", "KDE"}})),
            "KDE Plasma");
  EXPECT_EQ(DescribeDesktop(MapEnvironment({{"NIRI_SOCKET", "/run/x"}})),
            "niri");
  EXPECT_EQ(
      DescribeDesktop(MapEnvironment({{"HYPRLAND_INSTANCE_SIGNATURE", "abc"}})),
      "Hyprland");
  EXPECT_EQ(
      DescribeDesktop(MapEnvironment({{"XDG_CURRENT_DESKTOP", "pop:COSMIC"}})),
      "COSMIC");
  EXPECT_EQ(DescribeDesktop(MapEnvironment()), "");

  MonitorInfo monitor;
  monitor.width = 1600;
  monitor.height = 900;
  monitor.scale = 1.6;
  monitor.refresh_millihertz = 165003;
  EXPECT_EQ(monitor.PixelWidth(), 2560);
  EXPECT_EQ(monitor.PixelHeight(), 1440);
  EXPECT_EQ(monitor.RefreshHz(), 165);
}

// ---- SearchIndex -----------------------------------------------------------

TEST(SearchIndexTest, MatchesTitlesKeywordsAndBothLanguages) {
  SearchIndex index;
  const int backend =
      index.Add({"graphics",
                 "Графический движок",
                 "Vulkan",
                 {"graphics.backend", "MOCKTAIL_GRAPHICS_BACKEND", "renderer",
                  "vulkan", "opengl"}});
  const int vsync = index.Add(
      {"graphics",
       "Vertical sync",
       "Lowest input latency",
       {"graphics.vsync", "MOCKTAIL_VSYNC", "вертикальная синхронизация"}});
  const int display = index.Add(
      {"display",
       "Display server",
       "Automatic",
       {"display.server", "SDL_VIDEODRIVER", "wayland", "x11", "xwayland"}});
  EXPECT_EQ(index.Match("vulkan"), (std::vector<int>{backend}));
  EXPECT_EQ(index.Match("ГРАФИЧЕСКИЙ"), (std::vector<int>{backend}));
  EXPECT_EQ(index.Match("sdl_videodriver"), (std::vector<int>{display}));
  EXPECT_EQ(index.Match("синхрон"), (std::vector<int>{vsync}));
  EXPECT_EQ(index.Match("vertical sync"), (std::vector<int>{vsync}));
  EXPECT_TRUE(index.Match("vertical wayland").empty());
  EXPECT_TRUE(index.Match("   ").empty());
  // A title match ranks above a keyword match.
  const int other = index.Add({"display", "Wayland notes", "", {}});
  EXPECT_EQ(index.Match("wayland"), (std::vector<int>{other, display}));
  // ё is е.
  const int yo = index.Add({"audio", "Ещё", "", {}});
  EXPECT_EQ(index.Match("еще"), (std::vector<int>{yo}));
  index.SetSubtitle(vsync, "Самая низкая задержка");
  EXPECT_EQ(index.Match("задержка"), (std::vector<int>{vsync}));
  ASSERT_NE(index.Get(vsync), nullptr);
  EXPECT_EQ(index.Get(vsync)->subtitle, "Самая низкая задержка");
  EXPECT_EQ(index.Get(99), nullptr);
}

// ---- Graphics, Display, Performance and Audio pages -------------------------

MachineProfile DetectedMachine() {
  MachineProfile machine;
  machine.detected = true;
  machine.wayland_available = true;
  machine.x11_available = true;
  machine.monitor.valid = true;
  machine.monitor.width = 1600;
  machine.monitor.height = 900;
  machine.monitor.scale = 1.6;
  machine.monitor.refresh_millihertz = 165003;
  return machine;
}

TEST(GamePagesTest, RenderingPresetFollowsPhysicsWorkers) {
  // performance_policy.cc: throughput (the default) always, auto only with
  // multithreaded rendering, latency never.
  EXPECT_TRUE(RenderingPresetActive("", ""));
  EXPECT_TRUE(RenderingPresetActive("throughput", "false"));
  EXPECT_TRUE(RenderingPresetActive("auto", "true"));
  EXPECT_FALSE(RenderingPresetActive("auto", "false"));
  EXPECT_FALSE(RenderingPresetActive("latency", "true"));
}

TEST(GamePagesTest, ResolvesTheGraphicsQualityLevel) {
  QualityEffect effect = ResolveGraphicsQuality("default", true, false);
  EXPECT_EQ(effect.source, QualitySource::kMocktailDefault);
  EXPECT_EQ(effect.level, 3);
  EXPECT_FALSE(effect.ignored);
  EXPECT_EQ(ResolveGraphicsQuality("", true, true).level, 1);
  effect = ResolveGraphicsQuality("12", true, true);
  EXPECT_EQ(effect.source, QualitySource::kConfiguredLevel);
  EXPECT_EQ(effect.level, 12);
  effect = ResolveGraphicsQuality("manual", true, false);
  EXPECT_EQ(effect.source, QualitySource::kRobloxSetting);
  EXPECT_FALSE(effect.ignored);
  effect = ResolveGraphicsQuality("12", false, false);
  EXPECT_EQ(effect.source, QualitySource::kRobloxSetting);
  EXPECT_TRUE(effect.ignored);
  EXPECT_EQ(ParseQualityLevel("21"), 21);
  EXPECT_FALSE(ParseQualityLevel("22").has_value());
  EXPECT_FALSE(ParseQualityLevel("0").has_value());
  EXPECT_FALSE(ParseQualityLevel("default").has_value());

  MachineProfile machine = DetectedMachine();
  machine.gpu.nvidia = true;
  EXPECT_EQ(RecommendGraphicsQuality(machine).value, "manual");
  machine.gpu = {};
  machine.gpu.intel = true;
  EXPECT_EQ(RecommendGraphicsQuality(machine).value, "default");
  EXPECT_EQ(RecommendGraphicsQuality(machine).reason,
            QualityRecommendationReason::kModestGraphics);
  // An Arc card is discrete; an APU is not.
  machine.gpu.intel_discrete = true;
  EXPECT_EQ(RecommendGraphicsQuality(machine).value, "manual");
  machine.gpu = {};
  machine.gpu.amd = true;
  EXPECT_EQ(RecommendGraphicsQuality(machine).value, "default");
  machine.gpu.amd_discrete = true;
  EXPECT_EQ(RecommendGraphicsQuality(machine).value, "manual");
  // No known GPU, or detection not finished: no recommendation.
  machine.gpu = {};
  machine.gpu.other = true;
  EXPECT_TRUE(RecommendGraphicsQuality(machine).value.empty());
  EXPECT_EQ(RecommendGraphicsQuality(MachineProfile{}).reason,
            QualityRecommendationReason::kUnknown);
}

TEST(GamePagesTest, PresentationMatchesThePresentModePolicy) {
  // present_mode_policy.cc ResolvePresentModePolicy.
  EXPECT_EQ(ResolvePresentation("auto", "-1"), Presentation::kDriverDefault);
  EXPECT_EQ(ResolvePresentation("auto", ""), Presentation::kDriverDefault);
  EXPECT_EQ(ResolvePresentation("auto", "display"),
            Presentation::kSynchronized);
  EXPECT_EQ(ResolvePresentation("auto", "165"), Presentation::kSynchronized);
  EXPECT_EQ(ResolvePresentation("auto", "unlimited"),
            Presentation::kUnthrottled);
  EXPECT_EQ(ResolvePresentation("on", "unlimited"),
            Presentation::kSynchronized);
  EXPECT_EQ(ResolvePresentation("off", "-1"), Presentation::kUnthrottled);
}

TEST(GamePagesTest, RecommendsTheDisplaysFrameRate) {
  MachineProfile machine = DetectedMachine();
  machine.gpu.nvidia = true;
  machine.monitor = {};
  EXPECT_FALSE(RecommendFrameRate(machine).has_value());
  machine.monitor.valid = true;
  machine.monitor.refresh_millihertz = 165003;
  EXPECT_EQ(RecommendFrameRate(machine), "165");
  machine.monitor.refresh_millihertz = 59950;
  EXPECT_EQ(RecommendFrameRate(machine), "-1");
  // Above Roblox's menu maximum, the maximum.
  machine.monitor.refresh_millihertz = 360000;
  EXPECT_EQ(RecommendFrameRate(machine), "unlimited");
  machine.monitor.refresh_millihertz = 240000;
  EXPECT_EQ(RecommendFrameRate(machine), "240");
  // Integrated graphics rarely keep up with a fast screen.
  machine.gpu = {};
  machine.gpu.intel = true;
  machine.monitor.refresh_millihertz = 120000;
  EXPECT_FALSE(RecommendFrameRate(machine).has_value());
  machine.monitor.refresh_millihertz = 60000;
  EXPECT_EQ(RecommendFrameRate(machine), "-1");
  EXPECT_EQ(ParseFrameRate("75"), 75);
  EXPECT_FALSE(ParseFrameRate("-1").has_value());
  EXPECT_FALSE(ParseFrameRate("display").has_value());
}

TEST(GamePagesTest, FindsFastFlagsThatBlockTheStart) {
  launcher::FastFlagsDocument flags;
  std::string error;
  ASSERT_TRUE(launcher::FastFlagsDocument::FromBytes(
      R"({"DFIntTaskSchedulerTargetFps": 144,
          "FIntDebugFRMQualityLevelOverride": "5"})",
      &flags, &error))
      << error;
  // Roblox owns the cap: the target flag is the user's own.
  EXPECT_EQ(FrameRateFlagConflict(flags, "-1"), "");
  EXPECT_EQ(FrameRateFlagConflict(flags, "144"), "");
  EXPECT_EQ(FrameRateFlagConflict(flags, "165"), "DFIntTaskSchedulerTargetFps");
  EXPECT_EQ(FrameRateFlagConflict(flags, "unlimited"),
            "DFIntTaskSchedulerTargetFps");
  EXPECT_EQ(QualityFlagConflict(flags,
                                ResolveGraphicsQuality("default", true, false)),
            "FIntDebugFRMQualityLevelOverride");
  EXPECT_EQ(
      QualityFlagConflict(flags, ResolveGraphicsQuality("5", true, false)), "");
  EXPECT_EQ(
      QualityFlagConflict(flags, ResolveGraphicsQuality("manual", true, false)),
      "");
}

TEST(GamePagesTest, DerivesWindowSizesFromTheMonitor) {
  MonitorInfo monitor;
  monitor.valid = true;
  monitor.width = 1600;
  monitor.height = 900;
  std::vector<WindowSizePreset> presets = WindowSizePresets(monitor);
  ASSERT_GE(presets.size(), 3U);
  EXPECT_TRUE(presets.front().whole_screen);
  EXPECT_EQ(presets.front().size, (WindowSize{1600, 900}));
  bool has_default = false;
  for (const WindowSizePreset& preset : presets) {
    EXPECT_LE(preset.size.width, 1600);
    EXPECT_LE(preset.size.height, 900);
    if (preset.runtime_default) {
      has_default = true;
      EXPECT_EQ(preset.size, (WindowSize{1280, 720}));
    }
  }
  EXPECT_TRUE(has_default);
  // A small screen still offers the default, and an odd one its own size.
  monitor.width = 1201;
  monitor.height = 675;
  presets = WindowSizePresets(monitor);
  EXPECT_EQ(presets.front().size, (WindowSize{1280, 720}));
  EXPECT_TRUE(presets.front().runtime_default);
  EXPECT_EQ(presets[1].size, (WindowSize{1201, 675}));
  EXPECT_TRUE(presets[1].whole_screen);
  EXPECT_EQ(WindowSizePresets(MonitorInfo{}).size(), 4U);
}

TEST(GamePagesTest, ComputesTheGameResolution) {
  MonitorInfo monitor;
  monitor.valid = true;
  monitor.width = 1600;
  monitor.height = 900;
  monitor.scale = 1.6;
  GameResolution resolution = ComputeGameResolution(
      monitor, {1280, 720}, WindowMode::kFullscreen, true, true);
  EXPECT_EQ(resolution.logical, (WindowSize{1600, 900}));
  EXPECT_EQ(resolution.pixels, (WindowSize{2560, 1440}));
  EXPECT_FALSE(resolution.upscaled);
  EXPECT_TRUE(resolution.high_dpi_matters);
  resolution = ComputeGameResolution(monitor, {1280, 720},
                                     WindowMode::kWindowed, false, true);
  EXPECT_EQ(resolution.pixels, (WindowSize{1280, 720}));
  EXPECT_TRUE(resolution.upscaled);
  resolution = ComputeGameResolution(monitor, {1280, 720},
                                     WindowMode::kMaximized, true, true);
  EXPECT_TRUE(resolution.approximate);
  // X11 (XWayland): no pixel density, High-DPI changes nothing.
  resolution = ComputeGameResolution(monitor, {1280, 720},
                                     WindowMode::kWindowed, true, false);
  EXPECT_EQ(resolution.pixels, (WindowSize{1280, 720}));
  EXPECT_FALSE(resolution.high_dpi_matters);

  EXPECT_EQ(StartWindowMode("", true, true, true), WindowMode::kFullscreen);
  EXPECT_EQ(StartWindowMode("remember", true, false, true),
            WindowMode::kMaximized);
  EXPECT_EQ(StartWindowMode("remember", false, true, false),
            WindowMode::kWindowed);
  EXPECT_EQ(StartWindowMode("windowed", true, true, false),
            WindowMode::kWindowed);
}

TEST(GamePagesTest, RecommendsHighDpiOnlyWhereItMatters) {
  MachineProfile machine = DetectedMachine();
  machine.gpu.nvidia = true;
  EXPECT_EQ(RecommendHighDpi(machine, true), "true");
  EXPECT_FALSE(RecommendHighDpi(machine, false).has_value());
  machine.gpu = {};
  machine.gpu.intel = true;
  EXPECT_EQ(RecommendHighDpi(machine, true), "false");
  // Integrated AMD graphics too, but not a discrete card.
  machine.gpu = {};
  machine.gpu.amd = true;
  EXPECT_EQ(RecommendHighDpi(machine, true), "false");
  machine.gpu.amd_discrete = true;
  EXPECT_EQ(RecommendHighDpi(machine, true), "true");
  machine.monitor.scale = 1.0;
  EXPECT_FALSE(RecommendHighDpi(machine, true).has_value());
}

TEST(GamePagesTest, ExplainsTheDisplayServer) {
  MachineProfile machine = DetectedMachine();
  machine.gpu.nvidia = true;
  machine.gpu.nvidia_kernel_driver = true;
  // Nothing is known about the driver: the NVIDIA rule keeps XWayland.
  DisplayServerChoice choice =
      ResolveDisplayServer(machine, "auto", "direct-vulkan", "auto",
                           Presentation::kUnthrottled);
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);
  choice = ResolveDisplayServer(machine, "auto", "opengl", "auto",
                                Presentation::kDriverDefault);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kWaylandSession);
  choice = ResolveDisplayServer(machine, "wayland", "direct-vulkan", "",
                                Presentation::kDriverDefault);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kChosen);

  // Driver 555 or newer on the only card and explicit sync: Wayland for
  // frames that do not wait for the display, or on Hyprland.
  runtime::HostGpu card;
  card.vendor = kNvidiaPciVendor;
  machine.gpu.cards = {card};
  machine.gpu.nvidia_driver_version = "615.71.09";
  machine.wayland_explicit_sync = window::WaylandExplicitSync::kOffered;
  choice = ResolveDisplayServer(machine, "", "direct-vulkan", "",
                                Presentation::kUnthrottled);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanWayland);
  choice = ResolveDisplayServer(machine, "", "direct-vulkan", "",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);
  machine.hyprland_compositor = true;
  choice = ResolveDisplayServer(machine, "", "direct-vulkan", "",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanWayland);
  machine.nvidia_explicit_sync_disabled = true;
  choice = ResolveDisplayServer(machine, "", "direct-vulkan", "",
                                Presentation::kUnthrottled);
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);
  machine.nvidia_explicit_sync_disabled = false;

  // Drivers the user pinned that leave NVIDIA out, as their manifest says:
  // the rule does not apply.
  TemporaryDirectory manifests;
  machine.wayland_explicit_sync = window::WaylandExplicitSync::kAbsent;
  machine.vulkan_source = VulkanDriverSource::kUser;
  machine.user_vulkan_drivers =
      manifests.Write("radeon_icd.x86_64.json",
                      VulkanManifest("libvulkan_radeon.so"))
          .string();
  choice = ResolveDisplayServer(machine, "auto", "direct-vulkan", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kWaylandSession);
  // A manifest that names NVIDIA's driver under another name does not.
  machine.user_vulkan_drivers =
      manifests.Write("radeon_icd.json", VulkanManifest("libGLX_nvidia.so.0"))
          .string();
  choice = ResolveDisplayServer(machine, "auto", "direct-vulkan", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkanX11);

  // A chosen server the session lacks falls back to automatic.
  machine.user_vulkan_drivers =
      manifests.Write("radeon_icd.x86_64.json",
                      VulkanManifest("libvulkan_radeon.so"))
          .string();
  machine.x11_available = false;
  choice = ResolveDisplayServer(machine, "x11", "direct-vulkan", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kChosenUnavailable);
  machine.wayland_available = false;
  machine.x11_available = true;
  choice = ResolveDisplayServer(machine, "", "opengl", "auto",
                                Presentation::kSynchronized);
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kX11Only);
  EXPECT_EQ(ResolveDisplayServer(MachineProfile{}, "auto", "", "",
                                 Presentation::kDriverDefault)
                .reason,
            DisplayServerReason::kUnknown);
}

TEST(GamePagesTest, SuggestsAMemoryLimit) {
  constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  EXPECT_EQ(SuggestedMemoryLimitMiB(32 * kGiB), 6144U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(16 * kGiB), 4096U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(0), 4096U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(64 * kGiB) % 512U, 0U);
  // Never more than 3/4 of a small computer's memory (MemTotal of a 4 GiB
  // machine is about 3.7 GiB).
  EXPECT_EQ(SuggestedMemoryLimitMiB(3788ULL * 1024U * 1024U), 2560U);
  EXPECT_LE(SuggestedMemoryLimitMiB(2 * kGiB), 1536U);
}

TEST(GamePagesTest, ClassifiesSavedAudioDevices) {
  const std::vector<std::string> devices = {"FxSound (Вывод)", "HDMI", "HDMI"};
  EXPECT_EQ(ClassifyAudioDevice("default", &devices, false),
            AudioDeviceState::kDefault);
  EXPECT_EQ(ClassifyAudioDevice("FxSound (Вывод)", &devices, false),
            AudioDeviceState::kConnected);
  EXPECT_EQ(ClassifyAudioDevice("HDMI", &devices, false),
            AudioDeviceState::kAmbiguous);
  EXPECT_EQ(ClassifyAudioDevice("USB headset", &devices, false),
            AudioDeviceState::kMissing);
  EXPECT_EQ(ClassifyAudioDevice("USB headset", nullptr, false),
            AudioDeviceState::kNotListed);
  EXPECT_EQ(ClassifyAudioDevice("disabled", &devices, true),
            AudioDeviceState::kDisabled);
  EXPECT_EQ(ClassifyAudioDevice("disabled", &devices, false),
            AudioDeviceState::kMissing);
  EXPECT_EQ(ClassifyAudioDevice("id:18", &devices, true),
            AudioDeviceState::kNumericId);
}


// ---- What Mocktail decides and what Roblox decides --------------------------

std::vector<RobloxOverrideKind> OverrideKinds(const GameSettings& settings) {
  std::vector<RobloxOverrideKind> kinds;
  for (const RobloxOverride& entry : ResolveRobloxOverrides(settings)) {
    kinds.push_back(entry.kind);
  }
  return kinds;
}

bool Owns(const GameSettings& settings, RobloxOwnedSetting setting) {
  const std::vector<RobloxOwnedSetting> owned = RobloxOwnedSettings(settings);
  return std::find(owned.begin(), owned.end(), setting) != owned.end();
}

TEST(RobloxOverridesTest, DefaultsForceTheQualityPresetAndDesktopLayout) {
  // An empty config: physics workers on throughput turn the preset on
  // (performance_policy.cc), which forces level 3, and the PC profile gives
  // Roblox the desktop layout (main.cc ApplyDesktopAppPolicy).
  const GameSettings settings;
  EXPECT_EQ(OverrideKinds(settings),
            (std::vector<RobloxOverrideKind>{
                RobloxOverrideKind::kGraphicsQuality,
                RobloxOverrideKind::kRenderingLimits,
                RobloxOverrideKind::kDevice}));
  const std::vector<RobloxOverride> overrides =
      ResolveRobloxOverrides(settings);
  EXPECT_EQ(overrides[0].key, "engine.graphics_quality");
  EXPECT_EQ(overrides[0].number, 3);
  EXPECT_EQ(overrides[0].value, "default");
  EXPECT_EQ(overrides[1].key, "performance.physics_worker_mode");
  EXPECT_EQ(overrides[2].key, "device");
  EXPECT_EQ(overrides[2].value, "pc-windows-11");
  EXPECT_FALSE(Owns(settings, RobloxOwnedSetting::kGraphicsQuality));
  for (const RobloxOwnedSetting owned :
       {RobloxOwnedSetting::kFrameRate, RobloxOwnedSetting::kFullscreen,
        RobloxOwnedSetting::kTheme, RobloxOwnedSetting::kVolume,
        RobloxOwnedSetting::kOutputSwitch, RobloxOwnedSetting::kVoiceChat,
        RobloxOwnedSetting::kCameraSensitivity,
        RobloxOwnedSetting::kCameraMode, RobloxOwnedSetting::kChat}) {
    EXPECT_TRUE(Owns(settings, owned)) << static_cast<int>(owned);
  }
  EXPECT_TRUE(FindOverrideConflicts(settings).empty());

  // Intel integrated graphics with Vulkan get level 1.
  GameSettings intel;
  intel.intel_integrated_vulkan = true;
  EXPECT_EQ(RobloxOverrideOf(intel, "engine.graphics_quality")->number, 1);
}

TEST(RobloxOverridesTest, ThePresetFollowsBothPerformanceKeys) {
  GameSettings settings;
  settings.physics_worker_mode = "latency";
  settings.multithreaded_rendering = "true";
  settings.graphics_quality = "12";
  // Low latency never merges the preset, so no level is forced either.
  EXPECT_FALSE(RobloxOverrideOf(settings, "engine.graphics_quality"));
  EXPECT_FALSE(RobloxOverrideOf(settings, "performance.physics_worker_mode"));
  EXPECT_FALSE(
      RobloxOverrideOf(settings, "performance.multithreaded_rendering"));
  EXPECT_TRUE(Owns(settings, RobloxOwnedSetting::kGraphicsQuality));
  EXPECT_TRUE(FindOverrideConflicts(settings).empty());

  // Automatic physics workers: multithreaded rendering turns it on.
  settings.physics_worker_mode = "auto";
  std::optional<RobloxOverride> preset =
      RobloxOverrideOf(settings, "performance.multithreaded_rendering");
  ASSERT_TRUE(preset.has_value());
  EXPECT_EQ(preset->kind, RobloxOverrideKind::kRenderingLimits);
  EXPECT_FALSE(RobloxOverrideOf(settings, "performance.physics_worker_mode"));
  EXPECT_EQ(RobloxOverrideOf(settings, "engine.graphics_quality")->number, 12);
  settings.multithreaded_rendering = "false";
  EXPECT_EQ(ResolveRobloxOverrides(settings).size(), 1U);  // the device

  // Throughput turns it on by itself; multithreaded rendering adds nothing.
  settings.physics_worker_mode = "throughput";
  settings.multithreaded_rendering = "true";
  preset = RobloxOverrideOf(settings, "performance.physics_worker_mode");
  ASSERT_TRUE(preset.has_value());
  EXPECT_FALSE(
      RobloxOverrideOf(settings, "performance.multithreaded_rendering"));
}

TEST(RobloxOverridesTest, WarnsWhenThePresetStillLimitsAHigherLevel) {
  GameSettings settings;
  settings.graphics_quality = "12";
  EXPECT_EQ(FindOverrideConflicts(settings),
            (std::vector<OverrideConflict>{
                OverrideConflict::kHighLevelUnderPresetLimits}));
  const std::optional<RobloxOverride> quality =
      RobloxOverrideOf(settings, "engine.graphics_quality");
  ASSERT_TRUE(quality.has_value());
  EXPECT_EQ(quality->value, "level");
  EXPECT_EQ(quality->number, 12);
  // Levels up to Mocktail's own 3 lose nothing to the preset.
  settings.graphics_quality = "3";
  EXPECT_TRUE(FindOverrideConflicts(settings).empty());
  // Roblox's slider decides the level, the preset's other limits stay.
  settings.graphics_quality = "manual";
  EXPECT_FALSE(RobloxOverrideOf(settings, "engine.graphics_quality"));
  EXPECT_TRUE(Owns(settings, RobloxOwnedSetting::kGraphicsQuality));
  EXPECT_EQ(FindOverrideConflicts(settings),
            (std::vector<OverrideConflict>{
                OverrideConflict::kRobloxSliderUnderPresetLimits}));
  // The environment's spellings of manual (ParseGraphicsQualityVariable).
  settings.graphics_quality = "auto";
  EXPECT_FALSE(RobloxOverrideOf(settings, "engine.graphics_quality"));
  settings.graphics_quality = "0";
  EXPECT_FALSE(RobloxOverrideOf(settings, "engine.graphics_quality"));
}

TEST(RobloxOverridesTest, FollowsTheFrameRatePolicy) {
  // frame_rate_policy.cc: -1 and display set no target.
  GameSettings settings;
  for (const char* value : {"", "-1", "display", "fast"}) {
    settings.frame_rate_limit = value;
    EXPECT_FALSE(RobloxOverrideOf(settings, "graphics.frame_rate_limit"))
        << value;
    EXPECT_TRUE(Owns(settings, RobloxOwnedSetting::kFrameRate)) << value;
  }
  settings.frame_rate_limit = "144";
  std::optional<RobloxOverride> target =
      RobloxOverrideOf(settings, "graphics.frame_rate_limit");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->number, 144);
  EXPECT_EQ(target->value, "fixed");
  EXPECT_FALSE(Owns(settings, RobloxOwnedSetting::kFrameRate));
  settings.frame_rate_limit = "unlimited";
  target = RobloxOverrideOf(settings, "graphics.frame_rate_limit");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->number, 240);
  EXPECT_EQ(target->value, "unlimited");
}

TEST(RobloxOverridesTest, FollowsTheWindowThemeMicrophoneAndDevice) {
  GameSettings settings;
  settings.physics_worker_mode = "latency";
  settings.device = "mobile-pixel-7";
  EXPECT_TRUE(ResolveRobloxOverrides(settings).empty());
  for (const char* value : {"roblox", "remember", "default"}) {
    settings.theme = value;
    settings.start_mode = value;
    settings.input_device = value;
    EXPECT_TRUE(ResolveRobloxOverrides(settings).empty()) << value;
  }

  settings.theme = "dark";
  settings.start_mode = "fullscreen";
  settings.input_device = "disabled";
  settings.device = "console";
  EXPECT_EQ(OverrideKinds(settings),
            (std::vector<RobloxOverrideKind>{
                RobloxOverrideKind::kStartMode, RobloxOverrideKind::kTheme,
                RobloxOverrideKind::kMicrophone,
                RobloxOverrideKind::kDevice}));
  EXPECT_EQ(RobloxOverrideOf(settings, "appearance.theme")->value, "dark");
  EXPECT_EQ(RobloxOverrideOf(settings, "display.start_mode")->value,
            "fullscreen");
  EXPECT_EQ(RobloxOverrideOf(settings, "device")->value, "console-ps5");
  EXPECT_FALSE(Owns(settings, RobloxOwnedSetting::kTheme));
  EXPECT_FALSE(Owns(settings, RobloxOwnedSetting::kFullscreen));
  EXPECT_FALSE(Owns(settings, RobloxOwnedSetting::kVoiceChat));
  EXPECT_TRUE(Owns(settings, RobloxOwnedSetting::kVolume));

  for (const char* value : {"system", "light"}) {
    settings.theme = value;
    EXPECT_TRUE(RobloxOverrideOf(settings, "appearance.theme")) << value;
  }
  for (const char* value : {"windowed", "maximized"}) {
    settings.start_mode = value;
    EXPECT_TRUE(RobloxOverrideOf(settings, "display.start_mode")) << value;
  }
  // An unknown profile does not load at all, so it decides nothing.
  settings.device = "toaster";
  EXPECT_FALSE(RobloxOverrideOf(settings, "device"));
  settings.device = "pc";
  EXPECT_EQ(RobloxOverrideOf(settings, "device")->value, "pc-windows-11");
}

TEST(RobloxOverridesTest, LeavesOnlyUntouchedSettingsWithEveryOverride) {
  GameSettings settings;
  settings.graphics_quality = "12";
  settings.frame_rate_limit = "unlimited";
  settings.start_mode = "windowed";
  settings.theme = "system";
  settings.input_device = "disabled";
  EXPECT_EQ(ResolveRobloxOverrides(settings).size(), 7U);
  EXPECT_EQ(RobloxOwnedSettings(settings),
            (std::vector<RobloxOwnedSetting>{
                RobloxOwnedSetting::kVolume, RobloxOwnedSetting::kOutputSwitch,
                RobloxOwnedSetting::kCameraSensitivity,
                RobloxOwnedSetting::kCameraMode, RobloxOwnedSetting::kChat}));
  // Each override names the row whose value causes it.
  for (const RobloxOverride& entry : ResolveRobloxOverrides(settings)) {
    EXPECT_EQ(RobloxOverrideOf(settings, entry.key)->kind, entry.kind)
        << entry.key;
  }
}

TEST(RobloxOverridesTest, ListsTheAlwaysOnFlags) {
  std::string names;
  for (const AlwaysOnFlagInfo& info : AlwaysOnFlags()) {
    EXPECT_FALSE(info.names.empty());
    // The values the runtime sets; the video memory budget depends on the
    // RAM.
    EXPECT_EQ(info.names.find('=') == std::string_view::npos,
              info.flag == AlwaysOnFlag::kVideoMemory)
        << info.names;
    names += std::string(info.names) + ",";
  }
  for (const char* flag :
       {"FFlagGameBasicSettingsFramerateCap5",
        "FFlagDebugUseWebRtcAudioDevices", "FFlagRemoteAudioDeviceSync",
        "DFFlagVoiceChatSkipPermissionCheckForTests",
        "FIntRenderForceVideoMemorySize"}) {
    EXPECT_NE(names.find(flag), std::string::npos) << flag;
  }
  // texture_memory_policy.cc: an eighth of the RAM, 256 MiB to 1.5 GiB,
  // nothing below 4 GiB.
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  EXPECT_EQ(VideoMemoryBudgetBytes(16384 * kMiB), 1536 * kMiB);
  EXPECT_EQ(VideoMemoryBudgetBytes(4096 * kMiB), 512 * kMiB);
  EXPECT_EQ(VideoMemoryBudgetBytes(2048 * kMiB), 0U);
}

TEST_F(MachineProfileTest, OffersTheGraphicsCardsThisComputerHas) {
  // One NVIDIA card: integrated graphics cannot be chosen, and every value
  // uses the card.
  AddPciGpu(0, "pci0000:00/0000:00:01.0/0000:01:00.0", "0x10de", "0x2504");
  root_.Write("usr/share/vulkan/icd.d/nvidia_icd.json", "{}");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(GpuChoiceAvailability(profile, runtime::GpuPreference::kAuto),
            GpuChoiceState::kAvailable);
  EXPECT_EQ(GpuChoiceAvailability(profile, runtime::GpuPreference::kDiscrete),
            GpuChoiceState::kAvailable);
  EXPECT_EQ(
      GpuChoiceAvailability(profile, runtime::GpuPreference::kIntegrated),
      GpuChoiceState::kNoSuchCard);
  EXPECT_EQ(RecommendGpuPreference(profile).value, "auto");
  EXPECT_EQ(RecommendGpuPreference(profile).reason,
            GpuRecommendationReason::kSingleCard);

  // Intel graphics beside it, without their driver: they cannot be picked.
  AddPciGpu(1, "pci0000:00/0000:00:02.0", "0x8086", "0x9a49");
  profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(
      GpuChoiceAvailability(profile, runtime::GpuPreference::kIntegrated),
      GpuChoiceState::kNoVulkanDriver);
  // With it, both can; Automatic picks the discrete card unless PRIME
  // offloading is off.
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{}");
  profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(
      GpuChoiceAvailability(profile, runtime::GpuPreference::kIntegrated),
      GpuChoiceState::kAvailable);
  EXPECT_EQ(RecommendGpuPreference(profile).reason,
            GpuRecommendationReason::kDiscreteCard);
  profile = DetectMachineProfile(MapEnvironment({{"DRI_PRIME", "0"}}), Probe());
  EXPECT_EQ(RecommendGpuPreference(profile).reason,
            GpuRecommendationReason::kPrimeIntegrated);

  // The user's own driver list: Mocktail picks nothing, so nothing is said.
  profile = DetectMachineProfile(
      MapEnvironment({{"VK_DRIVER_FILES", "/opt/vk/my_icd.json"}}), Probe());
  EXPECT_EQ(GpuChoiceAvailability(profile, runtime::GpuPreference::kDiscrete),
            GpuChoiceState::kUnknown);
  EXPECT_TRUE(RecommendGpuPreference(profile).value.empty());
  EXPECT_EQ(GpuChoiceAvailability(MachineProfile{},
                                  runtime::GpuPreference::kDiscrete),
            GpuChoiceState::kUnknown);
}

TEST_F(MachineProfileTest, ExplainsNvidiaShaderLoading) {
  EXPECT_EQ(ResolveShaderLoading(MachineProfile{}, "", "", ""),
            ShaderLoadingState::kUnknown);
  // AMD only: nothing was ever restricted.
  AddPciGpu(0, "pci0000:00/0000:00:01.1/0000:0a:00.0", "0x1002", "0x73df");
  root_.Write("usr/share/vulkan/icd.d/radeon_icd.x86_64.json", "{}");
  MachineProfile profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(ResolveShaderLoading(profile, "", "", "false"),
            ShaderLoadingState::kNoNvidia);

  // An Intel + NVIDIA laptop.
  std::filesystem::remove_all(root_.path() / "sys");
  AddPciGpu(0, "pci0000:00/0000:00:02.0", "0x8086", "0x9a49");
  AddPciGpu(1, "pci0000:00/0000:00:01.0/0000:01:00.0", "0x10de", "0x2520");
  root_.Write("usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{}");
  root_.Write("usr/share/vulkan/icd.d/nvidia_icd.json", "{}");
  profile = DetectMachineProfile(MapEnvironment(), Probe());
  EXPECT_EQ(ResolveShaderLoading(profile, "", "", ""),
            ShaderLoadingState::kMultithreaded);
  EXPECT_EQ(ResolveShaderLoading(profile, "vulkan", "auto", "true"),
            ShaderLoadingState::kMultithreaded);
  EXPECT_EQ(ResolveShaderLoading(profile, "direct-vulkan", "auto", "false"),
            ShaderLoadingState::kSingleThread);
  // Only direct Vulkan reads the flag, and only an NVIDIA card is denied.
  EXPECT_EQ(ResolveShaderLoading(profile, "opengl", "auto", "false"),
            ShaderLoadingState::kNotVulkan);
  EXPECT_EQ(ResolveShaderLoading(profile, "angle-vulkan", "auto", "false"),
            ShaderLoadingState::kNotVulkan);
  EXPECT_EQ(
      ResolveShaderLoading(profile, "direct-vulkan", "integrated", "false"),
      ShaderLoadingState::kOtherCard);
}

}  // namespace
}  // namespace mocktail::launcher_ui
