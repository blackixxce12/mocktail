#ifndef MOCKTAIL_LAUNCHER_UI_STYLE_H_
#define MOCKTAIL_LAUNCHER_UI_STYLE_H_

namespace mocktail::launcher_ui {

// Dark Adwaita (FORCE_DARK, like the progress window that follows Play)
// plus the few classes the window adds: launch-bar, play-button,
// env-badge, recommended-badge, launcher-warning, search-highlight,
// hint-popover, combo-option. Colors come from libadwaita's variables, so
// accent and high-contrast settings still apply.
void ApplyLauncherStyle();

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_STYLE_H_
