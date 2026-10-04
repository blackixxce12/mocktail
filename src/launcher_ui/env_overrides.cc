#include "launcher_ui/env_overrides.h"

#include <algorithm>
#include <string>

#include "launcher_ui/setting_kinds.h"
#include "launcher_ui/settings_draft.h"
#include "runtime/launcher_ui_launch.h"
#include "runtime/managed_environment.h"

namespace mocktail::launcher_ui {
namespace {

// The order in which the game honours the variables that pick the display
// server: an explicit SDL driver always wins, then the force switches
// (src/window/video_driver_policy.cc:10-29), and MOCKTAIL_DISPLAY_SERVER
// only turns into a force switch when none of them is set
// (graphics_launch_policy.cc, UserSelectsVideoDriver).
constexpr std::string_view kDisplayServerPrecedence[] = {
    "SDL_VIDEODRIVER",    "SDL_VIDEO_DRIVER",         "MOCKTAIL_FORCE_WAYLAND",
    "MOCKTAIL_FORCE_X11", "MOCKTAIL_ANGLE_FORCE_X11", "MOCKTAIL_DISPLAY_SERVER",
};

int Precedence(std::string_view name) {
  const auto found = std::find(std::begin(kDisplayServerPrecedence),
                               std::end(kDisplayServerPrecedence), name);
  return found == std::end(kDisplayServerPrecedence)
             ? 0
             : static_cast<int>(found - std::begin(kDisplayServerPrecedence));
}

}  // namespace

EnvOverrides EnvOverrides::FromEnvironment(
    const runtime::Environment& environment) {
  return FromNames(
      environment.GetOr(runtime::kLauncherUiEnvOverridesVariable, ""),
      environment);
}

EnvOverrides EnvOverrides::FromNames(std::string_view names,
                                     const runtime::Environment& environment) {
  EnvOverrides result;
  while (!names.empty()) {
    const std::size_t comma = names.find(',');
    const std::string_view name = names.substr(0, comma);
    names = comma == std::string_view::npos ? std::string_view()
                                            : names.substr(comma + 1);
    const runtime::ManagedEnvironmentVariable* variable =
        runtime::FindManagedEnvironmentVariable(name);
    if (variable == nullptr ||
        std::any_of(
            result.overrides_.begin(), result.overrides_.end(),
            [name](const EnvOverride& entry) { return entry.name == name; })) {
      continue;
    }
    const std::optional<std::string> value = environment.Get(name);
    if (!value.has_value()) {
      continue;
    }
    EnvOverride entry;
    entry.name = std::string(name);
    entry.value = *value;
    entry.yaml_key = std::string(variable->yaml_key);
    entry.imported = runtime::ImportManagedEnvironmentValue(name, *value);
    result.overrides_.push_back(std::move(entry));
  }
  // Mark everything but the first variable of each key in startup order.
  for (EnvOverride& entry : result.overrides_) {
    const std::vector<const EnvOverride*> same = result.ForKey(entry.yaml_key);
    entry.shadowed = !same.empty() && same.front() != &entry;
  }
  return result;
}

std::vector<const EnvOverride*> EnvOverrides::ForKey(
    std::string_view key) const {
  std::vector<const EnvOverride*> matches;
  for (const EnvOverride& entry : overrides_) {
    if (entry.yaml_key == key) {
      matches.push_back(&entry);
    }
  }
  std::stable_sort(matches.begin(), matches.end(),
                   [](const EnvOverride* left, const EnvOverride* right) {
                     return Precedence(left->name) < Precedence(right->name);
                   });
  return matches;
}

const EnvOverride* EnvOverrides::Effective(std::string_view key) const {
  const std::vector<const EnvOverride*> matches = ForKey(key);
  return matches.empty() ? nullptr : matches.front();
}

int EnvOverrides::SettingCount() const {
  std::vector<std::string_view> keys;
  for (const EnvOverride& entry : overrides_) {
    if (std::find(keys.begin(), keys.end(), entry.yaml_key) == keys.end()) {
      keys.push_back(entry.yaml_key);
    }
  }
  return static_cast<int>(keys.size());
}

std::string RedactEnvironmentValue(std::string_view value) {
  // scheme://user:password@host -> scheme://***@host
  const std::size_t scheme = value.find("://");
  if (scheme != std::string_view::npos) {
    const std::size_t start = scheme + 3;
    const std::size_t at = value.find('@', start);
    const std::size_t slash = value.find('/', start);
    if (at != std::string_view::npos &&
        (slash == std::string_view::npos || at < slash)) {
      return std::string(value.substr(0, start)) + "***" +
             std::string(value.substr(at));
    }
  }
  return std::string(value);
}

EnvImportReport ImportEnvOverrides(const EnvOverrides& overrides,
                                   SettingsDraft* draft) {
  EnvImportReport report;
  for (const EnvOverride& entry : overrides.all()) {
    if (entry.shadowed || !entry.imported.has_value()) {
      report.skipped.push_back(&entry);
      continue;
    }
    std::string error;
    if (draft == nullptr ||
        !draft->Set(entry.yaml_key, *entry.imported,
                    ScalarKindFor(entry.yaml_key, *entry.imported), &error)) {
      report.errors.push_back(entry.yaml_key + ": " + error);
      report.skipped.push_back(&entry);
      continue;
    }
    report.imported.push_back(&entry);
  }
  return report;
}

}  // namespace mocktail::launcher_ui
