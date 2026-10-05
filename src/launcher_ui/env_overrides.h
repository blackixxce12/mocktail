#ifndef MOCKTAIL_LAUNCHER_UI_ENV_OVERRIDES_H_
#define MOCKTAIL_LAUNCHER_UI_ENV_OVERRIDES_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"

namespace mocktail::launcher_ui {

class SettingsDraft;

// One variable from the user's shell or desktop shortcut that overrides a
// setting the window edits. The environment wins over config.yaml at
// startup (runtime_config_file.h), so while it is set the row's value is
// not what the game uses.
struct EnvOverride {
  std::string name;
  std::string value;
  // The config.yaml key it overrides ("display.server").
  std::string yaml_key;
  // The config.yaml value with the same effect, when there is one.
  std::optional<std::string> imported;
  // Another variable for the same key wins at startup, so this one has no
  // effect of its own (SDL_VIDEODRIVER beats MOCKTAIL_DISPLAY_SERVER).
  bool shadowed = false;
  // A command-line option set it for this launch only (`option`, such as
  // --graphics): it is not the user's environment, Move into Settings
  // leaves it alone, and it stays on "play ignore-env".
  bool command_line = false;
  std::string option;
};

// The overrides the window was started with: the names mocktail passed in
// MOCKTAIL_LAUNCHER_ENV_OVERRIDES (the user's managed variables, captured
// before mocktail set anything itself) and MOCKTAIL_LAUNCHER_CLI_OVERRIDES
// (the ones its command line set), with their current values.
class EnvOverrides {
 public:
  static EnvOverrides FromEnvironment(const runtime::Environment& environment);
  // `names` as in MOCKTAIL_LAUNCHER_ENV_OVERRIDES and `command_line_names`
  // as in MOCKTAIL_LAUNCHER_CLI_OVERRIDES; names that are not managed
  // variables, or not set in `environment`, are dropped. A name in both is
  // the command line's: its value replaced the user's.
  static EnvOverrides FromNames(std::string_view comma_separated_names,
                                const runtime::Environment& environment,
                                std::string_view command_line_names = {});

  const std::vector<EnvOverride>& all() const { return overrides_; }
  bool empty() const { return overrides_.empty(); }
  // Overrides of `key`, the one that takes effect first.
  std::vector<const EnvOverride*> ForKey(std::string_view key) const;
  // The variable whose value the game uses for `key`, if any.
  const EnvOverride* Effective(std::string_view key) const;
  // Distinct settings overridden.
  int SettingCount() const;
  // Distinct settings the user's environment overrides; the command line's
  // options are left out.
  int EnvironmentSettingCount() const;
  // Some override comes from the user's environment, and can be moved into
  // the settings.
  bool HasEnvironment() const { return EnvironmentSettingCount() > 0; }

 private:
  std::vector<EnvOverride> overrides_;
};

// A value safe to show: a user name and password in a URL are masked.
std::string RedactEnvironmentValue(std::string_view value);

struct EnvImportReport {
  // "NAME" -> key=value moved into the draft.
  std::vector<const EnvOverride*> imported;
  // Variables whose value has no config.yaml form, or that are shadowed.
  std::vector<const EnvOverride*> skipped;
  // Keys the draft refused, with the reason.
  std::vector<std::string> errors;
};

// Writes the effective override of every key the user's environment
// overrides into the draft ("Move into settings"); the command line's are
// skipped. The caller then asks mocktail to leave the variables out of
// this launch.
EnvImportReport ImportEnvOverrides(const EnvOverrides& overrides,
                                   SettingsDraft* draft);

// The draft holds every value ImportEnvOverrides would move into it. Once
// it does not (a discard or a reload dropped them), leaving the variables
// out of the launch would start Roblox with neither their values nor
// saved ones.
bool DraftHoldsEnvOverrides(const EnvOverrides& overrides,
                            const SettingsDraft& draft);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ENV_OVERRIDES_H_
