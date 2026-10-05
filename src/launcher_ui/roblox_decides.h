#ifndef MOCKTAIL_LAUNCHER_UI_ROBLOX_DECIDES_H_
#define MOCKTAIL_LAUNCHER_UI_ROBLOX_DECIDES_H_

#include <adwaita.h>

#include <functional>
#include <optional>
#include <string>

#include "launcher_ui/bindings.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/roblox_overrides.h"

// What Mocktail decides and what Roblox decides, in words: the "Overrides
// Roblox setting" badge of every row whose value replaces one of Roblox's
// own settings (Hint::overrides_roblox), and the overview group on the
// Advanced page with a link to it from the Performance page. The rules, and
// the runtime code each one follows, are in roblox_overrides.h; this file
// only reads the settings and says what they mean.
namespace mocktail::launcher_ui {

// The settings roblox_overrides.h works from, as the game will read them
// this launch: LauncherContext::GameValue (the draft, the template's
// defaults and the managed variables that override them), device.type of a
// detailed device: block, and whether direct Vulkan renders on Intel
// integrated graphics (graphics quality level 1 by default).
GameSettings CurrentGameSettings(const LauncherContext& context);

// Hint::overrides_roblox for a row bound to `key`: what the key's current
// value takes over from Roblox, nullopt when nothing.
std::function<std::optional<RobloxOverrideNote>(LauncherContext& context)>
RobloxOverrideHint(std::string key);

// A conflict (FindOverrideConflicts) as a warning sentence.
std::string DescribeOverrideConflict(OverrideConflict conflict,
                                     const GameSettings& settings);

// Adds the group "What Mocktail decides and what Roblox decides" to `page`
// (the Advanced page): the in-game settings the current settings take over,
// each opening the row that does it, conflicts between them, the settings
// left to Roblox, and the client settings Mocktail sets at every start.
void AddRobloxDecidesGroup(LauncherContext* context, GtkWidget* page);

// A row that opens that group, saying how many of Roblox's settings
// Mocktail decides now (the Performance page).
GtkWidget* BuildRobloxDecidesLinkRow(LauncherContext* context);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ROBLOX_DECIDES_H_
