#ifndef MOCKTAIL_LAUNCHER_UI_PAGE_DIALOGS_H_
#define MOCKTAIL_LAUNCHER_UI_PAGE_DIALOGS_H_

namespace mocktail::launcher_ui {

class LauncherContext;

// The dialogs the Integrations, Advanced and About pages open from a row,
// for the self-test to render. Each presents itself on the context's window.

// Integrations › Discord › Customize texts (integrations_page.cc).
void OpenDiscordTextsDialog(LauncherContext* context);
// Advanced › Fast Flags › Fast Flags editor (advanced_fast_flags.cc).
void OpenFastFlagsEditor(LauncherContext* context);
// About › License & credits (about_page.cc).
void OpenAboutDialog(LauncherContext* context);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_PAGE_DIALOGS_H_
