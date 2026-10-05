#include "runtime/managed_environment.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/graphics_launch_policy.h"
#include "runtime/runtime_config_file.h"

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

class TemporaryConfig {
 public:
  TemporaryConfig() {
    char pattern[] = "/tmp/mocktail_managed_environment_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      directory_ = created;
    }
  }

  ~TemporaryConfig() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  std::filesystem::path Write(const std::string& contents) const {
    const std::filesystem::path file = directory_ / "config.yaml";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << contents;
    return file;
  }

 private:
  std::filesystem::path directory_;
};

std::string Quoted(std::string_view value) {
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('"');
  return quoted;
}

// Builds `a:\n  b:\n    c: "value"\n` for the dotted key a.b.c.
std::string YamlFor(std::string_view dotted, std::string_view value) {
  std::string yaml;
  std::string indent;
  std::size_t begin = 0;
  while (true) {
    const std::size_t dot = dotted.find('.', begin);
    if (dot == std::string_view::npos) {
      yaml += indent + std::string(dotted.substr(begin)) + ": " +
              Quoted(value) + "\n";
      return yaml;
    }
    yaml += indent + std::string(dotted.substr(begin, dot - begin)) + ":\n";
    indent += "  ";
    begin = dot + 1;
  }
}

std::optional<std::string> Import(std::string_view name,
                                  std::string_view value) {
  return ImportManagedEnvironmentValue(name, value);
}

TEST(ManagedEnvironmentTest, CoversEverySettingTheLauncherNeeds) {
  const std::unordered_map<std::string, std::string> required = {
      {"SDL_VIDEODRIVER", "display.server"},
      {"SDL_VIDEO_DRIVER", "display.server"},
      {"MOCKTAIL_FORCE_WAYLAND", "display.server"},
      {"MOCKTAIL_FORCE_X11", "display.server"},
      {"MOCKTAIL_ANGLE_FORCE_X11", "display.server"},
      {"MOCKTAIL_DISPLAY_SERVER", "display.server"},
      {"MOCKTAIL_WINDOW_START_MODE", "display.start_mode"},
      {"MOCKTAIL_NATIVE_LOGIN", "account.sign_in"},
      {"MOCKTAIL_GRAPHICS_QUALITY", "engine.graphics_quality"},
      {"MOCKTAIL_GPU", "engine.gpu"},
      {"MOCKTAIL_NVIDIA_SHADER_MT", "engine.nvidia_shader_mt"},
      {"MOCKTAIL_LAUNCHER_SHOW_ON_START", "launcher.show_on_start"},
      {"MOCKTAIL_VSYNC", "graphics.vsync"},
      {"MOCKTAIL_GRAPHICS_BACKEND", "graphics.backend"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "graphics.frame_rate_limit"},
      {"MOCKTAIL_WIN_WIDTH", "window.width"},
      {"MOCKTAIL_WIN_HEIGHT", "window.height"},
      {"MOCKTAIL_WIN_TITLE", "window.title"},
      {"MOCKTAIL_WIN_HIGH_DPI", "window.high_dpi"},
      {"MOCKTAIL_THEME", "appearance.theme"},
      {"MOCKTAIL_MULTITHREADED_RENDERING",
       "performance.multithreaded_rendering"},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "performance.physics_worker_mode"},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "performance.memory_limit_mb"},
      {"MOCKTAIL_GAMEMODE", "performance.gamemode"},
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "audio.output_device"},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "audio.input_device"},
      {"MOCKTAIL_DEVICE_PROFILE", "device"},
  };
  for (const auto& [name, key] : required) {
    const ManagedEnvironmentVariable* variable =
        FindManagedEnvironmentVariable(name);
    ASSERT_NE(variable, nullptr) << name;
    EXPECT_EQ(variable->yaml_key, key) << name;
    EXPECT_NE(variable->importer, nullptr) << name;
  }
  EXPECT_EQ(FindManagedEnvironmentVariable("MOCKTAIL_HEADLESS"), nullptr);
  EXPECT_EQ(FindManagedEnvironmentVariable("HOME"), nullptr);
  EXPECT_EQ(FindManagedEnvironmentVariable(""), nullptr);
}

TEST(ManagedEnvironmentTest, ListsEveryUserVideoDriverVariable) {
  // display.server steps aside for these; the settings window must be able
  // to show them as overriding it.
  for (const std::string_view name : kUserVideoDriverVariables) {
    const ManagedEnvironmentVariable* variable =
        FindManagedEnvironmentVariable(name);
    ASSERT_NE(variable, nullptr) << name;
    EXPECT_EQ(variable->yaml_key, "display.server") << name;
  }
}

TEST(ManagedEnvironmentTest, NamesAreUniqueAndEveryEntryCanImport) {
  std::set<std::string_view> names;
  for (const ManagedEnvironmentVariable& variable :
       ManagedEnvironmentVariables()) {
    EXPECT_TRUE(names.insert(variable.name).second) << variable.name;
    EXPECT_FALSE(variable.yaml_key.empty()) << variable.name;
    EXPECT_NE(variable.importer, nullptr) << variable.name;
    // An empty variable carries no setting of its own.
    EXPECT_EQ(Import(variable.name, ""), std::nullopt) << variable.name;
  }
}

// Every importer must produce a value the real loader accepts for its key,
// and that value must resolve to the same setting as the variable did.
TEST(ManagedEnvironmentTest, ImportedValuesLoadToTheSameSettings) {
  struct Case {
    const char* name;
    const char* value;
  };
  const std::vector<Case> cases = {
      {"MOCKTAIL_GRAPHICS_BACKEND", "vulkan"},
      {"MOCKTAIL_GRAPHICS_QUALITY", "auto"},
      {"MOCKTAIL_GPU", "integrated"},
      {"MOCKTAIL_NVIDIA_SHADER_MT", "off"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "165"},
      {"MOCKTAIL_VSYNC", "off"},
      {"MOCKTAIL_WINDOW_START_MODE", "maximized"},
      {"MOCKTAIL_WIN_WIDTH", "1600"},
      {"MOCKTAIL_WIN_HEIGHT", "900"},
      {"MOCKTAIL_WIN_TITLE", "Roblox \"Mocktail\": #1"},
      {"MOCKTAIL_WIN_HIGH_DPI", "on"},
      {"MOCKTAIL_DISPLAY_SERVER", "x11"},
      {"MOCKTAIL_THEME", "dark"},
      {"MOCKTAIL_MULTITHREADED_RENDERING", "1"},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "latency"},
      {"MOCKTAIL_GAMEMODE", "1"},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "6144"},
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Наушники USB"},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "disabled"},
      {"MOCKTAIL_NATIVE_LOGIN", "0"},
      {"MOCKTAIL_DISCORD_RPC_ENABLED", "true"},
      {"MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME", "0"},
      {"MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME", "off"},
      {"MOCKTAIL_DISCORD_RPC_JOIN_ENABLED", "false"},
      {"MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY", "0"},
      {"MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL", "Join me"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_BROWSING", "Looking around"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_JOINING", "On my way"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_PLAYING", "{place_name}"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_STATE", "Playing"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE", "Somewhere"},
      {"MOCKTAIL_DISCORD_APPLICATION_ID", "123456789012345678"},
      {"MOCKTAIL_FLEASION_ENABLED", "0"},
      {"MOCKTAIL_FLEASION_PROXY_MODE", "hosts"},
      {"MOCKTAIL_FLEASION_PROXY_PORT", "59443"},
      {"MOCKTAIL_FLEASION_CA_CERTIFICATE", "/tmp/fleasion/ca.crt"},
      {"MOCKTAIL_USE_SYSTEM_PROXY", "1"},
      {"MOCKTAIL_HTTP_PROXY_HOST", "proxy.example.test"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "3128"},
      {"MOCKTAIL_CA_BUNDLE", "/etc/ssl/custom.pem"},
      {"MOCKTAIL_DEVICE_PROFILE", "mobile"},
      {"MOCKTAIL_LAUNCHER_SHOW_ON_START", "false"},
  };
  std::set<std::string_view> covered;
  TemporaryConfig temporary;
  for (const Case& entry : cases) {
    const ManagedEnvironmentVariable* variable =
        FindManagedEnvironmentVariable(entry.name);
    ASSERT_NE(variable, nullptr) << entry.name;
    covered.insert(variable->name);
    const std::optional<std::string> imported = Import(entry.name, entry.value);
    ASSERT_TRUE(imported.has_value()) << entry.name << '=' << entry.value;

    std::string yaml = "version: 1\n" + YamlFor(variable->yaml_key, *imported);
    std::unordered_map<std::string, std::string> variables = {
        {entry.name, entry.value}};
    // The fixed proxy is only valid as a host and port pair.
    if (variable->yaml_key == "network.proxy_host") {
      yaml = "version: 1\nnetwork:\n  proxy_host: " + Quoted(*imported) +
             "\n  proxy_port: 3128\n";
      variables.emplace("MOCKTAIL_HTTP_PROXY_PORT", "3128");
    } else if (variable->yaml_key == "network.proxy_port") {
      yaml = "version: 1\nnetwork:\n  proxy_host: proxy.example.test\n"
             "  proxy_port: " +
             Quoted(*imported) + "\n";
      variables.emplace("MOCKTAIL_HTTP_PROXY_HOST", "proxy.example.test");
    }
    const RuntimeConfigLoadResult from_yaml =
        LoadRuntimeConfig(MapEnvironment(), temporary.Write(yaml));
    ASSERT_TRUE(from_yaml) << entry.name << ": " << from_yaml.error << '\n'
                           << yaml;
    const RuntimeConfigLoadResult from_variable = LoadRuntimeConfig(
        MapEnvironment(variables), temporary.Write("version: 1\n"));
    ASSERT_TRUE(from_variable) << entry.name << ": " << from_variable.error;

    const RuntimeConfig& a = from_yaml.config;
    const RuntimeConfig& b = from_variable.config;
    // Backend aliases import as their canonical name.
    EXPECT_EQ(a.graphics_backend(), b.graphics_backend()) << entry.name;
    EXPECT_EQ(a.engine().graphics_quality, b.engine().graphics_quality)
        << entry.name;
    EXPECT_EQ(a.engine().gpu, b.engine().gpu) << entry.name;
    EXPECT_EQ(a.engine().nvidia_shader_mt, b.engine().nvidia_shader_mt)
        << entry.name;
    EXPECT_EQ(a.frame_rate().mode, b.frame_rate().mode) << entry.name;
    EXPECT_EQ(a.frame_rate().fixed_fps, b.frame_rate().fixed_fps)
        << entry.name;
    EXPECT_EQ(a.vsync_mode(), b.vsync_mode()) << entry.name;
    EXPECT_EQ(a.display().start_mode, b.display().start_mode) << entry.name;
    EXPECT_EQ(a.display().server, b.display().server) << entry.name;
    EXPECT_EQ(a.window().width, b.window().width) << entry.name;
    EXPECT_EQ(a.window().height, b.window().height) << entry.name;
    EXPECT_EQ(a.window().title, b.window().title) << entry.name;
    EXPECT_EQ(a.window().high_dpi, b.window().high_dpi) << entry.name;
    EXPECT_EQ(a.theme_mode(), b.theme_mode()) << entry.name;
    EXPECT_EQ(a.performance().multithreaded_rendering,
              b.performance().multithreaded_rendering)
        << entry.name;
    EXPECT_EQ(a.performance().physics_worker_mode,
              b.performance().physics_worker_mode)
        << entry.name;
    EXPECT_EQ(a.performance().game_mode, b.performance().game_mode)
        << entry.name;
    EXPECT_EQ(a.performance().memory_limit_mb, b.performance().memory_limit_mb)
        << entry.name;
    EXPECT_EQ(a.audio_output_device(), b.audio_output_device()) << entry.name;
    EXPECT_EQ(a.audio_input_device(), b.audio_input_device()) << entry.name;
    EXPECT_EQ(a.account().sign_in, b.account().sign_in) << entry.name;
    EXPECT_EQ(a.discord_rpc().enabled, b.discord_rpc().enabled) << entry.name;
    EXPECT_EQ(a.discord_rpc().show_place_name, b.discord_rpc().show_place_name)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().show_elapsed_time,
              b.discord_rpc().show_elapsed_time)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().join_enabled, b.discord_rpc().join_enabled)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().public_servers_only,
              b.discord_rpc().public_servers_only)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().join_button_label,
              b.discord_rpc().join_button_label)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().application_id, b.discord_rpc().application_id)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().text.browsing, b.discord_rpc().text.browsing)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().text.joining, b.discord_rpc().text.joining)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().text.playing, b.discord_rpc().text.playing)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().text.state, b.discord_rpc().text.state)
        << entry.name;
    EXPECT_EQ(a.discord_rpc().text.unknown_place,
              b.discord_rpc().text.unknown_place)
        << entry.name;
    EXPECT_EQ(a.fleasion_enabled(), b.fleasion_enabled()) << entry.name;
    EXPECT_EQ(a.fleasion_proxy_mode(), b.fleasion_proxy_mode()) << entry.name;
    EXPECT_EQ(a.fleasion_proxy_port(), b.fleasion_proxy_port()) << entry.name;
    EXPECT_EQ(a.fleasion_ca_certificate(), b.fleasion_ca_certificate())
        << entry.name;
    EXPECT_EQ(a.use_system_proxy(), b.use_system_proxy()) << entry.name;
    EXPECT_EQ(a.network_proxy().has_value(), b.network_proxy().has_value())
        << entry.name;
    if (a.network_proxy().has_value() && b.network_proxy().has_value()) {
      EXPECT_EQ(a.network_proxy()->host, b.network_proxy()->host);
      EXPECT_EQ(a.network_proxy()->port, b.network_proxy()->port);
    }
    EXPECT_EQ(a.ca_bundle(), b.ca_bundle()) << entry.name;
    EXPECT_EQ(a.device_profile().name, b.device_profile().name) << entry.name;
    EXPECT_EQ(a.launcher().show_on_start, b.launcher().show_on_start)
        << entry.name;
  }
  // Every table entry except the display-server switches, which have their
  // own test, is exercised above.
  for (const ManagedEnvironmentVariable& variable :
       ManagedEnvironmentVariables()) {
    if (variable.yaml_key == "display.server" &&
        variable.name != "MOCKTAIL_DISPLAY_SERVER") {
      continue;
    }
    EXPECT_EQ(covered.count(variable.name), 1U) << variable.name;
  }
}

TEST(ManagedEnvironmentTest, ImportsVideoDriverOverridesAsDisplayServer) {
  for (const char* name : {"SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER"}) {
    EXPECT_EQ(Import(name, "wayland"), "wayland") << name;
    EXPECT_EQ(Import(name, "x11"), "x11") << name;
    // SDL compares names without case and tries the list in order.
    EXPECT_EQ(Import(name, "Wayland"), "wayland") << name;
    EXPECT_EQ(Import(name, "wayland,x11"), "wayland") << name;
    EXPECT_EQ(Import(name, " x11 , wayland"), "x11") << name;
    EXPECT_EQ(Import(name, "kmsdrm"), std::nullopt) << name;
    EXPECT_EQ(Import(name, "offscreen,wayland"), std::nullopt) << name;
    EXPECT_EQ(Import(name, ","), std::nullopt) << name;
  }
  EXPECT_EQ(Import("MOCKTAIL_FORCE_WAYLAND", "1"), "wayland");
  EXPECT_EQ(Import("MOCKTAIL_FORCE_WAYLAND", "yes"), "wayland");
  // "0" switches the Wayland preference off; there is no config equivalent.
  EXPECT_EQ(Import("MOCKTAIL_FORCE_WAYLAND", "0"), std::nullopt);
  for (const char* name : {"MOCKTAIL_FORCE_X11", "MOCKTAIL_ANGLE_FORCE_X11"}) {
    EXPECT_EQ(Import(name, "1"), "x11") << name;
    EXPECT_EQ(Import(name, "0"), std::nullopt) << name;
  }
  EXPECT_EQ(Import("MOCKTAIL_DISPLAY_SERVER", "auto"), "auto");
  EXPECT_EQ(Import("MOCKTAIL_DISPLAY_SERVER", "wayland"), "wayland");
  EXPECT_EQ(Import("MOCKTAIL_DISPLAY_SERVER", "Wayland"), std::nullopt);
}

TEST(ManagedEnvironmentTest, ImportsCanonicalSpellings) {
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan"),
            "direct-vulkan");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "native-vulkan"),
            "direct-vulkan");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "gles"), "opengl");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "system"), "system");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "angle-swiftshader"),
            "angle-swiftshader");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_BACKEND", "metal"), std::nullopt);

  EXPECT_EQ(Import("MOCKTAIL_NATIVE_LOGIN", "0"), "browser");
  EXPECT_EQ(Import("MOCKTAIL_NATIVE_LOGIN", "1"), "native");
  EXPECT_EQ(Import("MOCKTAIL_NATIVE_LOGIN", "false"), "native");

  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_QUALITY", "0"), "manual");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_QUALITY", "manual"), "manual");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_QUALITY", "default"), "default");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_QUALITY", "05"), "5");
  EXPECT_EQ(Import("MOCKTAIL_GRAPHICS_QUALITY", "22"), std::nullopt);

  EXPECT_EQ(Import("MOCKTAIL_GPU", "auto"), "auto");
  EXPECT_EQ(Import("MOCKTAIL_GPU", "discrete"), "discrete");
  EXPECT_EQ(Import("MOCKTAIL_GPU", "integrated"), "integrated");
  EXPECT_EQ(Import("MOCKTAIL_GPU", "Integrated"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_GPU", "igpu"), std::nullopt);

  EXPECT_EQ(Import("MOCKTAIL_FRAME_RATE_LIMIT", "-1"), "-1");
  EXPECT_EQ(Import("MOCKTAIL_FRAME_RATE_LIMIT", "display"), "display");
  EXPECT_EQ(Import("MOCKTAIL_FRAME_RATE_LIMIT", "unlimited"), "unlimited");
  EXPECT_EQ(Import("MOCKTAIL_FRAME_RATE_LIMIT", "0"), std::nullopt);

  EXPECT_EQ(Import("MOCKTAIL_GAMEMODE", "true"), "on");
  EXPECT_EQ(Import("MOCKTAIL_GAMEMODE", "0"), "off");
  EXPECT_EQ(Import("MOCKTAIL_DEVICE_PROFILE", "console"), "console-ps5");
  EXPECT_EQ(Import("MOCKTAIL_DEVICE_PROFILE", "tablet"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_WIN_HIGH_DPI", "1"), "true");
  EXPECT_EQ(Import("MOCKTAIL_WIN_HIGH_DPI", "off"), "false");
  EXPECT_EQ(Import("MOCKTAIL_WIN_HIGH_DPI", "yes"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_WIN_WIDTH", "1280px"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_WIN_WIDTH", "0"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_WIN_WIDTH", "99999999999"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_WIN_TITLE", "Line\nbreak"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_MEMORY_LIMIT_MB", "-1"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_HTTP_PROXY_HOST", "http://proxy"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_HTTP_PROXY_PORT", "65536"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_CA_BUNDLE", "relative.pem"), std::nullopt);
  EXPECT_EQ(Import("MOCKTAIL_DISCORD_APPLICATION_ID", "12345"), std::nullopt);
  // The runtime reads the system-proxy switch as on for anything but "0".
  EXPECT_EQ(Import("MOCKTAIL_USE_SYSTEM_PROXY", "false"), "true");
  EXPECT_EQ(Import("MOCKTAIL_USE_SYSTEM_PROXY", "0"), "false");
  EXPECT_EQ(Import("MOCKTAIL_UNKNOWN", "1"), std::nullopt);
}

TEST(ManagedEnvironmentTest, CapturesPresentManagedNamesInTableOrder) {
  const std::vector<const char*> environment = {
      "HOME=/home/user",
      "MOCKTAIL_VSYNC=off",
      "SDL_VIDEODRIVER=wayland",
      "MOCKTAIL_NATIVE_LOGIN=",  // present but empty still hides YAML
      "MOCKTAIL_HEADLESS=0",     // not a launcher setting
      "MOCKTAIL_VSYNC=on",       // duplicate entry
      "MOCKTAIL_GRAPHICS_BACKEND",  // malformed: no '='
      "=MOCKTAIL_THEME",            // malformed: empty name
      "MOCKTAIL_VSYNCX=1",          // prefix of nothing managed
      "MOCKTAIL_DEVICE_PROFILE=pc",
      nullptr,
  };

  const std::vector<std::string> captured =
      CaptureUserManagedEnvironment(environment.data());

  std::vector<std::string> expected;
  for (const ManagedEnvironmentVariable& variable :
       ManagedEnvironmentVariables()) {
    if (variable.name == "MOCKTAIL_VSYNC" ||
        variable.name == "SDL_VIDEODRIVER" ||
        variable.name == "MOCKTAIL_NATIVE_LOGIN" ||
        variable.name == "MOCKTAIL_DEVICE_PROFILE") {
      expected.emplace_back(variable.name);
    }
  }
  EXPECT_EQ(captured, expected);
  EXPECT_TRUE(CaptureUserManagedEnvironment(nullptr).empty());
  const char* empty[] = {nullptr};
  EXPECT_TRUE(CaptureUserManagedEnvironment(empty).empty());

  // main() captures from the environment ProcessStartState took.
  std::vector<std::string> start_environment;
  for (const char* entry : environment) {
    if (entry != nullptr) {
      start_environment.emplace_back(entry);
    }
  }
  EXPECT_EQ(CaptureUserManagedEnvironment(start_environment), expected);
  EXPECT_TRUE(
      CaptureUserManagedEnvironment(std::vector<std::string>{}).empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
