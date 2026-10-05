#include "runtime/runtime_config_file.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/graphics_launch_policy.h"
#include "runtime/runtime_config_bootstrap.h"

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

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
    char pattern[] = "/tmp/mocktail_runtime_config_XXXXXX";
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
    std::ofstream output(file);
    output << contents;
    return file;
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

TEST(RuntimeConfigBootstrapTest, CreatesCompletePrivateFirstRunFile) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.path() / "nested/mocktail/config.yaml";

  const mode_t previous_umask = umask(0);
  const RuntimeConfigBootstrapResult bootstrapped =
      EnsureRuntimeConfigFile(file);
  umask(previous_umask);

  ASSERT_TRUE(bootstrapped) << bootstrapped.error;
  EXPECT_TRUE(bootstrapped.created());
  struct stat metadata = {};
  ASSERT_EQ(lstat(file.c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISREG(metadata.st_mode));
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
  ASSERT_EQ(lstat(file.parent_path().c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISDIR(metadata.st_mode));
  EXPECT_EQ(metadata.st_mode & 0777, 0700);
  EXPECT_EQ(ReadFile(file), DefaultRuntimeConfigYaml());

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.theme_mode(), "roblox");
  EXPECT_EQ(loaded.config.frame_rate().mode, FrameRateLimitMode::kUnmanaged);
  EXPECT_EQ(loaded.config.vsync_mode(), "auto");
  EXPECT_FALSE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 0U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kAuto);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kThroughput);
  EXPECT_EQ(loaded.config.audio_output_device(), "default");
  EXPECT_EQ(loaded.config.audio_input_device(), "default");
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.desktop_playability());
  EXPECT_EQ(loaded.config.device_profile().name, "pc-windows-11");
}

TEST(RuntimeConfigBootstrapTest,
     MatchesShippedExampleAndDocumentsEveryDefault) {
  const std::filesystem::path example =
      std::filesystem::path(MOCKTAIL_TEST_SOURCE_DIR) /
      "config/mocktail.example.yaml";
  const std::string defaults(DefaultRuntimeConfigYaml());

  EXPECT_EQ(ReadFile(example), defaults);
  for (const std::string_view documented_setting : {
           "# Integer: configuration schema version. Only version 1 is "
           "supported.\nversion: 1",
           "# Presets: pc-windows-11, mobile-pixel-7, "
           "console-ps5.\n# Use the mapping below for a custom "
           "profile.\ndevice: pc-windows-11",
           "# Boolean (default: false): start without a visible SDL "
           "window.\n  headless: false",
           "# String (default: roblox): use Roblox's saved account theme. "
           "Supported\n  # overrides: dark, light, or system (follow the "
           "desktop color scheme).\n  theme: roblox",
           "# Recommended values: direct-vulkan, opengl, system, "
           "angle-vulkan.\n  backend: direct-vulkan",
           "# DFIntTaskSchedulerTargetFps without a whitelist.\n  "
           "# frame_rate_limit: -1",
           "# Optional presentation synchronization override: auto, on, or "
           "off.\n  # vsync: off",
           "# physical CPU core. A place's Lua/main thread can still remain "
           "serial.\n  multithreaded_rendering: false",
           "# sizes and coalesces midphase work. Supported: auto, latency, "
           "throughput.\n  physics_worker_mode: throughput",
           "# Integer MiB (default: 0): hard RAM cap for the Mocktail game "
           "process.\n  # 0 disables the cap. A watchdog stops Mocktail (exit "
           "status 137) once the\n  # process's resident memory plus swap "
           "reaches the cap; swap stays enabled.\n  # 6144 is a conservative "
           "starting point for 32 GiB RAM.\n  memory_limit_mb: 0",
           "# String (default: auto): request Feral GameMode when its host "
           "daemon and\n  # client library are available. Supported values: "
           "auto, on, off.\n  gamemode: auto",
           "# To pin output, copy an exact SDL device name printed during "
           "startup.\n  output_device: default",
           "# between boots; prefer the exact device name when it is unique.\n"
           "  input_device: default",
           "# Boolean (default: false): publish Mocktail activity to Discord "
           "Desktop.\n    # This never signs in to Discord and never reads an "
           "account token.\n    enabled: false",
           "# Boolean (default: true): show the current Roblox experience "
           "name.\n    show_place_name: true",
           "# Boolean (default: true): show how long the current session has "
           "run.\n    show_elapsed_time: true",
           "# Boolean (default: true): let friends open the current "
           "experience. When\n      # Roblox provides a public server ID, the "
           "button targets that server.\n      enabled: true",
           "# Boolean (default: true): never expose private or reserved "
           "joins.\n      public_servers_only: true",
           "#   playing: \"{place_name}\"\n    #   state: Playing Roblox",
            "# Integer (default: 1280): initial window width in logical desktop "
            "units.\n  "
            "width: 1280",
            "# Integer (default: 720): initial window height in logical desktop "
            "units.\n  "
            "height: 720",
            "# String (default: Roblox): window title.\n  title: Roblox",
            "# Boolean (default: false): render at physical display-pixel density instead\n  "
            "# of the logical desktop resolution. Enable only for sharper high-DPI output.\n  "
            "high_dpi: false",
           "# or x11. auto prefers Wayland but uses X11 (XWayland) for NVIDIA "
           "with direct\n  # Vulkan. SDL_VIDEODRIVER, when set, still takes "
           "precedence.\n  server: auto",
           "# String (default: remember): window state at start: remember (the "
           "mode the\n  # last session ended in), windowed, maximized, or "
           "fullscreen.\n  start_mode: remember",
           "# String (default: native): sign in on Roblox's own welcome screen "
           "(native)\n  # or in Mocktail's browser sign-in window (browser).\n  "
           "sign_in: native",
           "# Vulkan on Intel integrated graphics), manual leaves Roblox's "
           "in-game\n  # graphics slider in control, and an integer from 1 to "
           "21 forces that level.\n  graphics_quality: default",
           "# String (default: auto): graphics card for direct Vulkan when the "
           "computer\n  # has more than one: auto, discrete, or integrated. "
           "auto prefers the\n  # discrete card unless DRI_PRIME or "
           "__NV_PRIME_RENDER_OFFLOAD is 0. With a\n  # single card that card "
           "is used; VK_DRIVER_FILES, when set, takes precedence.\n  gpu: auto",
           "# Boolean (default: true): with direct Vulkan, let Roblox load its "
           "shader\n  # pack on several threads on NVIDIA GPUs, as it does on "
           "Intel and AMD.\n  # false makes Roblox load it on one thread "
           "there, as Mocktail did before\n  # this setting existed; use it "
           "only if shader loading fails on NVIDIA.\n  nvidia_shader_mt: true",
           "# Boolean (default: true): show the Mocktail settings window before "
           "Roblox\n  # starts. Website joins never show it; `mocktail "
           "--launcher` shows it and\n  # `mocktail --play` skips it regardless "
           "of this setting.\n  show_on_start: true",
           "# HostAbi profile, run two isolated canaries with the selected "
           "graphics\n  "
           "# backend, and promote only on success. An existing current "
           "payload is\n  "
           "# preserved when probation fails.\n  "
           "automatic: true",
           "# String (default: apk-pure): direct x86_64 APK provider "
           "selection. Both\n  "
           "# accepted values resolve the provider chain, which currently "
           "starts with\n  "
           "# APKPure. Supported values: auto, apk-pure.\n  "
           "source: apk-pure",
           "# Reserved for desktop update integrations. `mocktail_updater` "
           "itself never\n  # launches another process after changing the "
           "active payload.\n  launch_after_update: false",
       }) {
    EXPECT_NE(defaults.find(documented_setting), std::string::npos)
        << documented_setting;
  }
  EXPECT_EQ(defaults.find("testing_latest_only"), std::string::npos);
  // The cgroup scope that once disabled swap is gone; only the watchdog runs.
  EXPECT_EQ(defaults.find("cgroup"), std::string::npos);
}

TEST(RuntimeConfigBootstrapTest, PreservesExistingRegularFile) {
  TemporaryDirectory temporary;
  const std::filesystem::path regular = temporary.Write("user: settings\n");
  RuntimeConfigBootstrapResult bootstrapped = EnsureRuntimeConfigFile(regular);
  ASSERT_TRUE(bootstrapped) << bootstrapped.error;
  EXPECT_FALSE(bootstrapped.created());
  EXPECT_EQ(ReadFile(regular), "user: settings\n");
}

TEST(RuntimeConfigBootstrapTest, RejectsSymlinkAndNonRegularEntry) {
  TemporaryDirectory temporary;
  const std::filesystem::path target = temporary.path() / "owned.yaml";
  std::ofstream(target) << "owned: true\n";
  const std::filesystem::path symlink = temporary.path() / "linked.yaml";
  ASSERT_EQ(::symlink(target.c_str(), symlink.c_str()), 0);

  RuntimeConfigBootstrapResult bootstrapped = EnsureRuntimeConfigFile(symlink);
  EXPECT_FALSE(bootstrapped);
  EXPECT_NE(bootstrapped.error.find("symlink"), std::string::npos);
  EXPECT_TRUE(
      std::filesystem::is_symlink(std::filesystem::symlink_status(symlink)));
  EXPECT_EQ(ReadFile(target), "owned: true\n");

  const std::filesystem::path directory = temporary.path() / "directory.yaml";
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  bootstrapped = EnsureRuntimeConfigFile(directory);
  EXPECT_FALSE(bootstrapped);
  EXPECT_NE(bootstrapped.error.find("regular file"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_directory(directory));
}

TEST(RuntimeConfigBootstrapTest, PublishesOnlyOneWinnerDuringFirstRunRace) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.path() / "race/config.yaml";
  std::vector<RuntimeConfigBootstrapResult> results(12);
  std::vector<std::thread> workers;
  workers.reserve(results.size());
  for (std::size_t index = 0; index < results.size(); ++index) {
    workers.emplace_back([&file, &results, index]() {
      results[index] = EnsureRuntimeConfigFile(file);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  std::size_t created = 0;
  for (const RuntimeConfigBootstrapResult& result : results) {
    ASSERT_TRUE(result) << result.error;
    created += result.created() ? 1 : 0;
  }
  EXPECT_EQ(created, 1);
  EXPECT_EQ(ReadFile(file), DefaultRuntimeConfigYaml());
}

TEST(RuntimeConfigFileTest, UsesDefaultsWhenFileDoesNotExist) {
  const MapEnvironment environment;
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/config.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.theme_mode(), "roblox");
  EXPECT_EQ(loaded.config.roblox_library_path(), "rbx_bin/libroblox.so");
}

TEST(RuntimeConfigFileTest, RejectsSymlinkAndOversizedConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path target = temporary.Write("version: 1\n");
  const std::filesystem::path symlink = temporary.path() / "linked.yaml";
  ASSERT_EQ(::symlink(target.c_str(), symlink.c_str()), 0);

  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), symlink);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("cannot open"), std::string::npos);

  const std::filesystem::path oversized = temporary.path() / "large.yaml";
  std::ofstream output(oversized, std::ios::binary);
  output.seekp(1024 * 1024);
  output.put('\n');
  output.close();
  loaded = LoadRuntimeConfig(MapEnvironment(), oversized);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("no larger than 1 MiB"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsTypedDesktopAndGraphicsSettings) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
runtime:
  headless: false
  roblox_library: /payload/libroblox.so
appearance:
  theme: dark
graphics:
  backend: vulkan
  frame_rate_limit: unlimited
  vsync: off
performance:
  multithreaded_rendering: true
  physics_worker_mode: latency
  memory_limit_mb: 6144
  gamemode: off
audio:
  output_device: Built-in Audio Analog Stereo
window:
  width: 1920
  height: 1080
  title: Mocktail Desktop
  high_dpi: true
input:
  touch_enabled: false
network:
  proxy_host: proxy.example.test
  proxy_port: 3128
integrations:
  discord_rpc:
    enabled: true
    show_place_name: false
    show_elapsed_time: false
    application_id: 123456789012345678
    join:
      enabled: true
      public_servers_only: true
      button_label: Play Together
    text:
      browsing: Looking for games
      joining: Connecting
      playing: "Inside {place_name}"
      state: Playing Roblox on Linux
      unknown_place: Unknown world
updates:
  automatic: true
)yaml");

  const MapEnvironment environment;
  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(environment, file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.roblox_library_path(), "/payload/libroblox.so");
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.frame_rate().mode, FrameRateLimitMode::kUnlimited);
  EXPECT_EQ(loaded.config.vsync_mode(), "off");
  EXPECT_EQ(loaded.config.theme_mode(), "dark");
  EXPECT_TRUE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kLatency);
  EXPECT_GT(loaded.config.performance().physical_core_count, 0);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 6144U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kOff);
  EXPECT_EQ(loaded.config.audio_output_device(),
            "Built-in Audio Analog Stereo");
  EXPECT_EQ(loaded.config.window().width, 1920);
  EXPECT_EQ(loaded.config.window().height, 1080);
  EXPECT_EQ(loaded.config.window().title, "Mocktail Desktop");
  EXPECT_TRUE(loaded.config.window().high_dpi);
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.desktop_playability());
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxDesktopHttpUserAgent);
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->host, "proxy.example.test");
  EXPECT_EQ(loaded.config.network_proxy()->port, 3128);
  EXPECT_TRUE(loaded.config.discord_rpc().enabled);
  EXPECT_FALSE(loaded.config.discord_rpc().show_place_name);
  EXPECT_FALSE(loaded.config.discord_rpc().show_elapsed_time);
  EXPECT_EQ(loaded.config.discord_rpc().application_id,
            "123456789012345678");
  EXPECT_TRUE(loaded.config.discord_rpc().join_enabled);
  EXPECT_TRUE(loaded.config.discord_rpc().public_servers_only);
  EXPECT_EQ(loaded.config.discord_rpc().join_button_label, "Play Together");
  EXPECT_EQ(loaded.config.discord_rpc().text.browsing, "Looking for games");
  EXPECT_EQ(loaded.config.discord_rpc().text.joining, "Connecting");
  EXPECT_EQ(loaded.config.discord_rpc().text.playing,
            "Inside {place_name}");
  EXPECT_EQ(loaded.config.discord_rpc().text.state,
            "Playing Roblox on Linux");
  EXPECT_EQ(loaded.config.discord_rpc().text.unknown_place, "Unknown world");
}

TEST(RuntimeConfigFileTest, EnvironmentOverridesYaml) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  backend: system
  frame_rate_limit: 60
  vsync: on
performance:
  multithreaded_rendering: true
  physics_worker_mode: latency
  memory_limit_mb: 4096
  gamemode: on
audio:
  output_device: YAML Speakers
  input_device: YAML Microphone
input:
  touch_enabled: true
compatibility:
  desktop_playability: false
window:
  high_dpi: true
network:
  proxy_host: yaml-proxy.example.test
  proxy_port: 8080
)yaml");
  const MapEnvironment environment({
      {"MOCKTAIL_GRAPHICS_BACKEND", "vulkan"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "144"},
      {"MOCKTAIL_VSYNC", "off"},
      {"MOCKTAIL_MULTITHREADED_RENDERING", "0"},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "auto"},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "8192"},
      {"MOCKTAIL_GAMEMODE", "off"},
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Environment Headset"},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "Environment Microphone"},
      {"MOCKTAIL_TOUCH_MODE", "off"},
      {"MOCKTAIL_DESKTOP_PLAYABILITY", "1"},
      {"MOCKTAIL_WIN_HIGH_DPI", "0"},
      {"MOCKTAIL_HTTP_PROXY_HOST", "env-proxy.example.test"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "1080"},
  });

  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(environment, file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.frame_rate().fixed_fps, 144);
  EXPECT_EQ(loaded.config.vsync_mode(), "off");
  EXPECT_FALSE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kAuto);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 8192U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kOff);
  EXPECT_EQ(loaded.config.audio_output_device(), "Environment Headset");
  EXPECT_EQ(loaded.config.audio_input_device(), "Environment Microphone");
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_FALSE(loaded.config.window().high_dpi);
  EXPECT_TRUE(loaded.config.desktop_playability());
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxDesktopHttpUserAgent);
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->host, "env-proxy.example.test");
  EXPECT_EQ(loaded.config.network_proxy()->port, 1080);
}

TEST(RuntimeConfigFileTest, RejectsUnsafeAudioOutputDevice) {
  TemporaryDirectory temporary;
  const std::filesystem::path empty = temporary.Write(R"yaml(
version: 1
audio:
  output_device: ""
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), empty);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("audio.output_device"), std::string::npos);

  const RuntimeConfig from_environment = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Speaker\nInjected"}}));
  EXPECT_FALSE(from_environment.audio_output_device_valid());
  std::string error;
  EXPECT_FALSE(ExportRuntimeConfigEnvironment(from_environment, &error));
  EXPECT_NE(error.find("invalid audio output device"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsInvalidHighDpiPolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
window:
  high_dpi: yes
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("window.high_dpi"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsUnsafeAudioInputDevice) {
  TemporaryDirectory temporary;
  const std::filesystem::path empty = temporary.Write(R"yaml(
version: 1
audio:
  input_device: ""
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), empty);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("audio.input_device"), std::string::npos);

  const RuntimeConfig from_environment = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "Mic\nInjected"}}));
  EXPECT_FALSE(from_environment.audio_input_device_valid());
  std::string error;
  EXPECT_FALSE(ExportRuntimeConfigEnvironment(from_environment, &error));
  EXPECT_NE(error.find("invalid audio input device"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsIncompleteOrInvalidNetworkProxy) {
  TemporaryDirectory temporary;
  const std::filesystem::path incomplete = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), incomplete);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("configured together"), std::string::npos);

  const std::filesystem::path scheme = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: http://proxy.example.test
  proxy_port: 8080
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), scheme);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("without a scheme"), std::string::npos);

  const std::filesystem::path path = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test/path
  proxy_port: 8080
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), path);
  EXPECT_FALSE(loaded);

  const std::filesystem::path port = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test
  proxy_port: 65536
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), port);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("1 to 65535"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsSystemProxyFlagWithoutFixedEndpoint) {
  TemporaryDirectory temporary;
  const std::filesystem::path path = temporary.Write(R"yaml(
version: 1
network:
  use_system_proxy: true
)yaml");
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), path);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.use_system_proxy());
  EXPECT_FALSE(loaded.config.network_proxy().has_value());
}

TEST(RuntimeConfigFileTest, ExportsProxyVariablesOnlyWhenConfigured) {
  unsetenv("MOCKTAIL_HTTP_PROXY_HOST");
  unsetenv("MOCKTAIL_HTTP_PROXY_PORT");
  unsetenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  unsetenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY");
  std::string error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(
      RuntimeConfig::FromEnvironment(MapEnvironment()), &error))
      << error;
  EXPECT_EQ(getenv("MOCKTAIL_HTTP_PROXY_HOST"), nullptr);
  EXPECT_EQ(getenv("MOCKTAIL_HTTP_PROXY_PORT"), nullptr);
  EXPECT_EQ(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), nullptr);

  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_HTTP_PROXY_HOST", "127.0.0.1"},
                      {"MOCKTAIL_HTTP_PROXY_PORT", "7890"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_HOST"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_PORT"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_SCHEME"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_HOST"), "127.0.0.1");
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_PORT"), "7890");
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_SCHEME"), "http");
  EXPECT_STREQ(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), "1");
  unsetenv("MOCKTAIL_HTTP_PROXY_HOST");
  unsetenv("MOCKTAIL_HTTP_PROXY_PORT");
  unsetenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  unsetenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY");
}

TEST(RuntimeConfigFileTest, ExportsMultithreadedRenderingPolicy) {
  unsetenv("MOCKTAIL_MULTITHREADED_RENDERING");
  std::string error;
  const RuntimeConfig enabled = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_MULTITHREADED_RENDERING", "1"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(enabled, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), "1");

  const RuntimeConfig disabled =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(disabled, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), "0");
  unsetenv("MOCKTAIL_MULTITHREADED_RENDERING");
}

TEST(RuntimeConfigFileTest, ExportsHighDpiWindowPolicy) {
  unsetenv("MOCKTAIL_WIN_HIGH_DPI");
  std::string error;
  const RuntimeConfig default_config =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(default_config, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_WIN_HIGH_DPI"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_WIN_HIGH_DPI"), "0");

  const RuntimeConfig high_dpi = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_WIN_HIGH_DPI", "1"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(high_dpi, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_WIN_HIGH_DPI"), "1");
  unsetenv("MOCKTAIL_WIN_HIGH_DPI");
}

TEST(RuntimeConfigFileTest, ExportsPhysicsWorkerMode) {
  unsetenv("MOCKTAIL_PHYSICS_WORKER_MODE");
  std::string error;
  const RuntimeConfig latency = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_PHYSICS_WORKER_MODE", "latency"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(latency, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), "latency");

  const RuntimeConfig default_config =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(default_config, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), "throughput");
  unsetenv("MOCKTAIL_PHYSICS_WORKER_MODE");
}

TEST(RuntimeConfigFileTest, ValidatesAndExportsMemoryLimitPolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path negative = temporary.Write(R"yaml(
version: 1
performance:
  memory_limit_mb: -1
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), negative);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("memory_limit_mb"), std::string::npos);

  loaded = LoadRuntimeConfig(
      MapEnvironment({{"MOCKTAIL_MEMORY_LIMIT_MB", "not-a-number"}}),
      "/does/not/exist/config.yaml");
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("memory-limit policy"), std::string::npos);

  unsetenv("MOCKTAIL_MEMORY_LIMIT_MB");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_MEMORY_LIMIT_MB", "6144"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_MEMORY_LIMIT_MB"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_MEMORY_LIMIT_MB"), "6144");
  unsetenv("MOCKTAIL_MEMORY_LIMIT_MB");
}

TEST(RuntimeConfigFileTest, ValidatesAndExportsGameModePolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path invalid = temporary.Write(R"yaml(
version: 1
performance:
  gamemode: required
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), invalid);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.gamemode"), std::string::npos);

  loaded = LoadRuntimeConfig(MapEnvironment({{"MOCKTAIL_GAMEMODE", "invalid"}}),
                             "/does/not/exist/config.yaml");
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("GameMode policy"), std::string::npos);

  unsetenv("MOCKTAIL_GAMEMODE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_GAMEMODE", "on"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_GAMEMODE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_GAMEMODE"), "on");
  unsetenv("MOCKTAIL_GAMEMODE");
}

TEST(RuntimeConfigFileTest, ExportsAudioOutputDevice) {
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "USB Headset"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE"), "USB Headset");
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
}

TEST(RuntimeConfigFileTest, ExportsAudioInputDevice) {
  unsetenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "USB Microphone"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_AUDIO_INPUT_DEVICE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_AUDIO_INPUT_DEVICE"), "USB Microphone");
  unsetenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
}

TEST(RuntimeConfigFileTest, ExportsDesktopPlayabilityUserAgent) {
  unsetenv("MOCKTAIL_USER_AGENT");
  unsetenv("MOCKTAIL_DESKTOP_PLAYABILITY");
  unsetenv("MOCKTAIL_DEVICE_PROFILE");
  std::string error;
  const RuntimeConfig desktop =
      RuntimeConfig::FromEnvironment(MapEnvironment());

  ASSERT_TRUE(ExportRuntimeConfigEnvironment(desktop, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), "1");
  ASSERT_NE(getenv("MOCKTAIL_DEVICE_PROFILE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PROFILE"), "pc-windows-11");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CLASS"), "pc");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PLATFORM_NAME"), "Windows");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_NAME"), "Windows 11 PC");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MANUFACTURER"), "Microsoft");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MODEL"), "Windows 11 PC");
  ASSERT_NE(getenv("MOCKTAIL_USER_AGENT"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_USER_AGENT"),
               kRobloxDesktopHttpUserAgent.data());
  unsetenv("MOCKTAIL_USER_AGENT");
  unsetenv("MOCKTAIL_DESKTOP_PLAYABILITY");
  unsetenv("MOCKTAIL_DEVICE_PROFILE");
}

TEST(RuntimeConfigFileTest, LoadsOneLineDevicePresetsAndAliases) {
  struct ExpectedProfile {
    const char* configured;
    const char* canonical;
    DeviceClass device_class;
    const char* model;
    bool touch;
    const char* user_agent;
  };
  for (const ExpectedProfile& expected : {
           ExpectedProfile{"pc-windows-11", "pc-windows-11", DeviceClass::kPc,
                           "Windows 11 PC", false, "Roblox/WinInet"},
           ExpectedProfile{"mobile", "mobile-pixel-7", DeviceClass::kMobile,
                           "Google Pixel 7", true, nullptr},
           ExpectedProfile{"console", "console-ps5", DeviceClass::kConsole,
                           "PlayStation 5", false, "Roblox/XboxOne"},
       }) {
    SCOPED_TRACE(expected.configured);
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.Write(
        "version: 1\ndevice: " + std::string(expected.configured) + "\n");

    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment(), file);

    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.config.device_profile().name, expected.canonical);
    EXPECT_EQ(loaded.config.device_profile().device_class,
              expected.device_class);
    EXPECT_EQ(loaded.config.device_profile().display_name, expected.model);
    EXPECT_EQ(loaded.config.input_capabilities().touch_enabled, expected.touch);
    if (expected.user_agent == nullptr) {
      EXPECT_FALSE(loaded.config.roblox_http_user_agent().has_value());
    } else {
      ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
      EXPECT_EQ(*loaded.config.roblox_http_user_agent(), expected.user_agent);
    }
  }
}

TEST(RuntimeConfigFileTest, LoadsDetailedDeviceConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
device:
  type: mobile
  platform: Android
  name: Google Pixel 9 Pro
  manufacturer: Google
  model: Pixel 9 Pro
  brand: google
  code: komodo
  sku: pixel-9-pro
  soc_model: Google Tensor G4
  touch: true
  mouse: true
  keyboard: false
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.device_profile().name, "mobile-pixel-7");
  EXPECT_EQ(loaded.config.device_profile().platform_name, "Android");
  EXPECT_EQ(loaded.config.device_profile().display_name, "Google Pixel 9 Pro");
  EXPECT_EQ(loaded.config.device_profile().manufacturer, "Google");
  EXPECT_EQ(loaded.config.device_profile().model, "Pixel 9 Pro");
  EXPECT_EQ(loaded.config.device_profile().brand, "google");
  EXPECT_EQ(loaded.config.device_profile().device_code, "komodo");
  EXPECT_EQ(loaded.config.device_profile().device_sku, "pixel-9-pro");
  EXPECT_EQ(loaded.config.device_profile().soc_model, "Google Tensor G4");
  EXPECT_NE(loaded.config.device_profile().cache_key,
            loaded.config.device_profile().name);
  EXPECT_TRUE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.input_capabilities().mouse_enabled);
  EXPECT_FALSE(loaded.config.input_capabilities().keyboard_enabled);
}

TEST(RuntimeConfigFileTest, ExportsMobileIdentityAndClearsDesktopUserAgent) {
  ASSERT_EQ(0, setenv("MOCKTAIL_USER_AGENT", "stale-desktop", 1));
  const RuntimeConfig mobile = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", "mobile-pixel-7"}}));
  std::string error;

  ASSERT_TRUE(ExportRuntimeConfigEnvironment(mobile, &error)) << error;
  EXPECT_EQ(getenv("MOCKTAIL_USER_AGENT"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PROFILE"), "mobile-pixel-7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CLASS"), "mobile");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PLATFORM_NAME"), "Android");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_NAME"), "Google Pixel 7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MODEL"), "Pixel 7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CODE"), "panther");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_SOC_MODEL"), "Google Tensor G2");
  EXPECT_STREQ(getenv("MOCKTAIL_TOUCH_MODE"), "on");
  EXPECT_STREQ(getenv("MOCKTAIL_MOUSE_MODE"), "off");
  EXPECT_STREQ(getenv("MOCKTAIL_KEYBOARD_MODE"), "off");
  EXPECT_STREQ(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), "0");
}

TEST(RuntimeConfigFileTest, EnvironmentDevicePresetOverridesYamlPreset) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
device: mobile-pixel-7
)yaml");

  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
      MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", "console"}}), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.device_profile().name, "console-ps5");
  EXPECT_EQ(loaded.config.device_profile().device_class, DeviceClass::kConsole);
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxConsoleAdmissionUserAgent);
}

TEST(RuntimeConfigFileTest, RejectsUnknownOrContradictoryDevicePreset) {
  TemporaryDirectory temporary;
  const std::filesystem::path unknown = temporary.Write(R"yaml(
version: 1
device: smart-fridge
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), unknown);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device must be"), std::string::npos);

  const std::filesystem::path contradictory = temporary.Write(R"yaml(
version: 1
device: mobile-pixel-7
input:
  touch_enabled: false
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), contradictory);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("cannot be combined"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsInvalidDetailedDeviceConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path missing_type = temporary.Write(R"yaml(
version: 1
device:
  model: Pixel 9 Pro
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), missing_type);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device.type is required"), std::string::npos);

  const std::filesystem::path empty_model = temporary.Write(R"yaml(
version: 1
device:
  type: mobile
  model: ""
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), empty_model);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device.model"), std::string::npos);

  const std::filesystem::path invalid_input = temporary.Write(R"yaml(
version: 1
device:
  type: console
  touch: false
  mouse: false
  keyboard: false
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), invalid_input);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("at least one usable input"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsMobilePlayabilityWithoutChangingTouch) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
input:
  touch_enabled: false
compatibility:
  desktop_playability: false
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_FALSE(loaded.config.desktop_playability());
  EXPECT_FALSE(loaded.config.roblox_http_user_agent().has_value());
}

TEST(RuntimeConfigFileTest, RejectsInvalidDesktopPlayabilityValue) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
compatibility:
  desktop_playability: automatic
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("compatibility.desktop_playability"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, AcceptsProductionDirectVulkanBackendName) {
  MapEnvironment environment({{"MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan"}});
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/direct-vulkan.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
}

TEST(RuntimeConfigFileTest, AcceptsStrictOpenGlBackendName) {
  MapEnvironment environment({{"MOCKTAIL_GRAPHICS_BACKEND", "opengl"}});
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/opengl.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kSystem);
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
}

TEST(RuntimeConfigFileTest, LoadsStrictOpenGlFromYamlWithoutEnvironment) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  backend: opengl
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kSystem);
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
}

TEST(RuntimeConfigFileTest, RejectsInvalidKnownSettings) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  frame_rate_limit: fastest
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("frame_rate_limit"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsNonBooleanPerformancePolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
performance:
  multithreaded_rendering: automatic
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.multithreaded_rendering"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsUnknownPhysicsWorkerMode) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
performance:
  physics_worker_mode: maximum
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.physics_worker_mode"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsMalformedYaml) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write("graphics: [\n");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("invalid YAML"), std::string::npos);
}

TEST(RuntimeConfigFileTest, FleasionRoutesThroughConfiguredLoopbackPort) {
  TemporaryDirectory temporary;
  const auto file = temporary.Write(
      "integrations:\n  fleasion:\n    enabled: true\n    proxy_mode: env\n"
      "    proxy_port: 59443\n    ca_certificate: /tmp/fleasion-ca.crt\n");
  auto loaded = LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.fleasion_enabled());
  ASSERT_TRUE(loaded.config.network_proxy());
  EXPECT_EQ(loaded.config.network_proxy()->host, "127.0.0.1");
  EXPECT_EQ(loaded.config.network_proxy()->port, 59443);
  EXPECT_EQ(*loaded.config.fleasion_ca_certificate(), "/tmp/fleasion-ca.crt");
  loaded = LoadRuntimeConfig(MapEnvironment({{"MOCKTAIL_FLEASION_ENABLED", "0"}}), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.fleasion_enabled());
  EXPECT_FALSE(loaded.config.network_proxy());
}

TEST(RuntimeConfigFileTest, FleasionRejectsInvalidConfigurationAndProxyConflicts) {
  TemporaryDirectory temporary;
  for (const char* setting : {"proxy_port: 0", "proxy_port: 65536", "proxy_port: 123x",
                              "proxy_mode: invalid", "ca_certificate: relative.pem"}) {
    const auto file = temporary.Write(std::string(
        "integrations:\n  fleasion:\n    enabled: true\n    ") + setting + "\n");
    EXPECT_FALSE(LoadRuntimeConfig(MapEnvironment(), file)) << setting;
  }
  const auto file = temporary.Write("integrations:\n  fleasion:\n    enabled: true\n");
  EXPECT_FALSE(LoadRuntimeConfig(MapEnvironment({{"MOCKTAIL_USE_SYSTEM_PROXY", "1"}}), file));
  EXPECT_FALSE(LoadRuntimeConfig(MapEnvironment({{"MOCKTAIL_HTTP_PROXY_HOST", "other"},
                                                {"MOCKTAIL_HTTP_PROXY_PORT", "3128"}}), file));
}

TEST(RuntimeConfigFileTest, FleasionEnvironmentRoundTripsAndHostsModeDoesNotSetProxy) {
  TemporaryDirectory temporary;
  const auto file = temporary.Write("integrations:\n  fleasion:\n    enabled: true\n"
                                     "    proxy_mode: hosts\n");
  const auto loaded = LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.network_proxy());
  std::string error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  const auto resolved = RuntimeConfig::FromEnvironment(ProcessEnvironment());
  EXPECT_TRUE(resolved.fleasion_enabled());
  EXPECT_TRUE(resolved.fleasion_valid());
  EXPECT_EQ(resolved.fleasion_proxy_mode(), "hosts");
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(RuntimeConfig::FromEnvironment(MapEnvironment()), &error));
}

// Saves the listed variables, clears them for the test, and puts the saved
// values back afterwards so export tests do not leak into each other.
class ScopedEnvironment {
 public:
  explicit ScopedEnvironment(std::vector<std::string> names)
      : names_(std::move(names)) {
    for (const std::string& name : names_) {
      const char* value = getenv(name.c_str());
      saved_.push_back(value == nullptr ? std::nullopt
                                        : std::optional<std::string>(value));
      unsetenv(name.c_str());
    }
  }

  ~ScopedEnvironment() {
    for (std::size_t index = 0; index < names_.size(); ++index) {
      if (saved_[index].has_value()) {
        setenv(names_[index].c_str(), saved_[index]->c_str(), 1);
      } else {
        unsetenv(names_[index].c_str());
      }
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  std::vector<std::string> names_;
  std::vector<std::optional<std::string>> saved_;
};

std::optional<std::string> GetVariable(const char* name) {
  const char* value = getenv(name);
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

TEST(RuntimeConfigFileTest, ShippedTemplateDefaultsTheLauncherSections) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.Write(DefaultRuntimeConfigYaml());

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.display().server, DisplayServer::kAuto);
  EXPECT_EQ(loaded.config.display().start_mode, WindowStartMode::kRemember);
  EXPECT_EQ(loaded.config.account().sign_in, SignInMethod::kNative);
  EXPECT_EQ(loaded.config.engine().graphics_quality, GraphicsQuality{});
  EXPECT_EQ(loaded.config.engine().gpu, GpuPreference::kAuto);
  EXPECT_TRUE(loaded.config.engine().nvidia_shader_mt);
  EXPECT_TRUE(loaded.config.launcher().show_on_start);
  // New keys live in new top-level sections so that older Mocktail builds,
  // which ignore unknown sections, still start with this file.
  const std::string defaults(DefaultRuntimeConfigYaml());
  for (const std::string_view section :
       {"\ndisplay:\n", "\naccount:\n", "\nengine:\n", "\nlauncher:\n"}) {
    EXPECT_NE(defaults.find(section), std::string::npos) << section;
  }
}

TEST(RuntimeConfigFileTest, LoadsEveryLauncherManagedValue) {
  TemporaryDirectory temporary;
  struct Case {
    const char* yaml;
    DisplayServer server;
    WindowStartMode start_mode;
    SignInMethod sign_in;
    GraphicsQuality quality;
    bool nvidia_shader_mt;
    bool show_on_start;
  };
  for (const Case& entry : {
           Case{"display:\n  server: auto\n  start_mode: remember\n"
                "account:\n  sign_in: native\n"
                "engine:\n  graphics_quality: default\n"
                "  nvidia_shader_mt: true\n"
                "launcher:\n  show_on_start: true\n",
                DisplayServer::kAuto, WindowStartMode::kRemember,
                SignInMethod::kNative, GraphicsQuality{}, true, true},
           Case{"display:\n  server: wayland\n  start_mode: windowed\n"
                "account:\n  sign_in: browser\n"
                "engine:\n  graphics_quality: manual\n"
                "  nvidia_shader_mt: false\n"
                "launcher:\n  show_on_start: false\n",
                DisplayServer::kWayland, WindowStartMode::kWindowed,
                SignInMethod::kBrowser,
                GraphicsQuality{GraphicsQualityMode::kManual, 0}, false,
                false},
           Case{"display:\n  server: x11\n  start_mode: maximized\n"
                "engine:\n  graphics_quality: 1\n",
                DisplayServer::kX11, WindowStartMode::kMaximized,
                SignInMethod::kNative,
                GraphicsQuality{GraphicsQualityMode::kLevel, 1}, true, true},
           Case{"display:\n  start_mode: fullscreen\n"
                "engine:\n  graphics_quality: \"21\"\n",
                DisplayServer::kAuto, WindowStartMode::kFullscreen,
                SignInMethod::kNative,
                GraphicsQuality{GraphicsQualityMode::kLevel, 21}, true, true},
       }) {
    const std::filesystem::path file =
        temporary.Write(std::string("version: 1\n") + entry.yaml);
    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment(), file);
    ASSERT_TRUE(loaded) << loaded.error << '\n' << entry.yaml;
    EXPECT_EQ(loaded.config.display().server, entry.server) << entry.yaml;
    EXPECT_EQ(loaded.config.display().start_mode, entry.start_mode)
        << entry.yaml;
    EXPECT_EQ(loaded.config.account().sign_in, entry.sign_in) << entry.yaml;
    EXPECT_EQ(loaded.config.engine().graphics_quality, entry.quality)
        << entry.yaml;
    EXPECT_EQ(loaded.config.engine().nvidia_shader_mt, entry.nvidia_shader_mt)
        << entry.yaml;
    EXPECT_EQ(loaded.config.launcher().show_on_start, entry.show_on_start)
        << entry.yaml;
  }
}

TEST(RuntimeConfigFileTest, LoadsTheGraphicsCardPreference) {
  TemporaryDirectory temporary;
  for (const auto& [value, expected] : {
           std::pair<const char*, GpuPreference>("auto", GpuPreference::kAuto),
           {"discrete", GpuPreference::kDiscrete},
           {"integrated", GpuPreference::kIntegrated},
           {"\"integrated\"", GpuPreference::kIntegrated},
       }) {
    const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
        MapEnvironment(), temporary.Write(std::string("version: 1\nengine:\n"
                                                      "  gpu: ") +
                                          value + "\n"));
    ASSERT_TRUE(loaded) << loaded.error << '\n' << value;
    EXPECT_EQ(loaded.config.engine().gpu, expected) << value;
    EXPECT_EQ(loaded.config.engine().graphics_quality, GraphicsQuality{})
        << value;
  }
}

TEST(RuntimeConfigFileTest, RejectsInvalidLauncherManagedValuesByKey) {
  TemporaryDirectory temporary;
  struct Case {
    const char* section;
    const char* key;
    const char* value;
  };
  for (const Case& entry : {
           Case{"display", "server", "X11"},
           Case{"display", "server", "xwayland"},
           Case{"display", "server", "\"\""},
           Case{"display", "start_mode", "maximised"},
           Case{"display", "start_mode", "true"},
           Case{"account", "sign_in", "webview"},
           Case{"account", "sign_in", "0"},
           Case{"engine", "graphics_quality", "0"},
           Case{"engine", "graphics_quality", "22"},
           Case{"engine", "graphics_quality", "-1"},
           Case{"engine", "graphics_quality", "3.5"},
           Case{"engine", "graphics_quality", "auto"},
           Case{"engine", "graphics_quality", "high"},
           Case{"engine", "graphics_quality", "\"\""},
           Case{"engine", "gpu", "igpu"},
           Case{"engine", "gpu", "Discrete"},
           Case{"engine", "gpu", "nvidia"},
           Case{"engine", "gpu", "\"\""},
           Case{"engine", "nvidia_shader_mt", "off"},
           Case{"engine", "nvidia_shader_mt", "0"},
           Case{"engine", "nvidia_shader_mt", "auto"},
           Case{"engine", "nvidia_shader_mt", "False"},
           Case{"launcher", "show_on_start", "yes"},
           Case{"launcher", "show_on_start", "1"},
           Case{"launcher", "show_on_start", "True"},
       }) {
    const std::filesystem::path file = temporary.Write(
        std::string("version: 1\n") + entry.section + ":\n  " + entry.key +
        ": " + entry.value + "\n");
    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment(), file);
    const std::string key = std::string(entry.section) + "." + entry.key;
    EXPECT_FALSE(loaded) << key << ": " << entry.value;
    EXPECT_NE(loaded.error.find(key), std::string::npos)
        << key << ": " << entry.value << " -> " << loaded.error;
  }
}

TEST(RuntimeConfigFileTest, RejectsUnknownKeysAndNonMappingsInNewSections) {
  TemporaryDirectory temporary;
  for (const char* key :
       {"display.video_driver", "account.selected", "engine.msaa",
        "launcher.last_page"}) {
    const std::string dotted(key);
    const std::size_t dot = dotted.find('.');
    const std::filesystem::path file = temporary.Write(
        "version: 1\n" + dotted.substr(0, dot) + ":\n  " +
        dotted.substr(dot + 1) + ": true\n");
    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment(), file);
    EXPECT_FALSE(loaded) << key;
    EXPECT_EQ(loaded.error, "unknown runtime configuration key: " + dotted);
  }
  for (const char* section : {"display", "account", "engine", "launcher"}) {
    for (const char* body : {":\n", ": auto\n", ":\n  server:\n    - x11\n"}) {
      const std::filesystem::path file =
          temporary.Write(std::string("version: 1\n") + section + body);
      const RuntimeConfigLoadResult loaded =
          LoadRuntimeConfig(MapEnvironment(), file);
      EXPECT_FALSE(loaded) << section << body;
      EXPECT_NE(loaded.error.find(section), std::string::npos)
          << section << body << " -> " << loaded.error;
    }
  }
}

TEST(RuntimeConfigFileTest, EnvironmentOverridesLauncherManagedYaml) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
display:
  server: x11
  start_mode: maximized
account:
  sign_in: native
engine:
  graphics_quality: 12
  gpu: discrete
  nvidia_shader_mt: true
launcher:
  show_on_start: true
)yaml");

  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
      MapEnvironment({
          {"MOCKTAIL_DISPLAY_SERVER", "wayland"},
          {"MOCKTAIL_WINDOW_START_MODE", "fullscreen"},
          {"MOCKTAIL_NATIVE_LOGIN", "0"},
          {"MOCKTAIL_GRAPHICS_QUALITY", "manual"},
          {"MOCKTAIL_GPU", "integrated"},
          {"MOCKTAIL_NVIDIA_SHADER_MT", "off"},
          {"MOCKTAIL_LAUNCHER_SHOW_ON_START", "0"},
      }),
      file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.display().server, DisplayServer::kWayland);
  EXPECT_EQ(loaded.config.display().start_mode, WindowStartMode::kFullscreen);
  EXPECT_EQ(loaded.config.account().sign_in, SignInMethod::kBrowser);
  EXPECT_EQ(loaded.config.engine().graphics_quality.mode,
            GraphicsQualityMode::kManual);
  EXPECT_EQ(loaded.config.engine().gpu, GpuPreference::kIntegrated);
  EXPECT_FALSE(loaded.config.engine().nvidia_shader_mt);
  EXPECT_FALSE(loaded.config.launcher().show_on_start);

  // The legacy sign-in variable only ever meant "browser" for exactly "0",
  // and the rendering preset always read auto and 0 as "leave the slider".
  for (const char* native : {"1", "yes", "native"}) {
    loaded = LoadRuntimeConfig(
        MapEnvironment({{"MOCKTAIL_NATIVE_LOGIN", native}}),
        temporary.Write("version: 1\naccount:\n  sign_in: browser\n"));
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.config.account().sign_in, SignInMethod::kNative)
        << native;
  }
  for (const char* manual : {"auto", "0", "manual"}) {
    loaded = LoadRuntimeConfig(
        MapEnvironment({{"MOCKTAIL_GRAPHICS_QUALITY", manual}}), file);
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.config.engine().graphics_quality.mode,
              GraphicsQualityMode::kManual)
        << manual;
  }
  loaded = LoadRuntimeConfig(
      MapEnvironment({{"MOCKTAIL_GRAPHICS_QUALITY", "1"}}), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.engine().graphics_quality,
            (GraphicsQuality{GraphicsQualityMode::kLevel, 1}));

  // A present but empty variable hides the YAML value, as everywhere else.
  loaded = LoadRuntimeConfig(MapEnvironment({
                                 {"MOCKTAIL_DISPLAY_SERVER", ""},
                                 {"MOCKTAIL_WINDOW_START_MODE", ""},
                                 {"MOCKTAIL_GRAPHICS_QUALITY", ""},
                                 {"MOCKTAIL_GPU", ""},
                                 {"MOCKTAIL_NVIDIA_SHADER_MT", ""},
                                 {"MOCKTAIL_LAUNCHER_SHOW_ON_START", ""},
                             }),
                             temporary.Write(R"yaml(
version: 1
display:
  server: x11
  start_mode: windowed
engine:
  graphics_quality: 4
  gpu: integrated
  nvidia_shader_mt: false
launcher:
  show_on_start: false
)yaml"));
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.display().server, DisplayServer::kAuto);
  EXPECT_EQ(loaded.config.display().start_mode, WindowStartMode::kRemember);
  EXPECT_EQ(loaded.config.engine().graphics_quality, GraphicsQuality{});
  EXPECT_EQ(loaded.config.engine().gpu, GpuPreference::kAuto);
  EXPECT_TRUE(loaded.config.engine().nvidia_shader_mt);
  EXPECT_TRUE(loaded.config.launcher().show_on_start);
}

TEST(RuntimeConfigFileTest, RejectsInvalidLauncherManagedVariables) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write("version: 1\n");
  for (const auto& [name, value] : {
           std::pair<const char*, const char*>("MOCKTAIL_DISPLAY_SERVER",
                                               "xorg"),
           {"MOCKTAIL_WINDOW_START_MODE", "minimized"},
           {"MOCKTAIL_GRAPHICS_QUALITY", "25"},
           {"MOCKTAIL_GRAPHICS_QUALITY", "ultra"},
           {"MOCKTAIL_GPU", "igpu"},
           {"MOCKTAIL_GPU", "nvidia"},
           {"MOCKTAIL_NVIDIA_SHADER_MT", "auto"},
           {"MOCKTAIL_LAUNCHER_SHOW_ON_START", "maybe"},
       }) {
    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment({{name, value}}), file);
    EXPECT_FALSE(loaded) << name << '=' << value;
    EXPECT_NE(loaded.error.find(name), std::string::npos)
        << name << '=' << value << " -> " << loaded.error;

    std::string error;
    const RuntimeConfig invalid =
        RuntimeConfig::FromEnvironment(MapEnvironment({{name, value}}));
    EXPECT_FALSE(ExportRuntimeConfigEnvironment(invalid, &error))
        << name << '=' << value;
    EXPECT_FALSE(error.empty());
  }
}

TEST(RuntimeConfigFileTest, ExportsLauncherManagedSettings) {
  const ScopedEnvironment scoped({
      "MOCKTAIL_DISPLAY_SERVER",
      "MOCKTAIL_WINDOW_START_MODE",
      "MOCKTAIL_NATIVE_LOGIN",
      "MOCKTAIL_GRAPHICS_QUALITY",
      "MOCKTAIL_GPU",
      "MOCKTAIL_NVIDIA_SHADER_MT",
      "MOCKTAIL_LAUNCHER_SHOW_ON_START",
  });
  TemporaryDirectory temporary;
  std::string error;
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
      MapEnvironment(), temporary.Write(R"yaml(
version: 1
display:
  server: wayland
  start_mode: fullscreen
account:
  sign_in: browser
engine:
  graphics_quality: 7
  gpu: integrated
  nvidia_shader_mt: false
launcher:
  show_on_start: false
)yaml"));
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_DISPLAY_SERVER"), "wayland");
  EXPECT_EQ(GetVariable("MOCKTAIL_WINDOW_START_MODE"), "fullscreen");
  EXPECT_EQ(GetVariable("MOCKTAIL_NATIVE_LOGIN"), "0");
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_QUALITY"), "7");
  EXPECT_EQ(GetVariable("MOCKTAIL_GPU"), "integrated");
  EXPECT_EQ(GetVariable("MOCKTAIL_NVIDIA_SHADER_MT"), "0");
  EXPECT_EQ(GetVariable("MOCKTAIL_LAUNCHER_SHOW_ON_START"), "0");

  // The exported values read back to the same settings, which is what every
  // later reader in the game process relies on.
  const RuntimeConfig resolved =
      RuntimeConfig::FromEnvironment(ProcessEnvironment());
  EXPECT_EQ(resolved.display().server, DisplayServer::kWayland);
  EXPECT_EQ(resolved.display().start_mode, WindowStartMode::kFullscreen);
  EXPECT_EQ(resolved.account().sign_in, SignInMethod::kBrowser);
  EXPECT_EQ(resolved.engine().graphics_quality,
            (GraphicsQuality{GraphicsQualityMode::kLevel, 7}));
  EXPECT_EQ(resolved.engine().gpu, GpuPreference::kIntegrated);
  EXPECT_FALSE(resolved.engine().nvidia_shader_mt);
  EXPECT_FALSE(resolved.launcher().show_on_start);

  loaded = LoadRuntimeConfig(
      MapEnvironment(),
      temporary.Write("version: 1\nengine:\n  graphics_quality: manual\n"));
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_DISPLAY_SERVER"), "auto");
  EXPECT_EQ(GetVariable("MOCKTAIL_WINDOW_START_MODE"), "remember");
  EXPECT_EQ(GetVariable("MOCKTAIL_NATIVE_LOGIN"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_QUALITY"), "manual");
  EXPECT_EQ(GetVariable("MOCKTAIL_GPU"), "auto");
  EXPECT_EQ(GetVariable("MOCKTAIL_NVIDIA_SHADER_MT"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_LAUNCHER_SHOW_ON_START"), "1");
}

TEST(RuntimeConfigFileTest, DefaultGraphicsQualityLeavesThePresetInCharge) {
  const ScopedEnvironment scoped({"MOCKTAIL_GRAPHICS_QUALITY"});
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.Write("version: 1\nengine:\n  graphics_quality: default\n");
  std::string error;

  // Unset stays unset, so the preset keeps its level 3.
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(ProcessEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_QUALITY"), std::nullopt);

  // The Intel integrated graphics level the graphics launch policy
  // publishes survives.
  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_QUALITY", "1", 1), 0);
  loaded = LoadRuntimeConfig(ProcessEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_QUALITY"), "1");

  // The preset does not know the word "default"; it must never see it.
  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_QUALITY", "default", 1), 0);
  loaded = LoadRuntimeConfig(ProcessEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.engine().graphics_quality, GraphicsQuality{});
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(loaded.config, &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_QUALITY"), std::nullopt);
}

// ApplyGraphicsLaunchPolicy with a system backend, which publishes only the
// backend switches and display.server.
class DisplayServerPolicyTest : public ::testing::Test {
 protected:
  RuntimeConfig Load(std::string_view server,
                     const MapEnvironment& environment = MapEnvironment()) {
    const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
        environment,
        temporary_.Write("version: 1\ngraphics:\n  backend: system\n"
                         "display:\n  server: " +
                         std::string(server) + "\n"));
    EXPECT_TRUE(loaded) << loaded.error;
    return loaded.config;
  }

  bool Apply(const RuntimeConfig& config,
             const std::vector<std::string>& user_environment) {
    std::string error;
    const bool applied =
        ApplyGraphicsLaunchPolicy(config, user_environment, &error);
    EXPECT_TRUE(applied) << error;
    return applied;
  }

  void SetUp() override {
    // A desktop session that offers both display servers.
    ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-1", 1), 0);
    ASSERT_EQ(setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1), 0);
    ASSERT_EQ(setenv("DISPLAY", ":0", 1), 0);
  }

  TemporaryDirectory temporary_;
  const ScopedEnvironment scoped_{{
      "MOCKTAIL_GRAPHICS_BACKEND",
      "MOCKTAIL_PRELOAD_VULKAN_SHIM",
      "MOCKTAIL_REQUIRE_REAL_GRAPHICS",
      "MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK",
      "MOCKTAIL_SOFTWARE_WINDOW_FALLBACK",
      "SDL_VIDEODRIVER",
      "SDL_VIDEO_DRIVER",
      "MOCKTAIL_FORCE_WAYLAND",
      "MOCKTAIL_FORCE_X11",
      "MOCKTAIL_ANGLE_FORCE_X11",
      "WAYLAND_DISPLAY",
      "XDG_RUNTIME_DIR",
      "DISPLAY",
  }};
};

TEST_F(DisplayServerPolicyTest, WaylandForcesWaylandAndClearsX11Switches) {
  // Left over from Mocktail itself, not from the user's environment.
  ASSERT_EQ(setenv("MOCKTAIL_FORCE_X11", "1", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ANGLE_FORCE_X11", "1", 1), 0);
  ASSERT_TRUE(Apply(Load("wayland"), {}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);
  EXPECT_EQ(GetVariable("MOCKTAIL_ANGLE_FORCE_X11"), std::nullopt);
  EXPECT_EQ(GetVariable("SDL_VIDEODRIVER"), std::nullopt);
  EXPECT_EQ(GetVariable("MOCKTAIL_GRAPHICS_BACKEND"), "system");
}

TEST_F(DisplayServerPolicyTest, X11ForcesX11AndClearsWayland) {
  ASSERT_EQ(setenv("MOCKTAIL_FORCE_WAYLAND", "1", 1), 0);
  ASSERT_TRUE(Apply(Load("x11"), {}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
  EXPECT_EQ(GetVariable("SDL_VIDEODRIVER"), std::nullopt);
}

TEST_F(DisplayServerPolicyTest, AutoTouchesNothing) {
  ASSERT_TRUE(Apply(Load("auto"), {}));
  for (const std::string_view name : kUserVideoDriverVariables) {
    EXPECT_EQ(GetVariable(std::string(name).c_str()), std::nullopt) << name;
  }
  ASSERT_EQ(setenv("MOCKTAIL_FORCE_X11", "1", 1), 0);
  ASSERT_TRUE(Apply(Load("auto"), {}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
}

TEST_F(DisplayServerPolicyTest, UserVideoDriverVariablesWin) {
  // The user's shortcut runs `env SDL_VIDEODRIVER=x11 mocktail`.
  ASSERT_EQ(setenv("SDL_VIDEODRIVER", "x11", 1), 0);
  ASSERT_TRUE(Apply(Load("wayland"), {"SDL_VIDEODRIVER"}));
  EXPECT_EQ(GetVariable("SDL_VIDEODRIVER"), "x11");
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);
  ASSERT_EQ(unsetenv("SDL_VIDEODRIVER"), 0);

  for (const std::string_view name : kUserVideoDriverVariables) {
    const std::string owned(name);
    // Even an empty or "0" value is the user's own decision.
    for (const char* value : {"1", "0", ""}) {
      ASSERT_EQ(setenv(owned.c_str(), value, 1), 0);
      ASSERT_TRUE(Apply(Load("x11"), {"MOCKTAIL_VSYNC", owned}));
      EXPECT_EQ(GetVariable(owned.c_str()), value) << name;
      for (const std::string_view other : kUserVideoDriverVariables) {
        if (other != name) {
          EXPECT_EQ(GetVariable(std::string(other).c_str()), std::nullopt)
              << name << " / " << other;
        }
      }
      ASSERT_EQ(unsetenv(owned.c_str()), 0);
    }
  }
}

TEST_F(DisplayServerPolicyTest, VariablesRemovedSinceStartNoLongerWin) {
  // The settings window asked main to ignore the user's overrides, so the
  // captured SDL_VIDEODRIVER was unset before the config was loaded.
  ASSERT_TRUE(Apply(Load("wayland"), {"SDL_VIDEODRIVER"}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), "1");
  EXPECT_FALSE(UserSelectsVideoDriver({"SDL_VIDEODRIVER"}));
  // Only variables captured from the user's environment count.
  EXPECT_FALSE(UserSelectsVideoDriver({}));
  EXPECT_TRUE(UserSelectsVideoDriver({"MOCKTAIL_FORCE_WAYLAND"}));
}

TEST_F(DisplayServerPolicyTest, DisplayServerVariableIsAnOrdinarySetting) {
  // MOCKTAIL_DISPLAY_SERVER overrides the YAML value like any other setting
  // and is applied through the same switches.
  const RuntimeConfig config =
      Load("wayland", MapEnvironment({{"MOCKTAIL_DISPLAY_SERVER", "x11"}}));
  EXPECT_EQ(config.display().server, DisplayServer::kX11);
  ASSERT_TRUE(Apply(config, {"MOCKTAIL_DISPLAY_SERVER"}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), "1");
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
}

TEST_F(DisplayServerPolicyTest, OverloadWithoutCaptureTreatsCurrentAsUsers) {
  ASSERT_EQ(setenv("SDL_VIDEO_DRIVER", "wayland", 1), 0);
  std::string error;
  ASSERT_TRUE(ApplyGraphicsLaunchPolicy(Load("x11"), &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);
  ASSERT_EQ(unsetenv("SDL_VIDEO_DRIVER"), 0);
  ASSERT_TRUE(ApplyGraphicsLaunchPolicy(Load("x11"), &error)) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), "1");
}

TEST_F(DisplayServerPolicyTest, MissingSessionFallsBackToAutomatic) {
  // A Wayland setting saved on the desktop, then a start from an X11-only
  // session: forcing Wayland there would stop SDL from starting at all.
  ASSERT_EQ(unsetenv("WAYLAND_DISPLAY"), 0);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kWayland),
            DisplayServer::kAuto);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kX11), DisplayServer::kX11);
  ASSERT_TRUE(Apply(Load("wayland"), {}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);

  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-1", 1), 0);
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", "", 1), 0);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kWayland),
            DisplayServer::kAuto);
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1), 0);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kWayland),
            DisplayServer::kWayland);

  // A Wayland compositor without Xwayland has no X display.
  ASSERT_EQ(unsetenv("DISPLAY"), 0);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kX11), DisplayServer::kAuto);
  ASSERT_TRUE(Apply(Load("x11"), {}));
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);
  EXPECT_EQ(AvailableDisplayServer(DisplayServer::kAuto), DisplayServer::kAuto);
}

TEST_F(DisplayServerPolicyTest, RefusesAnInvalidDisplayServer) {
  const RuntimeConfig invalid = RuntimeConfig::FromEnvironment(MapEnvironment(
      {{"MOCKTAIL_GRAPHICS_BACKEND", "system"},
       {"MOCKTAIL_DISPLAY_SERVER", "mir"}}));
  std::string error;
  EXPECT_FALSE(ApplyGraphicsLaunchPolicy(invalid, {}, &error));
  EXPECT_NE(error.find("display server"), std::string::npos) << error;
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_WAYLAND"), std::nullopt);
  EXPECT_EQ(GetVariable("MOCKTAIL_FORCE_X11"), std::nullopt);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
