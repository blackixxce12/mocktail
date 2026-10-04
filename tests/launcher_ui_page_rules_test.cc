// GTK-free rules of the Integrations, Network & Updates, Advanced and About
// pages of the settings window (src/launcher_ui/page_rules.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "launcher/fast_flags_document.h"
#include "launcher_ui/page_rules.h"

namespace mocktail::launcher_ui {
namespace {

using launcher::FastFlagConflictEffect;
using launcher::FastFlagsDocument;
using launcher::FastFlagValueKind;

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

TEST(LauncherUiPageRulesTest, DiscordTextsFollowTheLoaderLimits) {
  EXPECT_EQ(CheckDiscordText("Join Server", kDiscordButtonLabelLimit),
            DiscordTextProblem::kNone);
  EXPECT_EQ(CheckDiscordText("", kDiscordTextLimit),
            DiscordTextProblem::kEmpty);
  EXPECT_EQ(CheckDiscordText(std::string(32, 'a'), kDiscordButtonLabelLimit),
            DiscordTextProblem::kNone);
  EXPECT_EQ(CheckDiscordText(std::string(33, 'a'), kDiscordButtonLabelLimit),
            DiscordTextProblem::kTooLong);
  // Bytes, not characters: 17 Cyrillic letters are 34 bytes.
  std::string cyrillic;
  for (int index = 0; index < 17; ++index) cyrillic += "я";
  EXPECT_EQ(CheckDiscordText(cyrillic, kDiscordButtonLabelLimit),
            DiscordTextProblem::kTooLong);
  EXPECT_EQ(CheckDiscordText("two\nlines", kDiscordTextLimit),
            DiscordTextProblem::kControlCharacter);
  EXPECT_EQ(CheckDiscordText("tab\there", kDiscordTextLimit),
            DiscordTextProblem::kControlCharacter);
}

TEST(LauncherUiPageRulesTest, DiscordApplicationIdIsASnowflake) {
  EXPECT_TRUE(IsDiscordApplicationId("1537088975720812655"));
  EXPECT_TRUE(IsDiscordApplicationId("12345678901234567"));
  EXPECT_TRUE(IsDiscordApplicationId("12345678901234567890"));
  EXPECT_FALSE(IsDiscordApplicationId("1234567890123456"));
  EXPECT_FALSE(IsDiscordApplicationId("123456789012345678901"));
  EXPECT_FALSE(IsDiscordApplicationId("15370889757208126x5"));
  EXPECT_FALSE(IsDiscordApplicationId(""));
}

TEST(LauncherUiPageRulesTest, PlaceTextIsRenderedLikeTheRuntime) {
  EXPECT_EQ(RenderDiscordPlaceText("{place_name}", "Tower of Hell"),
            "Tower of Hell");
  EXPECT_EQ(RenderDiscordPlaceText("Playing {place_name} ({place_name})", "X"),
            "Playing X (X)");
  EXPECT_EQ(RenderDiscordPlaceText("No placeholder", "X"), "No placeholder");
  // Cut to 128 bytes without splitting a character.
  std::string long_name;
  for (int index = 0; index < 100; ++index) long_name += "ж";
  const std::string rendered =
      RenderDiscordPlaceText("{place_name}", long_name);
  EXPECT_LE(rendered.size(), kDiscordTextLimit);
  EXPECT_EQ(rendered.size() % 2, 0U);
}

TEST(LauncherUiPageRulesTest, ProxyModeAndHostRules) {
  EXPECT_EQ(ProxyModeFor("false", false, false), ProxyMode::kNone);
  EXPECT_EQ(ProxyModeFor("true", false, false), ProxyMode::kSystem);
  EXPECT_EQ(ProxyModeFor("false", true, true), ProxyMode::kManual);
  // Half a fixed proxy is still the manual mode (and must be completed).
  EXPECT_EQ(ProxyModeFor("false", false, true), ProxyMode::kManual);
  EXPECT_EQ(ProxyModeFor("true", true, false), ProxyMode::kManual);

  EXPECT_EQ(CheckProxyHost("127.0.0.1"), ProxyHostProblem::kNone);
  EXPECT_EQ(CheckProxyHost("proxy.example.org"), ProxyHostProblem::kNone);
  EXPECT_EQ(CheckProxyHost("::1"), ProxyHostProblem::kNone);
  EXPECT_EQ(CheckProxyHost(""), ProxyHostProblem::kEmpty);
  EXPECT_EQ(CheckProxyHost("http://proxy"), ProxyHostProblem::kScheme);
  EXPECT_EQ(CheckProxyHost("user@proxy"), ProxyHostProblem::kInvalidCharacter);
  EXPECT_EQ(CheckProxyHost("proxy/path"), ProxyHostProblem::kInvalidCharacter);
  EXPECT_EQ(CheckProxyHost("[::1]"), ProxyHostProblem::kInvalidCharacter);
  EXPECT_EQ(CheckProxyHost("a b"), ProxyHostProblem::kInvalidCharacter);

  EXPECT_TRUE(IsValidPort("1"));
  EXPECT_TRUE(IsValidPort("65535"));
  EXPECT_FALSE(IsValidPort("0"));
  EXPECT_FALSE(IsValidPort("65536"));
  EXPECT_FALSE(IsValidPort("-1"));
  EXPECT_FALSE(IsValidPort("80a"));
  EXPECT_FALSE(IsValidPort(""));
}

TEST(LauncherUiPageRulesTest, FleasionConflictsMatchTheRuntime) {
  FleasionInputs inputs;
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kNone);
  // Disabled: nothing conflicts.
  inputs.use_system_proxy = true;
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kNone);
  inputs.enabled = true;
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kSystemProxy);
  inputs.use_system_proxy = false;
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kNone);
  inputs.network_proxy_host = "10.0.0.2";
  inputs.network_proxy_port = "8080";
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kDifferentProxy);
  // Fleasion's own endpoint is allowed in env mode.
  inputs.network_proxy_host = "127.0.0.1";
  inputs.network_proxy_port = "58443";
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kNone);
  inputs.proxy_mode = "hosts";
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kProxyInHostsMode);
  inputs.network_proxy_host.reset();
  inputs.network_proxy_port.reset();
  EXPECT_EQ(FindFleasionConflict(inputs), FleasionConflict::kNone);
}

TEST(LauncherUiPageRulesTest, FleasionPathsFollowXdgConfigHome) {
  EXPECT_EQ(DefaultFleasionCertificate(MapEnvironment(
                {{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "/cfg"}})),
            std::filesystem::path("/cfg/Fleasion/proxy_ca/ca.crt"));
  // A relative XDG_CONFIG_HOME is ignored, as fleasion.cc does.
  EXPECT_EQ(FleasionConfigDirectory(MapEnvironment(
                {{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "cfg"}})),
            std::filesystem::path("/home/u/.config/Fleasion"));

  EXPECT_EQ(CheckCertificatePath(""), CertificatePathProblem::kNone);
  EXPECT_EQ(CheckCertificatePath("/home/u/.config/Fleasion/proxy_ca/ca.crt"),
            CertificatePathProblem::kNone);
  EXPECT_EQ(CheckCertificatePath("ca.crt"), CertificatePathProblem::kRelative);
  EXPECT_EQ(CheckCertificatePath("/home/u/.config/Fleasion/proxy_ca/ca.key"),
            CertificatePathProblem::kPrivateKey);
}

TEST(LauncherUiPageRulesTest, UpdaterIsFoundWhereMocktailLooks) {
  std::set<std::string> executables;
  const auto is_executable = [&executables](const std::filesystem::path& path) {
    return executables.count(path.string()) != 0;
  };
  const MapEnvironment empty;
  EXPECT_TRUE(ResolveUpdaterHelper(empty, "/usr/lib/mocktail/ui", is_executable)
                  .empty());
  executables = {"/usr/lib/mocktail/mocktail_updater"};
  EXPECT_EQ(ResolveUpdaterHelper(empty, "/usr/lib/mocktail/ui", is_executable),
            std::filesystem::path("/usr/lib/mocktail/mocktail_updater"));
  EXPECT_EQ(ResolveUpdaterHelper(empty, "/usr/bin/ui", is_executable),
            std::filesystem::path("/usr/lib/mocktail/mocktail_updater"));
  executables.insert("/opt/m/libexec/mocktail/mocktail_updater");
  EXPECT_EQ(ResolveUpdaterHelper(empty, "/opt/m/bin/ui", is_executable),
            std::filesystem::path("/opt/m/libexec/mocktail/mocktail_updater"));
  // The override wins and must be an absolute executable.
  executables.insert("/x/updater");
  EXPECT_EQ(ResolveUpdaterHelper(
                MapEnvironment({{"MOCKTAIL_UPDATE_HELPER", "/x/updater"}}),
                "/usr/lib/mocktail/ui", is_executable),
            std::filesystem::path("/x/updater"));
  EXPECT_TRUE(ResolveUpdaterHelper(
                  MapEnvironment({{"MOCKTAIL_UPDATE_HELPER", "x/updater"}}),
                  "/usr/lib/mocktail/ui", is_executable)
                  .empty());
}

TEST(LauncherUiPageRulesTest, ParsesTheUpdaterStatusAndManifest) {
  constexpr std::string_view kManifest = R"({
    "schema_version": 1,
    "payload_id": "2.736.1408-2998-abc",
    "payload_path": "payloads/2.736.1408-2998-abc",
    "version_name": "2.736.1408",
    "version_code": 2998,
    "elf_build_id": "0123456789abcdef0123",
    "activated_at_epoch": 1789000000
  })";
  InstalledRoblox installed;
  ASSERT_TRUE(ParseActivePayloadManifest(kManifest, &installed));
  EXPECT_TRUE(installed.installed);
  EXPECT_EQ(installed.version_name, "2.736.1408");
  EXPECT_EQ(installed.version_code, "2998");
  EXPECT_EQ(installed.build_id, "0123456789abcdef0123");
  EXPECT_EQ(installed.activated_at, 1789000000);

  const std::string status = std::string("{\"current\": ") +
                             std::string(kManifest) +
                             ", \"previous_good\": null}\n";
  InstalledRoblox from_status;
  ASSERT_TRUE(ParseUpdaterStatus(status, &from_status));
  EXPECT_EQ(from_status.version_name, "2.736.1408");

  InstalledRoblox nothing;
  nothing.installed = true;
  ASSERT_TRUE(ParseUpdaterStatus("{\"current\": null, \"previous_good\": null}",
                                 &nothing));
  EXPECT_FALSE(nothing.installed);

  EXPECT_FALSE(ParseUpdaterStatus("not json", &nothing));
  EXPECT_FALSE(ParseUpdaterStatus("{}", &nothing));
  EXPECT_FALSE(ParseActivePayloadManifest("{\"schema_version\": 2}", &nothing));
  EXPECT_FALSE(ParseActivePayloadManifest(
      "{\"schema_version\": 1, \"payload_id\": 5}", &nothing));
}

TEST(LauncherUiPageRulesTest, ComparesTheLatestVersion) {
  LatestRoblox latest;
  ASSERT_TRUE(ParseCheckLatest("2.737.500 3001\n", &latest));
  EXPECT_EQ(latest.version_name, "2.737.500");
  EXPECT_EQ(latest.version_code, "3001");
  LatestRoblox ignored;
  EXPECT_FALSE(ParseCheckLatest("", &ignored));
  EXPECT_FALSE(ParseCheckLatest("2.737.500", &ignored));
  EXPECT_FALSE(ParseCheckLatest("2.737.500 abc", &ignored));

  InstalledRoblox installed;
  EXPECT_EQ(CompareWithLatest(installed, latest),
            UpdateComparison::kNotInstalled);
  installed.installed = true;
  installed.version_name = "2.736.1408";
  installed.version_code = "2998";
  EXPECT_EQ(CompareWithLatest(installed, latest),
            UpdateComparison::kNewerAvailable);
  installed.version_code = "3001";
  EXPECT_EQ(CompareWithLatest(installed, latest), UpdateComparison::kUpToDate);
  installed.version_code = "3002";
  EXPECT_EQ(CompareWithLatest(installed, latest),
            UpdateComparison::kInstalledNewer);

  EXPECT_EQ(UpdaterErrorMessage("noise\n[native-updater] provider offline\n\n"),
            "provider offline");
  EXPECT_EQ(UpdaterErrorMessage(""), "");
}

TEST(LauncherUiPageRulesTest, DevicePresetsAndAliases) {
  EXPECT_EQ(CanonicalDevicePreset("pc"), "pc-windows-11");
  EXPECT_EQ(CanonicalDevicePreset("pc-windows-11"), "pc-windows-11");
  EXPECT_EQ(CanonicalDevicePreset("mobile"), "mobile-pixel-7");
  EXPECT_EQ(CanonicalDevicePreset("console-xbox-series-x"), "console-ps5");
  EXPECT_EQ(CanonicalDevicePreset("toaster"), "");
}

FastFlagsDocument FlagsDocument(std::string_view json) {
  FastFlagsDocument document;
  std::string error;
  EXPECT_TRUE(FastFlagsDocument::FromBytes(json, &document, &error)) << error;
  return document;
}

const launcher::FastFlagConflict* FindConflict(
    const std::vector<launcher::FastFlagConflict>& conflicts,
    std::string_view name) {
  for (const launcher::FastFlagConflict& conflict : conflicts) {
    if (conflict.name == name) return &conflict;
  }
  return nullptr;
}

TEST(LauncherUiPageRulesTest, FastFlagConflictsFollowTheQualitySetting) {
  const FastFlagsDocument document = FlagsDocument(
      R"({"DFIntTaskSchedulerTargetFps": 60,
          "FIntDebugFRMQualityLevelOverride": 5,
          "FFlagDebugDisplayFPS": true})");
  FastFlagSettings settings;
  settings.frame_rate_limit = "165";
  std::vector<launcher::FastFlagConflict> conflicts =
      FindFastFlagConflicts(document, settings);
  const launcher::FastFlagConflict* target =
      FindConflict(conflicts, "DFIntTaskSchedulerTargetFps");
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->managed_value, "165");
  EXPECT_EQ(target->effect, FastFlagConflictEffect::kBlocksStart);
  // The preset (throughput) forces level 3 by default.
  const launcher::FastFlagConflict* quality =
      FindConflict(conflicts, "FIntDebugFRMQualityLevelOverride");
  ASSERT_NE(quality, nullptr);
  EXPECT_EQ(quality->managed_value, "3");
  EXPECT_EQ(quality->effect, FastFlagConflictEffect::kBlocksStart);
  EXPECT_EQ(FindConflict(conflicts, "FFlagDebugDisplayFPS"), nullptr);

  // A configured level that agrees, manual quality, or no preset: no
  // quality conflict.
  settings.graphics_quality = "5";
  EXPECT_EQ(FindConflict(FindFastFlagConflicts(document, settings),
                         "FIntDebugFRMQualityLevelOverride"),
            nullptr);
  settings.graphics_quality = "manual";
  EXPECT_EQ(FindConflict(FindFastFlagConflicts(document, settings),
                         "FIntDebugFRMQualityLevelOverride"),
            nullptr);
  settings.graphics_quality = "default";
  settings.physics_worker_mode = "latency";
  EXPECT_EQ(FindConflict(FindFastFlagConflicts(document, settings),
                         "FIntDebugFRMQualityLevelOverride"),
            nullptr);
  // Roblox owning the frame rate: no target conflict.
  settings.frame_rate_limit = "-1";
  EXPECT_EQ(FindConflict(FindFastFlagConflicts(document, settings),
                         "DFIntTaskSchedulerTargetFps"),
            nullptr);
}

TEST(LauncherUiPageRulesTest, ManagedQualityLevel) {
  FastFlagSettings settings;
  EXPECT_EQ(ManagedQualityLevel(settings), std::optional<std::string>("3"));
  settings.intel_only_direct_vulkan = true;
  EXPECT_EQ(ManagedQualityLevel(settings), std::optional<std::string>("1"));
  settings.graphics_quality = "12";
  EXPECT_EQ(ManagedQualityLevel(settings), std::optional<std::string>("12"));
  settings.physics_worker_mode = "auto";
  EXPECT_EQ(ManagedQualityLevel(settings), std::nullopt);
  settings.multithreaded_rendering = "true";
  EXPECT_EQ(ManagedQualityLevel(settings), std::optional<std::string>("12"));
}

TEST(LauncherUiPageRulesTest, FastFlagKindsAndChanges) {
  EXPECT_EQ(InferFastFlagKind("FFlagDebugDisplayFPS", "1"),
            FastFlagValueKind::kBoolean);
  EXPECT_EQ(InferFastFlagKind("DFIntTaskSchedulerTargetFps", "60"),
            FastFlagValueKind::kInteger);
  EXPECT_EQ(InferFastFlagKind("FLogNetwork", "7"), FastFlagValueKind::kInteger);
  EXPECT_EQ(InferFastFlagKind("FStringDebugX", "123"),
            FastFlagValueKind::kString);
  EXPECT_EQ(InferFastFlagKind("Custom", "True"), FastFlagValueKind::kBoolean);
  EXPECT_EQ(InferFastFlagKind("Custom", "-12"), FastFlagValueKind::kInteger);
  EXPECT_EQ(InferFastFlagKind("Custom", "text"), FastFlagValueKind::kString);

  const std::vector<launcher::FastFlagEntry> saved = {
      {"A", FastFlagValueKind::kInteger, "1"},
      {"B", FastFlagValueKind::kBoolean, "true"},
  };
  EXPECT_EQ(CountFastFlagChanges(saved, saved), 0);
  const std::vector<launcher::FastFlagEntry> current = {
      {"A", FastFlagValueKind::kInteger, "2"},
      {"C", FastFlagValueKind::kString, "x"},
  };
  // A changed, C added, B removed.
  EXPECT_EQ(CountFastFlagChanges(saved, current), 3);
}

TEST(LauncherUiPageRulesTest, SessionHeaderFields) {
  constexpr std::string_view kHeader =
      "[mocktail] version=1.0.4 commit=abc123 build=Release compiler=\"GNU "
      "15\"\n"
      "[mocktail] started=2026-10-05T10:00:00 package=build target=x86_64 "
      "host=\"Linux 7.2 x86_64\" pid=4242\n"
      "[mocktail] cpu=\"AMD Ryzen 7 5800H\" threads=16 ram=15.5GiB "
      "gpu=\"NVIDIA GA106M; AMD Cezanne\" gpu_driver=\"nvidia; amdgpu\" "
      "display=wayland graphics=direct-vulkan\n"
      "[mocktail] executable=~/bin/mocktail config=~/.config/mocktail/"
      "config.yaml\n"
      "[mocktail] log=\n";
  EXPECT_EQ(SessionHeaderField(kHeader, "version"), "1.0.4");
  EXPECT_EQ(SessionHeaderField(kHeader, "cpu"), "AMD Ryzen 7 5800H");
  EXPECT_EQ(SessionHeaderField(kHeader, "gpu"), "NVIDIA GA106M; AMD Cezanne");
  EXPECT_EQ(SessionHeaderField(kHeader, "gpu_driver"), "nvidia; amdgpu");
  EXPECT_EQ(SessionHeaderField(kHeader, "ram"), "15.5GiB");
  EXPECT_EQ(SessionHeaderField(kHeader, "threads"), "16");
  EXPECT_EQ(SessionHeaderField(kHeader, "missing"), "");

  const std::string trimmed = TrimSessionHeader(kHeader);
  EXPECT_EQ(trimmed.find("log="), std::string::npos);
  EXPECT_EQ(trimmed.find("pid="), std::string::npos);
  EXPECT_NE(trimmed.find("host=\"Linux 7.2 x86_64\"\n"), std::string::npos);

  EXPECT_EQ(FormatDiagnosticLines({{"Mocktail", "1.0.4"}, {"Proxy", ""}}),
            "Mocktail: 1.0.4\nProxy: -\n");
}

TEST(LauncherUiPageRulesTest, DiagnosticSettingsLeaveOutPrivateValues) {
  const std::vector<std::string_view>& keys = DiagnosticSettingKeys();
  for (const std::string_view key :
       {"network.proxy_host", "network.proxy_port", "network.ca_bundle",
        "integrations.fleasion.ca_certificate",
        "integrations.discord_rpc.application_id",
        "integrations.discord_rpc.text.playing", "audio.output_device",
        "audio.input_device", "window.title", "runtime.roblox_library"}) {
    EXPECT_EQ(std::find(keys.begin(), keys.end(), key), keys.end()) << key;
  }
  EXPECT_NE(std::find(keys.begin(), keys.end(), "graphics.backend"),
            keys.end());
}

}  // namespace
}  // namespace mocktail::launcher_ui
