#ifndef MOCKTAIL_LAUNCHER_UI_PAGE_RULES_H_
#define MOCKTAIL_LAUNCHER_UI_PAGE_RULES_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "launcher/fast_flags_document.h"
#include "runtime/environment.h"

// GTK-free rules behind the Integrations, Network & Updates, Advanced and
// About pages: what the runtime accepts (so a page can refuse a value that
// would stop Mocktail at start), how the updater answers, which Fast Flags
// collide with Mocktail's own settings, and what goes into the diagnostic
// text. The pages turn the results into translated words; nothing here
// touches GTK or the network.
namespace mocktail::launcher_ui {

// ---- Discord Rich Presence --------------------------------------------------

// The loader's limits (runtime_config_file.cc ValidateAndMap), in bytes.
inline constexpr std::size_t kDiscordButtonLabelLimit = 32;
inline constexpr std::size_t kDiscordTextLimit = 128;

enum class DiscordTextProblem { kNone, kEmpty, kTooLong, kControlCharacter };

// Why the loader would refuse `text` for a Discord text of at most `limit`
// bytes: it must be non-empty, bounded and free of control bytes.
DiscordTextProblem CheckDiscordText(std::string_view text, std::size_t limit);

// 17 to 20 ASCII digits (a Discord snowflake), as the loader requires for
// integrations.discord_rpc.application_id.
bool IsDiscordApplicationId(std::string_view text);

// The "playing" text as Discord shows it: every {place_name} replaced, cut to
// 128 bytes on a UTF-8 boundary (discord_rpc.cc RenderPlaceTemplate).
std::string RenderDiscordPlaceText(std::string_view text,
                                   std::string_view place_name);

// ---- proxy ------------------------------------------------------------------

enum class ProxyMode { kNone, kSystem, kManual };

// What network.use_system_proxy and the presence of network.proxy_host /
// proxy_port select. Either fixed value present means a manual proxy (the
// loader wants both, runtime_config_file.cc).
ProxyMode ProxyModeFor(std::string_view use_system_proxy, bool has_host,
                       bool has_port);

enum class ProxyHostProblem { kNone, kEmpty, kScheme, kInvalidCharacter };

// ParseNetworkProxyConfig's host rules (runtime_config.cc): no "://", no
// whitespace or control characters, none of / \ @ [ ] ? #.
ProxyHostProblem CheckProxyHost(std::string_view host);

// A port from 1 to 65535 in plain decimal.
bool IsValidPort(std::string_view port);

// ---- Fleasion ---------------------------------------------------------------

struct FleasionInputs {
  bool enabled = false;
  std::string proxy_mode = "env";
  std::string proxy_port = "58443";
  bool use_system_proxy = false;
  // network.proxy_host / proxy_port, when set.
  std::optional<std::string> network_proxy_host;
  std::optional<std::string> network_proxy_port;
};

enum class FleasionConflict {
  kNone,
  // Fleasion together with network.use_system_proxy.
  kSystemProxy,
  // env mode with a fixed proxy other than 127.0.0.1:<Fleasion's port>.
  kDifferentProxy,
  // hosts mode with any fixed proxy.
  kProxyInHostsMode,
};

// The combinations RuntimeConfig::FromEnvironment refuses while Fleasion is
// enabled (runtime_config.cc, "fleasion_valid_"); the loader then stops
// Mocktail with "Fleasion configuration is invalid".
FleasionConflict FindFleasionConflict(const FleasionInputs& inputs);

// $XDG_CONFIG_HOME/Fleasion when that is absolute, else $HOME/.config/Fleasion.
std::filesystem::path FleasionConfigDirectory(
    const runtime::Environment& environment);
// <that>/proxy_ca/ca.crt: the certificate Mocktail reads when
// integrations.fleasion.ca_certificate is unset (fleasion.cc).
std::filesystem::path DefaultFleasionCertificate(
    const runtime::Environment& environment);

enum class CertificatePathProblem { kNone, kRelative, kPrivateKey };

// A CA file path for network.ca_bundle or Fleasion's certificate: absolute
// (the loader's rule) and not a private key (*.key; the template warns
// "Never select ca.key"). Empty is fine: the key is then left out.
CertificatePathProblem CheckCertificatePath(std::string_view text);

// ---- the updater ------------------------------------------------------------

// mocktail_updater as mocktail itself finds it (payload_update_preflight.cc
// UpdateHelper): MOCKTAIL_UPDATE_HELPER when it is an absolute executable,
// else next to `executable`, in ../libexec/mocktail or ../lib/mocktail.
// Empty when none is executable.
std::filesystem::path ResolveUpdaterHelper(
    const runtime::Environment& environment,
    const std::filesystem::path& executable,
    const std::function<bool(const std::filesystem::path&)>& is_executable);

struct InstalledRoblox {
  bool installed = false;
  std::string version_name;       // "2.736.1408"
  std::string version_code;       // "2998"
  std::string build_id;           // the ELF build ID (hex)
  std::int64_t activated_at = 0;  // seconds since the epoch, 0 when unknown
};

// An active payload manifest (data_root/current.json, payload_store.cc
// ActivationManifest). False when it is not one.
bool ParseActivePayloadManifest(std::string_view json, InstalledRoblox* out);

// `mocktail_updater status`: {"current": <manifest or null>, ...}. A null
// current is a valid answer (nothing installed yet).
bool ParseUpdaterStatus(std::string_view json, InstalledRoblox* out);

struct LatestRoblox {
  std::string version_name;
  std::string version_code;
};

// `mocktail_updater check-latest` prints "<version name> <version code>".
bool ParseCheckLatest(std::string_view output, LatestRoblox* out);

enum class UpdateComparison {
  kNotInstalled,
  kUpToDate,
  kNewerAvailable,
  kInstalledNewer,
};

// By version code, as the updater compares them (update_coordinator.cc).
UpdateComparison CompareWithLatest(const InstalledRoblox& installed,
                                   const LatestRoblox& latest);

// The last non-empty line of the updater's stderr without its
// "[native-updater] " prefix; empty when there is none.
std::string UpdaterErrorMessage(std::string_view stderr_text);

// ---- device -----------------------------------------------------------------

// pc-windows-11, mobile-pixel-7 or console-ps5 for a preset name or one of
// its aliases (device_profile.cc), empty for anything else.
std::string CanonicalDevicePreset(std::string_view value);

// ---- Fast Flags -------------------------------------------------------------

// The settings Mocktail turns into client settings of its own, as the game
// will read them (config.yaml values, or the overriding variables).
struct FastFlagSettings {
  std::string frame_rate_limit = "-1";
  std::string multithreaded_rendering = "false";
  std::string physics_worker_mode = "throughput";
  std::string memory_limit_mb = "0";
  std::string gamemode = "auto";
  // engine.graphics_quality: default, manual or 1..21.
  std::string graphics_quality = "default";
  // Direct Vulkan on Intel-only graphics: the default quality is level 1
  // there (graphics_launch_policy.cc).
  bool intel_only_direct_vulkan = false;
};

// The level the preset forces through FIntDebugFRMQualityLevelOverride,
// nullopt when it forces none (preset off, or graphics_quality manual).
// Whether the preset applies is RenderingPresetActive (recommendations.h),
// which the Graphics page uses too.
std::optional<std::string> ManagedQualityLevel(
    const FastFlagSettings& settings);

// FastFlagsDocument::FindManagedConflicts for these settings. The quality
// level is taken from `settings` rather than from this process's
// MOCKTAIL_GRAPHICS_QUALITY, which the settings window never has.
std::vector<launcher::FastFlagConflict> FindFastFlagConflicts(
    const launcher::FastFlagsDocument& document,
    const FastFlagSettings& settings);

// The value type a flag's name implies: FFlag/DFFlag booleans, FInt/DFInt/
// FLog/DFLog integers, FString/DFString text; other names follow the value
// (true/false, a whole number, else text).
launcher::FastFlagValueKind InferFastFlagKind(std::string_view name,
                                              std::string_view value);

// Entries added, removed or changed between two lists.
int CountFastFlagChanges(const std::vector<launcher::FastFlagEntry>& saved,
                         const std::vector<launcher::FastFlagEntry>& current);

// ---- diagnostics ------------------------------------------------------------

// `key=value` or `key="value"` from a SessionLog header; empty when absent.
std::string SessionHeaderField(std::string_view header, std::string_view key);

// The header without what only a running session has (its log path, pid).
std::string TrimSessionHeader(std::string_view header);

// "Name: value" lines.
std::string FormatDiagnosticLines(
    const std::vector<std::pair<std::string, std::string>>& lines);

// config.yaml keys whose values may go into a bug report: no host names,
// paths, free texts or identifiers.
const std::vector<std::string_view>& DiagnosticSettingKeys();

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_PAGE_RULES_H_
