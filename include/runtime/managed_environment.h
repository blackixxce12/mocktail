#ifndef MOCKTAIL_RUNTIME_MANAGED_ENVIRONMENT_H_
#define MOCKTAIL_RUNTIME_MANAGED_ENVIRONMENT_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// Converts one environment value into the config.yaml scalar with the same
// effect, spelled the way the loader expects it. Returns nullopt when the
// value has no config.yaml equivalent (for example SDL_VIDEODRIVER=kmsdrm)
// or when Mocktail itself would reject it.
using ManagedEnvironmentImporter =
    std::optional<std::string> (*)(std::string_view value);

// An environment variable that overrides a setting the settings window
// edits. Several variables can override the same key; display.server, for
// example, is decided by SDL_VIDEODRIVER and the MOCKTAIL_FORCE_* switches
// as well as by MOCKTAIL_DISPLAY_SERVER.
struct ManagedEnvironmentVariable {
  std::string_view name;
  // Dotted config.yaml key, such as "graphics.vsync". "device" is the
  // one-line device preset.
  std::string_view yaml_key;
  ManagedEnvironmentImporter importer;
};

// Every managed variable, grouped by setting in settings-window order.
const std::vector<ManagedEnvironmentVariable>& ManagedEnvironmentVariables();

const ManagedEnvironmentVariable* FindManagedEnvironmentVariable(
    std::string_view name);

// Imports `value` of the managed variable `name`; nullopt for an unmanaged
// name or a value that cannot be imported.
std::optional<std::string> ImportManagedEnvironmentValue(
    std::string_view name, std::string_view value);

// Names of the managed variables present in `environment`, an environ-style
// null-terminated "NAME=value" array, in ManagedEnvironmentVariables()
// order and without duplicates. A variable that is present but empty
// counts: it still hides the config.yaml value.
std::vector<std::string> CaptureUserManagedEnvironment(
    const char* const* environment);

// The same for a list of "NAME=value" entries. main() passes the
// environment ProcessStartState captured as its first statement, before
// Mocktail exports anything itself, so the result is exactly what the
// user's shell or shortcut set, and it agrees with the environment a
// restart after website sign-in starts from.
std::vector<std::string> CaptureUserManagedEnvironment(
    const std::vector<std::string>& environment);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_MANAGED_ENVIRONMENT_H_
