#include "runtime/managed_environment.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/device_profile.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/game_mode.h"
#include "runtime/performance_policy.h"
#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {
namespace {

using Imported = std::optional<std::string>;

bool HasControlBytes(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
  });
}

std::optional<long long> ParseDecimal(std::string_view value) {
  long long parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || result.ec != std::errc() ||
      result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](char a, char b) {
                      const auto lower = [](char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32)
                                                    : c;
                      };
                      return lower(a) == lower(b);
                    });
}

// window.cc treats these switches as on when non-empty and not "0".
bool LegacySwitchEnabled(std::string_view value) {
  return !value.empty() && value != "0";
}

Imported Switch(std::string_view value) {
  const std::optional<bool> parsed = ParseEnvironmentSwitch(value);
  if (!parsed.has_value()) return std::nullopt;
  return std::string(*parsed ? "true" : "false");
}

Imported Text(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum || HasControlBytes(value)) {
    return std::nullopt;
  }
  return std::string(value);
}

Imported AbsolutePath(std::string_view value) {
  if (value.empty() || HasControlBytes(value) ||
      !std::filesystem::path(std::string(value)).is_absolute()) {
    return std::nullopt;
  }
  return std::string(value);
}

Imported Port(std::string_view value) {
  const std::optional<long long> port = ParseDecimal(value);
  if (!port.has_value() || *port < 1 || *port > 65535) return std::nullopt;
  return std::to_string(*port);
}

Imported DisplayServerVariable(std::string_view value) {
  const std::optional<DisplayServer> server = ParseDisplayServer(value);
  if (!server.has_value()) return std::nullopt;
  return std::string(DisplayServerName(*server));
}

// SDL reads a comma-separated priority list and compares names without
// regard to case; the first entry wins whenever that driver is available.
Imported SdlVideoDriver(std::string_view value) {
  std::string_view first = value.substr(0, value.find(','));
  while (!first.empty() && first.front() == ' ') first.remove_prefix(1);
  while (!first.empty() && first.back() == ' ') first.remove_suffix(1);
  if (EqualsIgnoringAsciiCase(first, "wayland")) return std::string("wayland");
  if (EqualsIgnoringAsciiCase(first, "x11")) return std::string("x11");
  return std::nullopt;
}

Imported ForceWayland(std::string_view value) {
  if (!LegacySwitchEnabled(value)) return std::nullopt;
  return std::string("wayland");
}

Imported ForceX11(std::string_view value) {
  if (!LegacySwitchEnabled(value)) return std::nullopt;
  return std::string("x11");
}

Imported WindowStartModeVariable(std::string_view value) {
  const std::optional<WindowStartMode> mode = ParseWindowStartMode(value);
  if (!mode.has_value()) return std::nullopt;
  return std::string(WindowStartModeName(*mode));
}

// The config key takes canonical names only: window.cc opens the direct
// Vulkan window for the exact string direct-vulkan, and gles is the same
// strict EGL path as opengl.
Imported GraphicsBackendVariable(std::string_view value) {
  if (value == "auto" || value == "system" || value == "opengl" ||
      value == "direct-vulkan" || value == "angle-vulkan" ||
      value == "angle-swiftshader") {
    return std::string(value);
  }
  if (value == "gles") return std::string("opengl");
  if (value == "vulkan" || value == "native-vulkan") {
    return std::string("direct-vulkan");
  }
  return std::nullopt;
}

Imported GraphicsQualityVariable(std::string_view value) {
  const std::optional<GraphicsQuality> quality =
      ParseGraphicsQualityVariable(value);
  if (!quality.has_value()) return std::nullopt;
  return GraphicsQualityName(*quality);
}

Imported FrameRateLimit(std::string_view value) {
  if (value.empty()) return std::nullopt;
  const FrameRatePolicy policy = ParseFrameRatePolicy(value);
  switch (policy.mode) {
    case FrameRateLimitMode::kUnmanaged:
      return std::string("-1");
    case FrameRateLimitMode::kDisplay:
      return std::string("display");
    case FrameRateLimitMode::kUnlimited:
      return std::string("unlimited");
    case FrameRateLimitMode::kFixed:
      return std::to_string(policy.fixed_fps);
    case FrameRateLimitMode::kInvalid:
      break;
  }
  return std::nullopt;
}

Imported Vsync(std::string_view value) {
  if (value == "auto" || value == "on" || value == "off") {
    return std::string(value);
  }
  return std::nullopt;
}

Imported WindowDimension(std::string_view value) {
  const std::optional<long long> size = ParseDecimal(value);
  if (!size.has_value() || *size <= 0 ||
      *size > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return std::to_string(*size);
}

Imported WindowTitle(std::string_view value) { return Text(value, 512); }

Imported Theme(std::string_view value) {
  if (value == "roblox" || value == "system" || value == "light" ||
      value == "dark") {
    return std::string(value);
  }
  return std::nullopt;
}

// An empty variable means the throughput default, not the parser's auto.
Imported PhysicsWorkers(std::string_view value) {
  PhysicsWorkerMode mode = PhysicsWorkerMode::kAuto;
  if (value.empty() || !ParsePhysicsWorkerMode(value, &mode)) {
    return std::nullopt;
  }
  return std::string(PhysicsWorkerModeName(mode));
}

Imported GameMode(std::string_view value) {
  GameModePolicy policy = GameModePolicy::kAuto;
  if (!ParseGameModePolicy(value, &policy)) return std::nullopt;
  return std::string(GameModePolicyName(policy));
}

Imported MemoryLimit(std::string_view value) {
  std::uint64_t megabytes = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), megabytes);
  if (value.empty() || result.ec != std::errc() ||
      result.ptr != value.data() + value.size() ||
      megabytes > std::numeric_limits<std::uint64_t>::max() / (1024U * 1024U)) {
    return std::nullopt;
  }
  return std::to_string(megabytes);
}

Imported AudioDevice(std::string_view value) {
  if (!IsValidDeviceProfileValue(value, 512)) return std::nullopt;
  return std::string(value);
}

// Only "0" ever selected the browser window; every other value is native.
Imported NativeLogin(std::string_view value) {
  if (value.empty()) return std::nullopt;
  return std::string(value == "0" ? "browser" : "native");
}

Imported DeviceProfileVariable(std::string_view value) {
  const DeviceProfile* profile = FindDeviceProfile(value);
  if (value.empty() || profile == nullptr) return std::nullopt;
  return std::string(profile->name);
}

// The runtime reads this one as a legacy switch: anything but "0" is on.
Imported UseSystemProxy(std::string_view value) {
  if (value.empty()) return std::nullopt;
  return std::string(value == "0" ? "false" : "true");
}

Imported ProxyHost(std::string_view value) {
  if (!ParseNetworkProxyConfig(value, "1").has_value()) return std::nullopt;
  return std::string(value);
}

Imported DiscordLabel(std::string_view value) { return Text(value, 32); }

Imported DiscordText(std::string_view value) { return Text(value, 128); }

Imported DiscordApplicationId(std::string_view value) {
  if (value.size() < 17 || value.size() > 20 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= '0' && byte <= '9';
      })) {
    return std::nullopt;
  }
  return std::string(value);
}

Imported FleasionProxyMode(std::string_view value) {
  if (value == "env" || value == "hosts") return std::string(value);
  return std::nullopt;
}

std::vector<ManagedEnvironmentVariable> BuildManagedEnvironmentVariables() {
  return {
      // Graphics
      {"MOCKTAIL_GRAPHICS_BACKEND", "graphics.backend",
       GraphicsBackendVariable},
      {"MOCKTAIL_GRAPHICS_QUALITY", "engine.graphics_quality",
       GraphicsQualityVariable},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "graphics.frame_rate_limit",
       FrameRateLimit},
      {"MOCKTAIL_VSYNC", "graphics.vsync", Vsync},
      // Display
      {"MOCKTAIL_WINDOW_START_MODE", "display.start_mode",
       WindowStartModeVariable},
      {"MOCKTAIL_WIN_WIDTH", "window.width", WindowDimension},
      {"MOCKTAIL_WIN_HEIGHT", "window.height", WindowDimension},
      {"MOCKTAIL_WIN_TITLE", "window.title", WindowTitle},
      {"MOCKTAIL_WIN_HIGH_DPI", "window.high_dpi", Switch},
      {"MOCKTAIL_DISPLAY_SERVER", "display.server", DisplayServerVariable},
      {"SDL_VIDEODRIVER", "display.server", SdlVideoDriver},
      {"SDL_VIDEO_DRIVER", "display.server", SdlVideoDriver},
      {"MOCKTAIL_FORCE_WAYLAND", "display.server", ForceWayland},
      {"MOCKTAIL_FORCE_X11", "display.server", ForceX11},
      {"MOCKTAIL_ANGLE_FORCE_X11", "display.server", ForceX11},
      {"MOCKTAIL_THEME", "appearance.theme", Theme},
      // Performance
      {"MOCKTAIL_MULTITHREADED_RENDERING",
       "performance.multithreaded_rendering", Switch},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "performance.physics_worker_mode",
       PhysicsWorkers},
      {"MOCKTAIL_GAMEMODE", "performance.gamemode", GameMode},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "performance.memory_limit_mb",
       MemoryLimit},
      // Audio
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "audio.output_device", AudioDevice},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "audio.input_device", AudioDevice},
      // Accounts
      {"MOCKTAIL_NATIVE_LOGIN", "account.sign_in", NativeLogin},
      // Integrations
      {"MOCKTAIL_DISCORD_RPC_ENABLED", "integrations.discord_rpc.enabled",
       Switch},
      {"MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME",
       "integrations.discord_rpc.show_place_name", Switch},
      {"MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME",
       "integrations.discord_rpc.show_elapsed_time", Switch},
      {"MOCKTAIL_DISCORD_RPC_JOIN_ENABLED",
       "integrations.discord_rpc.join.enabled", Switch},
      {"MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY",
       "integrations.discord_rpc.join.public_servers_only", Switch},
      {"MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
       "integrations.discord_rpc.join.button_label", DiscordLabel},
      {"MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
       "integrations.discord_rpc.text.browsing", DiscordText},
      {"MOCKTAIL_DISCORD_RPC_TEXT_JOINING",
       "integrations.discord_rpc.text.joining", DiscordText},
      {"MOCKTAIL_DISCORD_RPC_TEXT_PLAYING",
       "integrations.discord_rpc.text.playing", DiscordText},
      {"MOCKTAIL_DISCORD_RPC_TEXT_STATE", "integrations.discord_rpc.text.state",
       DiscordText},
      {"MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
       "integrations.discord_rpc.text.unknown_place", DiscordText},
      {"MOCKTAIL_DISCORD_APPLICATION_ID",
       "integrations.discord_rpc.application_id", DiscordApplicationId},
      {"MOCKTAIL_FLEASION_ENABLED", "integrations.fleasion.enabled", Switch},
      {"MOCKTAIL_FLEASION_PROXY_MODE", "integrations.fleasion.proxy_mode",
       FleasionProxyMode},
      {"MOCKTAIL_FLEASION_PROXY_PORT", "integrations.fleasion.proxy_port",
       Port},
      {"MOCKTAIL_FLEASION_CA_CERTIFICATE",
       "integrations.fleasion.ca_certificate", AbsolutePath},
      // Network
      {"MOCKTAIL_USE_SYSTEM_PROXY", "network.use_system_proxy",
       UseSystemProxy},
      {"MOCKTAIL_HTTP_PROXY_HOST", "network.proxy_host", ProxyHost},
      {"MOCKTAIL_HTTP_PROXY_PORT", "network.proxy_port", Port},
      {"MOCKTAIL_CA_BUNDLE", "network.ca_bundle", AbsolutePath},
      // Advanced
      {"MOCKTAIL_DEVICE_PROFILE", "device", DeviceProfileVariable},
      {"MOCKTAIL_LAUNCHER_SHOW_ON_START", "launcher.show_on_start", Switch},
  };
}

}  // namespace

const std::vector<ManagedEnvironmentVariable>& ManagedEnvironmentVariables() {
  static const std::vector<ManagedEnvironmentVariable> variables =
      BuildManagedEnvironmentVariables();
  return variables;
}

const ManagedEnvironmentVariable* FindManagedEnvironmentVariable(
    std::string_view name) {
  for (const ManagedEnvironmentVariable& variable :
       ManagedEnvironmentVariables()) {
    if (variable.name == name) {
      return &variable;
    }
  }
  return nullptr;
}

std::optional<std::string> ImportManagedEnvironmentValue(
    std::string_view name, std::string_view value) {
  const ManagedEnvironmentVariable* variable =
      FindManagedEnvironmentVariable(name);
  if (variable == nullptr || variable->importer == nullptr) {
    return std::nullopt;
  }
  return variable->importer(value);
}

namespace {

// Marks the managed variable a "NAME=value" entry sets, if any.
void NoteManagedAssignment(
    std::string_view assignment,
    const std::vector<ManagedEnvironmentVariable>& variables,
    std::vector<bool>* present) {
  const std::size_t equals = assignment.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    return;
  }
  const std::string_view name = assignment.substr(0, equals);
  for (std::size_t index = 0; index < variables.size(); ++index) {
    if (variables[index].name == name) {
      (*present)[index] = true;
      return;
    }
  }
}

std::vector<std::string> PresentManagedNames(
    const std::vector<ManagedEnvironmentVariable>& variables,
    const std::vector<bool>& present) {
  std::vector<std::string> names;
  for (std::size_t index = 0; index < variables.size(); ++index) {
    if (present[index]) {
      names.emplace_back(variables[index].name);
    }
  }
  return names;
}

}  // namespace

std::vector<std::string> CaptureUserManagedEnvironment(
    const char* const* environment) {
  const std::vector<ManagedEnvironmentVariable>& variables =
      ManagedEnvironmentVariables();
  std::vector<bool> present(variables.size(), false);
  for (const char* const* entry = environment;
       entry != nullptr && *entry != nullptr; ++entry) {
    NoteManagedAssignment(*entry, variables, &present);
  }
  return PresentManagedNames(variables, present);
}

std::vector<std::string> CaptureUserManagedEnvironment(
    const std::vector<std::string>& environment) {
  const std::vector<ManagedEnvironmentVariable>& variables =
      ManagedEnvironmentVariables();
  std::vector<bool> present(variables.size(), false);
  for (const std::string& entry : environment) {
    NoteManagedAssignment(entry, variables, &present);
  }
  return PresentManagedNames(variables, present);
}

}  // namespace runtime
}  // namespace mocktail
