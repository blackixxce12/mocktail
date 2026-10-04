// GTK-free parts of the settings window: the staged config.yaml draft, the
// environment overrides, the machine profile, recommendations and search.

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

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
  EXPECT_NE(profile.vulkan_icd.find("nvidia_icd.json"), std::string::npos);
  EXPECT_TRUE(profile.has_vulkan_driver());
  // NVIDIA + direct Vulkan + both displays: automatic means XWayland.
  EXPECT_TRUE(profile.NvidiaDirectVulkanUsesX11());
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan"), "x11");
  EXPECT_EQ(profile.AutomaticDisplayServer("vulkan"), "x11");
  EXPECT_EQ(profile.AutomaticDisplayServer("opengl"), "wayland");
  EXPECT_EQ(profile.AutomaticDisplayServer("angle-vulkan"), "wayland");
  EXPECT_EQ(RecommendGraphicsBackend(profile).value, "direct-vulkan");
  EXPECT_EQ(RecommendGraphicsBackend(profile).reason,
            BackendRecommendationReason::kVulkanDriver);
}

TEST_F(MachineProfileTest, DetectsAMachineWithoutVulkan) {
  AddGpu(0, "0x8086");
  const MachineProfile profile = DetectMachineProfile(
      MapEnvironment({{"DISPLAY", ":1"}, {"XDG_CURRENT_DESKTOP", "XFCE"}}),
      Probe());
  EXPECT_TRUE(profile.gpu.intel_only());
  EXPECT_FALSE(profile.gpu.nvidia_kernel_driver);
  EXPECT_EQ(profile.session, SessionType::kX11);
  EXPECT_FALSE(profile.wayland_available);
  EXPECT_EQ(profile.desktop, "Xfce");
  EXPECT_FALSE(profile.has_vulkan_driver());
  EXPECT_FALSE(profile.angle.has_value());
  EXPECT_FALSE(profile.gamemode_library);
  EXPECT_FALSE(profile.NvidiaDirectVulkanUsesX11());
  EXPECT_EQ(profile.AutomaticDisplayServer("direct-vulkan"), "x11");
  EXPECT_EQ(RecommendGraphicsBackend(profile).value, "opengl");
  EXPECT_EQ(RecommendGraphicsBackend(profile).reason,
            BackendRecommendationReason::kNoVulkanDriver);
  EXPECT_EQ(RecommendGraphicsBackend(MachineProfile{}).reason,
            BackendRecommendationReason::kUnknown);
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
  MonitorInfo monitor;
  EXPECT_FALSE(RecommendFrameRate(monitor).has_value());
  monitor.valid = true;
  monitor.refresh_millihertz = 165003;
  EXPECT_EQ(RecommendFrameRate(monitor), "165");
  monitor.refresh_millihertz = 59950;
  EXPECT_EQ(RecommendFrameRate(monitor), "-1");
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
  machine.monitor.scale = 1.0;
  EXPECT_FALSE(RecommendHighDpi(machine, true).has_value());
}

TEST(GamePagesTest, ExplainsTheDisplayServer) {
  MachineProfile machine = DetectedMachine();
  machine.gpu.nvidia = true;
  machine.gpu.nvidia_kernel_driver = true;
  DisplayServerChoice choice =
      ResolveDisplayServer(machine, "auto", "direct-vulkan");
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kNvidiaVulkan);
  choice = ResolveDisplayServer(machine, "auto", "opengl");
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kWaylandSession);
  choice = ResolveDisplayServer(machine, "wayland", "direct-vulkan");
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kChosen);
  // A chosen server the session lacks falls back to automatic.
  machine.x11_available = false;
  choice = ResolveDisplayServer(machine, "x11", "direct-vulkan");
  EXPECT_EQ(choice.server, "wayland");
  EXPECT_EQ(choice.reason, DisplayServerReason::kChosenUnavailable);
  machine.wayland_available = false;
  machine.x11_available = true;
  choice = ResolveDisplayServer(machine, "", "opengl");
  EXPECT_EQ(choice.server, "x11");
  EXPECT_EQ(choice.reason, DisplayServerReason::kX11Only);
  EXPECT_EQ(ResolveDisplayServer(MachineProfile{}, "auto", "").reason,
            DisplayServerReason::kUnknown);
}

TEST(GamePagesTest, SuggestsAMemoryLimit) {
  constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  EXPECT_EQ(SuggestedMemoryLimitMiB(32 * kGiB), 6144U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(16 * kGiB), 4096U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(0), 4096U);
  EXPECT_EQ(SuggestedMemoryLimitMiB(64 * kGiB) % 512U, 0U);
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

}  // namespace
}  // namespace mocktail::launcher_ui
