#ifndef MOCKTAIL_LAUNCHER_UI_PAGES_H_
#define MOCKTAIL_LAUNCHER_UI_PAGES_H_

#include <gtk/gtk.h>

#include <array>

// The sections of the settings window and the function that builds each
// page. Every page lives in its own file (graphics_page.cc, ...); the
// window calls the builders once, in this order, after setting the current
// section on the context so the rows they bind are indexed for search under
// it. A builder returns an AdwPreferencesPage made with NewPage()
// (bindings.h) and never keeps widgets of another page.
namespace mocktail::launcher_ui {

class LauncherContext;

enum class Section {
  kGraphics,
  kDisplay,
  kPerformance,
  kAudio,
  kAccounts,
  kIntegrations,
  kNetworkUpdates,
  kAdvanced,
  kAbout,
};

inline constexpr std::size_t kSectionCount = 9;

struct SectionInfo {
  Section section;
  // Stack page name and search id: "graphics", "display", ...
  const char* id;
  // N_() title, translated where shown.
  const char* title;
  // Symbolic icon from the Adwaita theme.
  const char* icon;
  // 0: game settings, 1: Mocktail, 2: footer (About).
  int sidebar_group;
  GtkWidget* (*build)(LauncherContext* context);
};

const std::array<SectionInfo, kSectionCount>& Sections();
const SectionInfo& GetSectionInfo(Section section);
// nullptr for an unknown id.
const SectionInfo* FindSection(const char* id);

GtkWidget* BuildGraphicsPage(LauncherContext* context);
GtkWidget* BuildDisplayPage(LauncherContext* context);
GtkWidget* BuildPerformancePage(LauncherContext* context);
GtkWidget* BuildAudioPage(LauncherContext* context);
GtkWidget* BuildAccountsPage(LauncherContext* context);
GtkWidget* BuildIntegrationsPage(LauncherContext* context);
GtkWidget* BuildNetworkUpdatesPage(LauncherContext* context);
GtkWidget* BuildAdvancedPage(LauncherContext* context);
GtkWidget* BuildAboutPage(LauncherContext* context);

// The account chip at the left of the launch bar (accounts_page.cc): a
// GtkMenuButton with the selected account's avatar and names and a popover
// to switch accounts, play as guest, add or manage accounts. The window
// calls it once, after BuildAccountsPage(). It follows
// LauncherContext::OnLayoutChanged to drop its names when the window is
// narrow.
GtkWidget* BuildAccountChip(LauncherContext* context);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_PAGES_H_
