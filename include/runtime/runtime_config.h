#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/device_profile.h"
#include "runtime/environment.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail {
namespace runtime {

enum class GraphicsBackend {
  kAuto,
  kSystem,
  kVulkan,
  kAngleVulkan,
  kAngleSwiftShader,
  kUnknown,
};

struct WindowConfig {
  int width = 1280;
  int height = 720;
  std::string title = "Roblox";
  // GNOME desktop dimensions are logical units; keep the render surface at
  // that size unless the user explicitly requests high-density rendering.
  bool high_dpi = false;
  bool high_dpi_valid = true;
};

struct InputCapabilityConfig {
  bool touch_enabled = false;
  bool mouse_enabled = true;
  bool keyboard_enabled = true;
};

struct NetworkProxyConfig {
  std::string scheme = "http";
  std::string host;
  int port = 0;
};

struct DiscordRpcTextConfig {
  std::string browsing = "Browsing experiences";
  std::string joining = "Joining an experience";
  std::string playing = "{place_name}";
  std::string state = "Playing Roblox";
  std::string unknown_place = "Unknown experience";
};

struct DiscordRpcConfig {
  bool enabled = false;
  bool show_place_name = true;
  bool show_elapsed_time = true;
  bool join_enabled = true;
  bool public_servers_only = true;
  std::string join_button_label = "Join Server";
  std::string application_id;
  DiscordRpcTextConfig text;
};

// display.server. Auto keeps the window policy's own choice (XWayland for
// NVIDIA with direct Vulkan unless the driver and the compositor support
// explicit sync); the others force that SDL video driver.
enum class DisplayServer {
  kAuto,
  kWayland,
  kX11,
};

// display.start_mode. Remember restores the presentation the game recorded
// in window-state.json; the others replace it for this start only.
enum class WindowStartMode {
  kRemember,
  kWindowed,
  kMaximized,
  kFullscreen,
};

// account.sign_in. Browser is the WebKit sign-in window that
// MOCKTAIL_NATIVE_LOGIN=0 selects.
enum class SignInMethod {
  kNative,
  kBrowser,
};

enum class GraphicsQualityMode {
  // Mocktail's rendering preset level when that preset is active.
  kDefault,
  // No FRM override: Roblox's in-game graphics slider stays in control.
  kManual,
  // Force FIntDebugFRMQualityLevelOverride to `level`.
  kLevel,
};

inline constexpr int kMinimumGraphicsQualityLevel = 1;
inline constexpr int kMaximumGraphicsQualityLevel = 21;

struct GraphicsQuality {
  GraphicsQualityMode mode = GraphicsQualityMode::kDefault;
  int level = 0;

  bool operator==(const GraphicsQuality& other) const {
    return mode == other.mode && level == other.level;
  }
  bool operator!=(const GraphicsQuality& other) const {
    return !(*this == other);
  }
};

// engine.gpu: the graphics card direct Vulkan renders on when the computer
// has more than one. Auto prefers the discrete card unless DRI_PRIME or
// __NV_PRIME_RENDER_OFFLOAD asks for the integrated one; a computer with a
// single card always uses it.
enum class GpuPreference {
  kAuto,
  kDiscrete,
  kIntegrated,
};

struct DisplayConfig {
  DisplayServer server = DisplayServer::kAuto;
  bool server_valid = true;
  WindowStartMode start_mode = WindowStartMode::kRemember;
  bool start_mode_valid = true;
};

struct AccountConfig {
  SignInMethod sign_in = SignInMethod::kNative;
};

struct EngineConfig {
  GraphicsQuality graphics_quality;
  bool graphics_quality_valid = true;
  GpuPreference gpu = GpuPreference::kAuto;
  bool gpu_valid = true;
  // engine.nvidia_shader_mt / MOCKTAIL_NVIDIA_SHADER_MT. False makes the
  // direct Vulkan launch policy deny Roblox's multithreaded shader pack
  // loading on NVIDIA GPUs.
  bool nvidia_shader_mt = true;
  bool nvidia_shader_mt_valid = true;
};

struct LauncherConfig {
  bool show_on_start = true;
  bool show_on_start_valid = true;
};

// Parsers accept exactly the lowercase config.yaml spelling, and names return
// it. MOCKTAIL_DISPLAY_SERVER, MOCKTAIL_WINDOW_START_MODE and MOCKTAIL_GPU
// carry the same spelling; MOCKTAIL_NATIVE_LOGIN keeps its 1/0 form.
std::optional<DisplayServer> ParseDisplayServer(std::string_view value);
std::string_view DisplayServerName(DisplayServer server);
std::optional<WindowStartMode> ParseWindowStartMode(std::string_view value);
std::string_view WindowStartModeName(WindowStartMode mode);
std::optional<SignInMethod> ParseSignInMethod(std::string_view value);
std::string_view SignInMethodName(SignInMethod method);
// config.yaml form: default, manual, or a level from 1 to 21.
std::optional<GraphicsQuality> ParseGraphicsQuality(std::string_view value);
// MOCKTAIL_GRAPHICS_QUALITY form: the config.yaml form plus the spellings the
// rendering preset has always treated as manual (auto and 0).
std::optional<GraphicsQuality> ParseGraphicsQualityVariable(
    std::string_view value);
std::string GraphicsQualityName(const GraphicsQuality& quality);
std::optional<GpuPreference> ParseGpuPreference(std::string_view value);
std::string_view GpuPreferenceName(GpuPreference preference);
// Switch-style variables: 1, true or on; 0, false or off.
std::optional<bool> ParseEnvironmentSwitch(std::string_view value);

// Replaces the restored presentation flags as display.start_mode asks.
// Remember keeps both; fullscreen leaves the maximized flag underneath so
// leaving fullscreen returns to the recorded window state.
void ApplyWindowStartMode(WindowStartMode mode, bool* fullscreen,
                          bool* maximized);

std::optional<NetworkProxyConfig> ParseNetworkProxyConfig(
    std::string_view host, std::string_view port,
    std::string_view scheme = "http");
std::string BuildNetworkProxyUrl(const NetworkProxyConfig& proxy);

// Immutable, supported runtime options. Advanced MOCKTAIL_PATCH_* controls
// intentionally do not belong here; they remain isolated in the legacy path.
class RuntimeConfig {
 public:
  static RuntimeConfig FromEnvironment(const Environment& environment);

  bool headless() const { return headless_; }
  const std::filesystem::path& roblox_library_path() const {
    return roblox_library_path_;
  }
  GraphicsBackend graphics_backend() const { return graphics_backend_; }
  const std::string& graphics_backend_name() const {
    return graphics_backend_name_;
  }
  const WindowConfig& window() const { return window_; }
  const std::string& theme_mode() const { return theme_mode_; }
  bool theme_mode_valid() const {
    return theme_mode_ == "roblox" || theme_mode_ == "system" ||
           theme_mode_ == "light" || theme_mode_ == "dark";
  }
  const InputCapabilityConfig& input_capabilities() const {
    return input_capabilities_;
  }
  const DeviceProfile& device_profile() const { return device_profile_; }
  bool device_profile_valid() const { return device_profile_valid_; }
  bool desktop_playability() const {
    return device_profile_.device_class == DeviceClass::kPc;
  }
  const std::optional<std::string>& roblox_http_user_agent() const {
    return roblox_http_user_agent_;
  }
  const FrameRatePolicy& frame_rate() const { return frame_rate_; }
  const std::string& vsync_mode() const { return vsync_mode_; }
  const PerformancePolicy& performance() const { return performance_; }
  const std::string& audio_output_device() const {
    return audio_output_device_;
  }
  bool audio_output_device_valid() const { return audio_output_device_valid_; }
  const std::string& audio_input_device() const { return audio_input_device_; }
  bool audio_input_device_valid() const { return audio_input_device_valid_; }
  bool microphone_enabled() const { return audio_input_device_ != "disabled"; }
  const std::optional<NetworkProxyConfig>& network_proxy() const {
    return network_proxy_;
  }
  const std::optional<std::filesystem::path>& ca_bundle() const {
    return ca_bundle_;
  }
  bool ca_bundle_valid() const { return ca_bundle_valid_; }
  bool use_system_proxy() const { return use_system_proxy_; }
  bool fleasion_enabled() const { return fleasion_enabled_; }
  bool fleasion_valid() const { return fleasion_valid_; }
  const std::string& fleasion_proxy_mode() const { return fleasion_proxy_mode_; }
  int fleasion_proxy_port() const { return fleasion_proxy_port_; }
  const std::optional<std::filesystem::path>& fleasion_ca_certificate() const {
    return fleasion_ca_certificate_;
  }
  const DiscordRpcConfig& discord_rpc() const { return discord_rpc_; }
  bool discord_rpc_valid() const { return discord_rpc_valid_; }
  const DisplayConfig& display() const { return display_; }
  const AccountConfig& account() const { return account_; }
  const EngineConfig& engine() const { return engine_; }
  const LauncherConfig& launcher() const { return launcher_; }

  // These legacy opt-ins create workers that do not own their VM/JNI state.
  // Empty and "0" values are disabled; every other non-empty value is unsafe.
  bool has_unsafe_detached_thread_overrides() const {
    return !unsafe_detached_thread_overrides_.empty();
  }
  const std::vector<std::string>& unsafe_detached_thread_overrides() const {
    return unsafe_detached_thread_overrides_;
  }

  static GraphicsBackend ParseGraphicsBackend(std::string_view name);

 private:
  bool headless_ = false;
  std::filesystem::path roblox_library_path_ = "rbx_bin/libroblox.so";
  GraphicsBackend graphics_backend_ = GraphicsBackend::kVulkan;
  std::string graphics_backend_name_ = "direct-vulkan";
  WindowConfig window_;
  std::string theme_mode_ = "roblox";
  InputCapabilityConfig input_capabilities_;
  DeviceProfile device_profile_ = *FindDeviceProfile(kDefaultDeviceProfileName);
  bool device_profile_valid_ = true;
  std::optional<std::string> roblox_http_user_agent_;
  FrameRatePolicy frame_rate_;
  std::string vsync_mode_ = "auto";
  PerformancePolicy performance_;
  std::string audio_output_device_ = "default";
  bool audio_output_device_valid_ = true;
  std::string audio_input_device_ = "default";
  bool audio_input_device_valid_ = true;
  bool use_system_proxy_ = false;
  bool fleasion_enabled_ = false;
  bool fleasion_valid_ = true;
  std::string fleasion_proxy_mode_ = "env";
  int fleasion_proxy_port_ = 58443;
  std::optional<std::filesystem::path> fleasion_ca_certificate_;
  std::optional<NetworkProxyConfig> network_proxy_;
  std::optional<std::filesystem::path> ca_bundle_;
  bool ca_bundle_valid_ = true;
  DiscordRpcConfig discord_rpc_;
  bool discord_rpc_valid_ = true;
  DisplayConfig display_;
  AccountConfig account_;
  EngineConfig engine_;
  LauncherConfig launcher_;
  std::vector<std::string> unsafe_detached_thread_overrides_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_
