#ifndef MOCKTAIL_RUNTIME_HOST_LAUNCH_ENVIRONMENT_H_
#define MOCKTAIL_RUNTIME_HOST_LAUNCH_ENVIRONMENT_H_

#include <string_view>

typedef struct _GAppLaunchContext GAppLaunchContext;

namespace mocktail {
namespace runtime {

// Variables Mocktail sets for its own process: every MOCKTAIL_* setting, the
// game window's graphics and driver choices, loader paths, and the bundled
// WebKit paths that the portable launcher exports in standalone mode. None of
// them may reach an application opened for the user, such as the browser.
bool IsMocktailOnlyEnvironmentVariable(std::string_view name,
                                       bool standalone_bundle);

// Unsets those variables in the environment that |context| hands to the
// launched application.
void RemoveMocktailEnvironment(GAppLaunchContext* context);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_HOST_LAUNCH_ENVIRONMENT_H_
