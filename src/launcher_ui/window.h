#ifndef MOCKTAIL_LAUNCHER_UI_WINDOW_H_
#define MOCKTAIL_LAUNCHER_UI_WINDOW_H_

#include <adwaita.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

struct WindowOptions {
  int width = 980;
  int height = 700;
};

// The application window (research/ux.md 4.1-4.2): an AdwNavigationSplitView
// with an AdwSidebar of the sections and the pages in an AdwViewStack, the
// content header with the page title and the banner, and the outer launch
// bar (account chip, status, Save, Play). Collapses below 640sp.
class LauncherWindow final : public LauncherShell {
 public:
  LauncherWindow(AdwApplication* application, LauncherContext* context,
                 WindowOptions options);
  ~LauncherWindow() override;
  LauncherWindow(const LauncherWindow&) = delete;
  LauncherWindow& operator=(const LauncherWindow&) = delete;

  void Present();
  GtkWidget* widget() const { return window_; }
  // The window content (what the self-test renders).
  GtkWidget* content() const { return content_; }
  GtkWidget* page(Section section) const;
  Section current_section() const { return current_; }
  bool collapsed() const;
  // Runs once the window has painted its first frame.
  void OnReady(std::function<void()> callback);
  // Adds a page that is not in the sidebar (the self-test's binding page).
  void AddHiddenPage(const char* name, GtkWidget* page);
  void ShowHiddenPage(const char* name);
  // Search UI, for the self-test.
  void Search(const std::string& query);
  int search_result_count() const { return search_result_count_; }

  // LauncherShell
  GtkWindow* gtk_window() override { return GTK_WINDOW(window_); }
  void PresentToast(AdwToast* toast) override;
  void Refresh() override;
  void ShowSection(Section section) override;
  void Reveal(GtkWidget* widget) override;
  bool narrow() const override { return narrow_; }
  void Finish() override;

 private:
  GtkWidget* BuildSidebar();
  GtkWidget* BuildContent();
  GtkWidget* BuildLaunchBar();
  void InstallActions(AdwApplication* application);
  void InstallBreakpoint();
  void FitSidebarWidth();
  void UpdateMonitor();
  void ShowSearchResults(const std::string& query);
  void LeaveSearch();
  void ShowShortcuts();
  void ConfirmResetAll();

  static void OnSidebarActivated(AdwSidebar* sidebar, guint index,
                                 gpointer data);
  static gboolean OnCloseRequest(GtkWindow* window, gpointer data);
  static void OnMap(GtkWidget* widget, gpointer data);
  static void OnFirstPaint(GdkFrameClock* clock, gpointer data);
  static void OnBannerButton(AdwBanner* banner, gpointer data);
  static void OnSearchChanged(GtkSearchEntry* entry, gpointer data);

  LauncherContext* context_;
  GtkWidget* window_ = nullptr;
  // The outer toolbar view: the split view and the launch bar under it.
  GtkWidget* content_ = nullptr;
  // Around the split view only, so toasts float above the launch bar.
  GtkWidget* toast_overlay_ = nullptr;
  GtkWidget* split_view_ = nullptr;
  GtkWidget* sidebar_ = nullptr;
  GtkWidget* content_page_ = nullptr;
  GtkWidget* window_title_ = nullptr;
  GtkWidget* banner_ = nullptr;
  GtkWidget* stack_ = nullptr;
  GtkWidget* search_bar_ = nullptr;
  GtkWidget* search_entry_ = nullptr;
  GtkWidget* search_page_ = nullptr;
  GtkWidget* search_group_ = nullptr;
  GtkWidget* status_label_ = nullptr;
  GtkWidget* save_button_ = nullptr;
  GtkWidget* play_button_ = nullptr;
  GtkWidget* warning_probe_ = nullptr;
  std::array<GtkWidget*, kSectionCount> pages_{};
  std::array<guint, kSectionCount> sidebar_index_{};
  std::vector<std::string> sidebar_titles_;
  Section current_ = Section::kGraphics;
  bool narrow_ = false;
  bool searching_ = false;
  bool finished_ = false;
  bool ready_ = false;
  int search_result_count_ = 0;
  std::vector<std::function<void()>> ready_callbacks_;
  GdkSurface* surface_ = nullptr;
  GdkMonitor* monitor_ = nullptr;
};

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_WINDOW_H_
