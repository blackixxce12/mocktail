#include "runtime/launcher_policy.h"

#include <optional>
#include <string>
#include <utility>

#include "runtime/runtime_config.h"
#include "runtime/runtime_config_file.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kShowOnStartVariable =
    "MOCKTAIL_LAUNCHER_SHOW_ON_START";

class EmptyEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

std::pair<LauncherDecision, std::string_view> Decide(
    const LauncherInputs& inputs) {
  if (!inputs.run_mode) {
    return {LauncherDecision::kSkip, "not an interactive run"};
  }
  if (inputs.external_launch_request) {
    return {LauncherDecision::kSkip, "website join"};
  }
  if (inputs.isolated_canary) {
    return {LauncherDecision::kSkip, "update canary"};
  }
  if (inputs.unsafe_latest) {
    return {LauncherDecision::kSkip, "unsafe latest run"};
  }
  if (inputs.already_decided) {
    return {LauncherDecision::kSkip, "re-executed start"};
  }
  if (inputs.headless) {
    return {LauncherDecision::kSkip, "headless run"};
  }
  if (!inputs.display_available) {
    return {LauncherDecision::kSkip, "no display"};
  }
  if (!inputs.helper_present) {
    return {LauncherDecision::kSkip, "settings window helper missing"};
  }
  if (inputs.cli_skip) {
    return {LauncherDecision::kSkip, "--play"};
  }
  if (inputs.cli_force_show) {
    return {LauncherDecision::kShow, "--launcher"};
  }
  return {inputs.show_on_start ? LauncherDecision::kShow
                               : LauncherDecision::kSkip,
          "launcher.show_on_start"};
}

}  // namespace

LauncherDecision DecideLauncher(const LauncherInputs& inputs) {
  return Decide(inputs).first;
}

std::string_view DescribeLauncherDecision(const LauncherInputs& inputs) {
  return Decide(inputs).second;
}

void ReadLauncherEnvironment(const Environment& environment,
                             LauncherInputs* inputs) {
  if (inputs == nullptr) {
    return;
  }
  inputs->isolated_canary =
      environment.Get("MOCKTAIL_ISOLATED_CANARY") == "1";
  inputs->unsafe_latest = environment.Get("MOCKTAIL_UNSAFE_LATEST") == "1";
  inputs->already_decided = environment.Get(kLauncherDecidedVariable) == "1";
  // RuntimeConfig reads MOCKTAIL_HEADLESS the same way.
  inputs->headless = Enabled(environment, "MOCKTAIL_HEADLESS");
  inputs->display_available = environment.HasNonEmpty("DISPLAY") ||
                              environment.HasNonEmpty("WAYLAND_DISPLAY");
}

bool ReadLauncherShowOnStart(const std::filesystem::path& config_file,
                             const Environment& environment) {
  if (const std::optional<std::string> configured =
          environment.Get(kShowOnStartVariable);
      configured.has_value()) {
    if (configured->empty()) {
      return true;
    }
    return ParseEnvironmentSwitch(*configured).value_or(true);
  }
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(EmptyEnvironment(), config_file);
  return !loaded || loaded.config.launcher().show_on_start;
}

bool ReadLauncherShowOnStart(const std::filesystem::path& config_file) {
  return ReadLauncherShowOnStart(config_file, ProcessEnvironment());
}

}  // namespace runtime
}  // namespace mocktail
