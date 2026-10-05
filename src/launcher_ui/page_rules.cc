#include "launcher_ui/page_rules.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "launcher_ui/recommendations.h"
#include "runtime/device_profile.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail::launcher_ui {
namespace {

constexpr std::string_view kQualityFlag = "FIntDebugFRMQualityLevelOverride";

bool IsDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

// Cuts at most `maximum` bytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(std::string text, std::size_t maximum) {
  if (text.size() <= maximum) return text;
  std::size_t boundary = maximum;
  while (boundary != 0 &&
         (static_cast<unsigned char>(text[boundary]) & 0xc0U) == 0x80U) {
    --boundary;
  }
  text.resize(boundary);
  return text;
}

std::string JsonScalarText(const nlohmann::json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<std::uint64_t>());
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<std::int64_t>());
  }
  return {};
}

bool ReadManifest(const nlohmann::json& document, InstalledRoblox* out) {
  if (!document.is_object() || !document.contains("schema_version") ||
      !document["schema_version"].is_number_integer() ||
      document["schema_version"].get<std::int64_t>() != 1 ||
      !document.contains("payload_id") || !document["payload_id"].is_string()) {
    return false;
  }
  InstalledRoblox result;
  result.installed = true;
  if (document.contains("version_name")) {
    result.version_name = JsonScalarText(document["version_name"]);
  }
  if (document.contains("version_code")) {
    result.version_code = JsonScalarText(document["version_code"]);
  }
  if (document.contains("elf_build_id")) {
    result.build_id = JsonScalarText(document["elf_build_id"]);
  }
  if (document.contains("activated_at_epoch") &&
      document["activated_at_epoch"].is_number_integer()) {
    result.activated_at = document["activated_at_epoch"].get<std::int64_t>();
  }
  *out = std::move(result);
  return true;
}

std::optional<long long> ParseWhole(std::string_view text) {
  long long value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

// ---- Discord Rich Presence --------------------------------------------------

DiscordTextProblem CheckDiscordText(std::string_view text, std::size_t limit) {
  if (text.empty()) return DiscordTextProblem::kEmpty;
  if (std::any_of(text.begin(), text.end(), [](unsigned char byte) {
        return byte < 0x20 || byte == 0x7f;
      })) {
    return DiscordTextProblem::kControlCharacter;
  }
  if (text.size() > limit) return DiscordTextProblem::kTooLong;
  return DiscordTextProblem::kNone;
}

bool IsDiscordApplicationId(std::string_view text) {
  return text.size() >= 17 && text.size() <= 20 && IsDigits(text);
}

std::string RenderDiscordPlaceText(std::string_view text,
                                   std::string_view place_name) {
  constexpr std::string_view kPlaceholder = "{place_name}";
  std::string rendered(text);
  std::size_t offset = 0;
  while ((offset = rendered.find(kPlaceholder, offset)) != std::string::npos) {
    rendered.replace(offset, kPlaceholder.size(), place_name);
    offset += place_name.size();
  }
  return TruncateUtf8(std::move(rendered), kDiscordTextLimit);
}

// ---- proxy ------------------------------------------------------------------

ProxyMode ProxyModeFor(std::string_view use_system_proxy, bool has_host,
                       bool has_port) {
  if (has_host || has_port) return ProxyMode::kManual;
  return use_system_proxy == "true" ? ProxyMode::kSystem : ProxyMode::kNone;
}

ProxyHostProblem CheckProxyHost(std::string_view host) {
  if (host.empty()) return ProxyHostProblem::kEmpty;
  if (host.find("://") != std::string_view::npos) {
    return ProxyHostProblem::kScheme;
  }
  if (std::any_of(host.begin(), host.end(), [](unsigned char character) {
        return character <= 0x20 || character == 0x7f || character == '/' ||
               character == '\\' || character == '@' || character == '[' ||
               character == ']' || character == '?' || character == '#';
      })) {
    return ProxyHostProblem::kInvalidCharacter;
  }
  return ProxyHostProblem::kNone;
}

bool IsValidPort(std::string_view port) {
  if (!IsDigits(port)) return false;
  const std::optional<long long> value = ParseWhole(port);
  return value.has_value() && *value >= 1 && *value <= 65535;
}

// ---- Fleasion ---------------------------------------------------------------

FleasionConflict FindFleasionConflict(const FleasionInputs& inputs) {
  if (!inputs.enabled) return FleasionConflict::kNone;
  if (inputs.use_system_proxy) return FleasionConflict::kSystemProxy;
  const bool fixed_proxy = inputs.network_proxy_host.has_value() ||
                           inputs.network_proxy_port.has_value();
  if (!fixed_proxy) return FleasionConflict::kNone;
  if (inputs.proxy_mode == "hosts") return FleasionConflict::kProxyInHostsMode;
  // env mode: only Fleasion's own endpoint may be configured as well.
  const std::optional<long long> fleasion_port = ParseWhole(inputs.proxy_port);
  const std::optional<long long> network_port =
      ParseWhole(inputs.network_proxy_port.value_or(""));
  if (inputs.network_proxy_host.value_or("") != "127.0.0.1" ||
      !fleasion_port.has_value() || !network_port.has_value() ||
      *fleasion_port != *network_port) {
    return FleasionConflict::kDifferentProxy;
  }
  return FleasionConflict::kNone;
}

std::filesystem::path FleasionConfigDirectory(
    const runtime::Environment& environment) {
  std::filesystem::path root = environment.GetOr("XDG_CONFIG_HOME", "");
  if (!root.is_absolute()) {
    root = std::filesystem::path(environment.GetOr("HOME", "")) / ".config";
  }
  return root / "Fleasion";
}

std::filesystem::path DefaultFleasionCertificate(
    const runtime::Environment& environment) {
  return FleasionConfigDirectory(environment) / "proxy_ca" / "ca.crt";
}

CertificatePathProblem CheckCertificatePath(std::string_view text) {
  if (text.empty()) return CertificatePathProblem::kNone;
  const std::filesystem::path path{std::string(text)};
  if (!path.is_absolute()) return CertificatePathProblem::kRelative;
  if (path.extension() == ".key") return CertificatePathProblem::kPrivateKey;
  return CertificatePathProblem::kNone;
}

// ---- the updater ------------------------------------------------------------

std::filesystem::path ResolveUpdaterHelper(
    const runtime::Environment& environment,
    const std::filesystem::path& executable,
    const std::function<bool(const std::filesystem::path&)>& is_executable) {
  if (const std::optional<std::string> configured =
          environment.Get("MOCKTAIL_UPDATE_HELPER");
      configured.has_value() && !configured->empty()) {
    const std::filesystem::path helper(*configured);
    return helper.is_absolute() && is_executable(helper)
               ? helper
               : std::filesystem::path();
  }
  if (executable.empty()) return {};
  const std::filesystem::path directory = executable.parent_path();
  for (const std::filesystem::path& candidate :
       {directory / "mocktail_updater",
        directory.parent_path() / "libexec" / "mocktail" / "mocktail_updater",
        directory.parent_path() / "lib" / "mocktail" / "mocktail_updater"}) {
    if (is_executable(candidate)) return candidate;
  }
  return {};
}

bool ParseActivePayloadManifest(std::string_view json, InstalledRoblox* out) {
  const nlohmann::json document =
      nlohmann::json::parse(json, nullptr, false, true);
  return !document.is_discarded() && ReadManifest(document, out);
}

bool ParseUpdaterStatus(std::string_view json, InstalledRoblox* out) {
  const nlohmann::json document =
      nlohmann::json::parse(json, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      !document.contains("current")) {
    return false;
  }
  const nlohmann::json& current = document["current"];
  if (current.is_null()) {
    *out = InstalledRoblox();
    return true;
  }
  return ReadManifest(current, out);
}

bool ParseCheckLatest(std::string_view output, LatestRoblox* out) {
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                             output.back() == ' ')) {
    output.remove_suffix(1);
  }
  const std::size_t space = output.find(' ');
  if (space == std::string_view::npos || space == 0) return false;
  const std::string_view name = output.substr(0, space);
  const std::string_view code = output.substr(space + 1);
  if (!IsDigits(code) || name.find_first_of(" \n") != std::string_view::npos) {
    return false;
  }
  out->version_name = std::string(name);
  out->version_code = std::string(code);
  return true;
}

UpdateComparison CompareWithLatest(const InstalledRoblox& installed,
                                   const LatestRoblox& latest) {
  if (!installed.installed) return UpdateComparison::kNotInstalled;
  const std::optional<long long> current = ParseWhole(installed.version_code);
  const std::optional<long long> newest = ParseWhole(latest.version_code);
  if (!current.has_value() || !newest.has_value()) {
    return installed.version_name == latest.version_name
               ? UpdateComparison::kUpToDate
               : UpdateComparison::kNewerAvailable;
  }
  if (*current == *newest) return UpdateComparison::kUpToDate;
  return *current < *newest ? UpdateComparison::kNewerAvailable
                            : UpdateComparison::kInstalledNewer;
}

std::string UpdaterErrorMessage(std::string_view stderr_text) {
  std::string_view last;
  std::size_t start = 0;
  while (start < stderr_text.size()) {
    std::size_t end = stderr_text.find('\n', start);
    if (end == std::string_view::npos) end = stderr_text.size();
    std::string_view line = stderr_text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    if (!line.empty()) last = line;
    start = end + 1;
  }
  constexpr std::string_view kPrefix = "[native-updater] ";
  if (StartsWith(last, kPrefix)) last.remove_prefix(kPrefix.size());
  return std::string(last);
}

// ---- device -----------------------------------------------------------------

std::string CanonicalDevicePreset(std::string_view value) {
  const runtime::DeviceProfile* profile = runtime::FindDeviceProfile(value);
  return profile != nullptr ? profile->name : std::string();
}

// ---- Fast Flags -------------------------------------------------------------

std::optional<std::string> ManagedQualityLevel(
    const FastFlagSettings& settings) {
  if (!RenderingPresetActive(settings.physics_worker_mode,
                             settings.multithreaded_rendering)) {
    return std::nullopt;
  }
  // performance_policy.cc: "manual" (and its "auto"/"0" spellings) leaves
  // the slider to Roblox; a level is forced as given; the default is 3,
  // or 1 where graphics_launch_policy.cc publishes it for Intel-only GPUs.
  const std::string& quality = settings.graphics_quality;
  if (quality == "manual" || quality == "auto" || quality == "0") {
    return std::nullopt;
  }
  if (IsDigits(quality)) return quality;
  return std::string(settings.intel_only_direct_vulkan ? "1" : "3");
}

std::vector<launcher::FastFlagConflict> FindFastFlagConflicts(
    const launcher::FastFlagsDocument& document,
    const FastFlagSettings& settings) {
  const runtime::FrameRatePolicy frame_rate =
      runtime::ParseFrameRatePolicy(settings.frame_rate_limit);
  const runtime::PerformancePolicy performance =
      runtime::ParsePerformancePolicy(
          settings.multithreaded_rendering == "true" ? "1" : "0",
          settings.memory_limit_mb, settings.gamemode,
          settings.physics_worker_mode);
  std::vector<launcher::FastFlagConflict> conflicts =
      document.FindManagedConflicts(frame_rate, performance);
  conflicts.erase(std::remove_if(conflicts.begin(), conflicts.end(),
                                 [](const launcher::FastFlagConflict& entry) {
                                   return entry.name == kQualityFlag;
                                 }),
                  conflicts.end());
  const std::optional<std::string> level = ManagedQualityLevel(settings);
  const launcher::FastFlagEntry* entry = document.Find(kQualityFlag);
  if (level.has_value() && entry != nullptr && entry->RobloxValue() != *level) {
    // Set with SetCompatibleValue: a different value stops the start.
    launcher::FastFlagConflict conflict;
    conflict.name = std::string(kQualityFlag);
    conflict.file_value = entry->RobloxValue();
    conflict.managed_value = *level;
    conflict.effect = launcher::FastFlagConflictEffect::kBlocksStart;
    conflicts.push_back(std::move(conflict));
  }
  return conflicts;
}

launcher::FastFlagValueKind InferFastFlagKind(std::string_view name,
                                              std::string_view value) {
  for (const std::string_view prefix : {"DFFlag", "FFlag", "SFFlag"}) {
    if (StartsWith(name, prefix)) return launcher::FastFlagValueKind::kBoolean;
  }
  for (const std::string_view prefix :
       {"DFInt", "FInt", "SFInt", "DFLog", "FLog"}) {
    if (StartsWith(name, prefix)) return launcher::FastFlagValueKind::kInteger;
  }
  for (const std::string_view prefix : {"DFString", "FString", "SFString"}) {
    if (StartsWith(name, prefix)) return launcher::FastFlagValueKind::kString;
  }
  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  if (lower == "true" || lower == "false") {
    return launcher::FastFlagValueKind::kBoolean;
  }
  std::string_view digits = value;
  if (!digits.empty() && digits.front() == '-') digits.remove_prefix(1);
  if (IsDigits(digits)) return launcher::FastFlagValueKind::kInteger;
  return launcher::FastFlagValueKind::kString;
}

int CountFastFlagChanges(const std::vector<launcher::FastFlagEntry>& saved,
                         const std::vector<launcher::FastFlagEntry>& current) {
  int changes = 0;
  for (const launcher::FastFlagEntry& entry : current) {
    const auto found =
        std::find_if(saved.begin(), saved.end(),
                     [&entry](const launcher::FastFlagEntry& old) {
                       return old.name == entry.name;
                     });
    if (found == saved.end() || found->kind != entry.kind ||
        found->value != entry.value) {
      ++changes;
    }
  }
  for (const launcher::FastFlagEntry& entry : saved) {
    if (std::none_of(current.begin(), current.end(),
                     [&entry](const launcher::FastFlagEntry& now) {
                       return now.name == entry.name;
                     })) {
      ++changes;
    }
  }
  return changes;
}

// ---- diagnostics ------------------------------------------------------------

std::string SessionHeaderField(std::string_view header, std::string_view key) {
  std::size_t search = 0;
  while (search < header.size()) {
    const std::size_t found = header.find(key, search);
    if (found == std::string_view::npos) return {};
    search = found + key.size();
    const bool starts_word = found == 0 || header[found - 1] == ' ' ||
                             header[found - 1] == '\n' ||
                             header[found - 1] == ']';
    if (!starts_word || search >= header.size() || header[search] != '=') {
      continue;
    }
    std::size_t value_start = search + 1;
    if (value_start < header.size() && header[value_start] == '"') {
      ++value_start;
      const std::size_t end = header.find('"', value_start);
      if (end == std::string_view::npos) return {};
      return std::string(header.substr(value_start, end - value_start));
    }
    const std::size_t end = header.find_first_of(" \n", value_start);
    return std::string(header.substr(
        value_start,
        (end == std::string_view::npos ? header.size() : end) - value_start));
  }
  return {};
}

namespace {

// Removes " key=value" (a value without spaces) from a header line.
void EraseField(std::string* line, std::string_view key) {
  const std::size_t start = line->find(" " + std::string(key) + "=");
  if (start == std::string::npos) return;
  const std::size_t end = line->find(' ', start + 1);
  line->erase(start,
              end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

std::string TrimSessionHeader(std::string_view header) {
  constexpr std::string_view kExecutable = "[mocktail] executable=";
  std::string result;
  std::size_t start = 0;
  while (start < header.size()) {
    std::size_t end = header.find('\n', start);
    if (end == std::string_view::npos) end = header.size();
    std::string line(header.substr(start, end - start));
    start = end + 1;
    if (StartsWith(line, "[mocktail] log=")) continue;
    // The settings window's own process: no game was started, and the
    // executable is the helper, not mocktail.
    EraseField(&line, "pid");
    EraseField(&line, "started");
    if (StartsWith(line, kExecutable)) {
      // The path may hold spaces; config= follows it.
      const std::size_t config = line.find(" config=");
      line = config == std::string::npos
                 ? std::string()
                 : "[mocktail]" + line.substr(config);
    }
    if (line.empty()) continue;
    result += line;
    result += '\n';
  }
  return result;
}

std::string FormatDiagnosticLines(
    const std::vector<std::pair<std::string, std::string>>& lines) {
  std::string text;
  for (const auto& [name, value] : lines) {
    text += name;
    text += ": ";
    text += value.empty() ? std::string("-") : value;
    text += '\n';
  }
  return text;
}

const std::vector<std::string_view>& DiagnosticSettingKeys() {
  static const std::vector<std::string_view> keys = {
      "device",
      "appearance.theme",
      "graphics.backend",
      "graphics.frame_rate_limit",
      "graphics.vsync",
      "engine.graphics_quality",
      "engine.gpu",
      "engine.nvidia_shader_mt",
      "performance.multithreaded_rendering",
      "performance.physics_worker_mode",
      "performance.memory_limit_mb",
      "performance.gamemode",
      "window.width",
      "window.height",
      "window.high_dpi",
      "display.server",
      "display.start_mode",
      "account.sign_in",
      "network.use_system_proxy",
      "integrations.discord_rpc.enabled",
      "integrations.fleasion.enabled",
      "integrations.fleasion.proxy_mode",
      "updates.automatic",
      "launcher.show_on_start",
  };
  return keys;
}

}  // namespace mocktail::launcher_ui
