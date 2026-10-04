#ifndef MOCKTAIL_LAUNCHER_UI_ADVANCED_FAST_FLAGS_H_
#define MOCKTAIL_LAUNCHER_UI_ADVANCED_FAST_FLAGS_H_

#include <adwaita.h>

#include "launcher_ui/launcher_context.h"

// Advanced › Fast Flags: <config_root>/fflags.json edited through
// launcher::FastFlagsDocument. The flags are staged like the settings and
// written by Save (a DirtySource); a flag that collides with a value
// Mocktail sets itself, so that Roblox would not start, blocks Save and Play
// until it is removed or the setting changes.
namespace mocktail::launcher_ui {

// The "Fast Flags editor" row; the first call loads fflags.json and
// registers it with the context's Save, Discard and Reload.
GtkWidget* BuildFastFlagsRow(LauncherContext* context);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_ADVANCED_FAST_FLAGS_H_
