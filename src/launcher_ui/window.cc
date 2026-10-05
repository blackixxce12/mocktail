#include "launcher_ui/window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "runtime/runtime_config_bootstrap.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kSearchPageName[] = "search";
constexpr guint kHighlightMilliseconds = 1600;

std::string ColorToHex(const GdkRGBA& color) {
  const auto channel = [](float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255));
  };
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", channel(color.red),
                channel(color.green), channel(color.blue));
  return buffer;
}

void SetAccessibleLabel(GtkWidget* widget, const char* label) {
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

}  // namespace

LauncherWindow::LauncherWindow(AdwApplication* application,
                               LauncherContext* context, WindowOptions options)
    : context_(context) {
  window_ = adw_application_window_new(GTK_APPLICATION(application));
  g_object_add_weak_pointer(G_OBJECT(window_),
                            reinterpret_cast<gpointer*>(&window_));
  gtk_window_set_title(GTK_WINDOW(window_), "Mocktail");
  gtk_window_set_icon_name(GTK_WINDOW(window_), "space.bigrat.mocktail");
  gtk_window_set_default_size(GTK_WINDOW(window_), options.width,
                              options.height);
  // Layout works down to 360x480 (SPEC 5a); never min == max.
  gtk_widget_set_size_request(window_, 360, 480);

  context_->AttachShell(this);

  GtkWidget* sidebar_page =
      GTK_WIDGET(adw_navigation_page_new(BuildSidebar(), "Mocktail"));
  adw_navigation_page_set_tag(ADW_NAVIGATION_PAGE(sidebar_page), "sidebar");
  content_page_ = GTK_WIDGET(adw_navigation_page_new(BuildContent(), ""));
  adw_navigation_page_set_tag(ADW_NAVIGATION_PAGE(content_page_), "content");
  split_view_ = adw_navigation_split_view_new();
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split_view_),
                                        ADW_NAVIGATION_PAGE(sidebar_page));
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split_view_),
                                        ADW_NAVIGATION_PAGE(content_page_));
  adw_navigation_split_view_set_min_sidebar_width(
      ADW_NAVIGATION_SPLIT_VIEW(split_view_), 200);
  adw_navigation_split_view_set_max_sidebar_width(
      ADW_NAVIGATION_SPLIT_VIEW(split_view_), 260);
  // Collapsed, the window still opens on a page, not on the section list.
  adw_navigation_split_view_set_show_content(
      ADW_NAVIGATION_SPLIT_VIEW(split_view_), TRUE);

  // Toasts sit over the pages, just above the launch bar: around the whole
  // window they covered the account chip, the status and Save, and in a
  // narrow window Play itself.
  toast_overlay_ = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), split_view_);
  content_ = adw_toolbar_view_new();
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_), toast_overlay_);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(content_), BuildLaunchBar());
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(content_),
                                        ADW_TOOLBAR_RAISED_BORDER);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), content_);

  InstallBreakpoint();
  InstallActions(application);
  g_signal_connect(window_, "close-request", G_CALLBACK(OnCloseRequest), this);
  g_signal_connect(window_, "map", G_CALLBACK(OnMap), this);
  ShowSection(Section::kGraphics);
  Refresh();
}

LauncherWindow::~LauncherWindow() {
  context_->AttachShell(nullptr);
  if (monitor_ != nullptr) {
    g_signal_handlers_disconnect_by_data(monitor_, this);
    g_object_unref(monitor_);
  }
  if (window_ != nullptr) {
    g_signal_handlers_disconnect_by_data(window_, this);
    if (surface_ != nullptr) {
      g_signal_handlers_disconnect_by_data(surface_, this);
    }
    // The rows' bindings talk to the context, which outlives this window.
    gtk_window_destroy(GTK_WINDOW(window_));
  }
}

void LauncherWindow::Present() { gtk_window_present(GTK_WINDOW(window_)); }

// ---- sidebar
// ---------------------------------------------------------------------

GtkWidget* LauncherWindow::BuildSidebar() {
  sidebar_ = adw_sidebar_new();
  AdwSidebarSection* groups[3] = {adw_sidebar_section_new(),
                                  adw_sidebar_section_new(),
                                  adw_sidebar_section_new()};
  adw_sidebar_section_set_title(groups[1], "Mocktail");
  guint index = 0;
  for (int group = 0; group < 3; ++group) {
    for (const SectionInfo& info : Sections()) {
      if (info.sidebar_group != group) continue;
      AdwSidebarItem* item = adw_sidebar_item_new(_(info.title));
      adw_sidebar_item_set_icon_name(item, info.icon);
      adw_sidebar_section_append(groups[group], item);
      sidebar_index_[static_cast<std::size_t>(info.section)] = index++;
    }
    adw_sidebar_append(ADW_SIDEBAR(sidebar_), groups[group]);
  }
  g_signal_connect(sidebar_, "activated", G_CALLBACK(OnSidebarActivated), this);

  GtkWidget* header = adw_header_bar_new();
  GtkWidget* search_button = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(search_button), "system-search-symbolic");
  gtk_widget_set_tooltip_text(search_button, _("Search Settings (Ctrl+F)"));
  SetAccessibleLabel(search_button, _("Search settings"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), search_button);

  GMenu* menu = g_menu_new();
  GMenu* files = g_menu_new();
  g_menu_append(files, _("Open config.yaml"), "win.open-config");
  g_menu_append(files, _("Open Logs Folder"), "win.open-logs");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(files));
  g_object_unref(files);
  GMenu* help = g_menu_new();
  g_menu_append(help, _("Keyboard Shortcuts"), "win.show-shortcuts");
  g_menu_append(help, _("About Mocktail"), "win.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(help));
  g_object_unref(help);
  GMenu* reset = g_menu_new();
  g_menu_append(reset, _("Reset All Settings…"), "win.reset-all");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(reset));
  g_object_unref(reset);
  GtkWidget* menu_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                 G_MENU_MODEL(menu));
  gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
  gtk_widget_set_tooltip_text(menu_button, _("Main Menu"));
  SetAccessibleLabel(menu_button, _("Main menu"));
  g_object_unref(menu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

  search_entry_ = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry_),
                                        _("Search settings"));
  gtk_widget_set_hexpand(search_entry_, TRUE);
  g_signal_connect(search_entry_, "search-changed", G_CALLBACK(OnSearchChanged),
                   this);
  g_signal_connect(search_entry_, "activate",
                   G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                     auto* self = static_cast<LauncherWindow*>(data);
                     // Enter opens the first result.
                     GtkWidget* row = static_cast<GtkWidget*>(g_object_get_data(
                         G_OBJECT(self->search_page_), "first-result"));
                     if (row != nullptr) gtk_widget_activate(row);
                   }),
                   this);
  search_bar_ = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(search_bar_), search_entry_);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(search_bar_),
                               GTK_EDITABLE(search_entry_));
  g_object_bind_property(
      search_button, "active", search_bar_, "search-mode-enabled",
      GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
  g_signal_connect(search_bar_, "notify::search-mode-enabled",
                   G_CALLBACK(+[](GObject* bar, GParamSpec*, gpointer data) {
                     if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar))) {
                       static_cast<LauncherWindow*>(data)->LeaveSearch();
                     }
                   }),
                   this);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), search_bar_);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), sidebar_);
  return toolbar;
}

void LauncherWindow::OnSidebarActivated(AdwSidebar*, guint index,
                                        gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  for (const SectionInfo& info : Sections()) {
    if (self->sidebar_index_[static_cast<std::size_t>(info.section)] == index) {
      if (self->searching_) {
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar_),
                                       FALSE);
      }
      self->ShowSection(info.section);
      adw_navigation_split_view_set_show_content(
          ADW_NAVIGATION_SPLIT_VIEW(self->split_view_), TRUE);
      return;
    }
  }
}

// ---- content
// ---------------------------------------------------------------------

GtkWidget* LauncherWindow::BuildContent() {
  stack_ = adw_view_stack_new();
  for (const SectionInfo& info : Sections()) {
    context_->BeginSection(info.section);
    GtkWidget* page = info.build(context_);
    pages_[static_cast<std::size_t>(info.section)] = page;
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack_), page, info.id,
                                        _(info.title), info.icon);
  }
  search_page_ = adw_preferences_page_new();
  adw_view_stack_add_named(ADW_VIEW_STACK(stack_), search_page_,
                           kSearchPageName);

  banner_ = adw_banner_new("");
  adw_banner_set_use_markup(ADW_BANNER(banner_), FALSE);
  g_signal_connect(banner_, "button-clicked", G_CALLBACK(OnBannerButton), this);

  window_title_ = adw_window_title_new("", "");
  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), window_title_);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), banner_);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack_);
  return toolbar;
}

GtkWidget* LauncherWindow::page(Section section) const {
  return pages_[static_cast<std::size_t>(section)];
}

void LauncherWindow::AddHiddenPage(const char* name, GtkWidget* page) {
  adw_view_stack_add_named(ADW_VIEW_STACK(stack_), page, name);
}

void LauncherWindow::ShowHiddenPage(const char* name) {
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(stack_), name);
}

void LauncherWindow::ShowSection(Section section) {
  current_ = section;
  const SectionInfo& info = GetSectionInfo(section);
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(stack_), info.id);
  adw_window_title_set_title(ADW_WINDOW_TITLE(window_title_), _(info.title));
  adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(content_page_),
                                _(info.title));
  const guint index = sidebar_index_[static_cast<std::size_t>(section)];
  if (adw_sidebar_get_selected(ADW_SIDEBAR(sidebar_)) != index) {
    adw_sidebar_set_selected(ADW_SIDEBAR(sidebar_), index);
  }
}

bool LauncherWindow::collapsed() const {
  return adw_navigation_split_view_get_collapsed(
      ADW_NAVIGATION_SPLIT_VIEW(split_view_));
}

void LauncherWindow::Reveal(GtkWidget* widget) {
  if (widget == nullptr) return;
  // Find the page that holds the widget.
  for (const SectionInfo& info : Sections()) {
    GtkWidget* holder = pages_[static_cast<std::size_t>(info.section)];
    if (holder != nullptr && gtk_widget_is_ancestor(widget, holder)) {
      if (searching_) {
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(search_bar_), FALSE);
      }
      ShowSection(info.section);
      adw_navigation_split_view_set_show_content(
          ADW_NAVIGATION_SPLIT_VIEW(split_view_), TRUE);
      break;
    }
  }
  // A row inside a collapsed expander row (advanced options) cannot take
  // focus until the expander is open.
  for (GtkWidget* parent = gtk_widget_get_parent(widget); parent != nullptr;
       parent = gtk_widget_get_parent(parent)) {
    if (ADW_IS_EXPANDER_ROW(parent)) {
      adw_expander_row_set_expanded(ADW_EXPANDER_ROW(parent), TRUE);
    }
  }
  // Focusing scrolls the page's viewport to the row.
  gtk_widget_grab_focus(widget);
  gtk_widget_add_css_class(widget, "search-highlight");
  g_timeout_add(
      kHighlightMilliseconds,
      [](gpointer data) -> gboolean {
        gtk_widget_remove_css_class(GTK_WIDGET(data), "search-highlight");
        g_object_unref(data);
        return G_SOURCE_REMOVE;
      },
      g_object_ref(widget));
}

// ---- search
// ------------------------------------------------------------------------

void LauncherWindow::OnSearchChanged(GtkSearchEntry* entry, gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  const std::string query = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (query.empty()) {
    if (self->searching_) self->LeaveSearch();
    return;
  }
  self->ShowSearchResults(query);
}

void LauncherWindow::Search(const std::string& query) {
  if (query.empty()) {
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(search_bar_), FALSE);
    LeaveSearch();
    return;
  }
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(search_bar_), TRUE);
  gtk_editable_set_text(GTK_EDITABLE(search_entry_), query.c_str());
  ShowSearchResults(query);
}

void LauncherWindow::ShowSearchResults(const std::string& query) {
  searching_ = true;
  if (search_group_ != nullptr) {
    adw_preferences_page_remove(ADW_PREFERENCES_PAGE(search_page_),
                                ADW_PREFERENCES_GROUP(search_group_));
  }
  g_object_set_data(G_OBJECT(search_page_), "first-result", nullptr);
  search_group_ = adw_preferences_group_new();
  const std::vector<int> matches = context_->search().Match(query);
  search_result_count_ = 0;
  for (const int id : matches) {
    const SearchEntry* entry = context_->search().Get(id);
    const RowRecord* record = nullptr;
    for (const RowRecord& candidate : context_->rows()) {
      if (candidate.search_id == id) record = &candidate;
    }
    if (entry == nullptr || record == nullptr || record->row == nullptr) {
      continue;
    }
    GtkWidget* result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(result),
                                  Markup(entry->title).c_str());
    // The page that holds the row names the section, wherever the row was
    // registered from.
    Section section = record->section;
    for (const SectionInfo& info : Sections()) {
      GtkWidget* holder = pages_[static_cast<std::size_t>(info.section)];
      if (holder != nullptr && gtk_widget_is_ancestor(record->row, holder)) {
        section = info.section;
      }
    }
    std::string subtitle = _(GetSectionInfo(section).title);
    if (!entry->subtitle.empty()) subtitle += " · " + entry->subtitle;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(result),
                                Markup(subtitle).c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(result), 2);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(result), TRUE);
    adw_action_row_add_suffix(ADW_ACTION_ROW(result),
                              gtk_image_new_from_icon_name("go-next-symbolic"));
    g_object_set_data(G_OBJECT(result), "target", record->row);
    g_signal_connect(result, "activated",
                     G_CALLBACK(+[](AdwActionRow* row, gpointer data) {
                       auto* self = static_cast<LauncherWindow*>(data);
                       auto* target = static_cast<GtkWidget*>(
                           g_object_get_data(G_OBJECT(row), "target"));
                       self->Reveal(target);
                     }),
                     this);
    if (search_result_count_ == 0) {
      g_object_set_data(G_OBJECT(search_page_), "first-result", result);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(search_group_), result);
    ++search_result_count_;
  }
  if (search_result_count_ == 0) {
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(search_group_),
                                    Markup(_("No matching settings")).c_str());
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(search_group_),
        Markup(_("Try another word, a config.yaml key or a variable name such "
                 "as SDL_VIDEODRIVER"))
            .c_str());
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(search_page_),
                           ADW_PREFERENCES_GROUP(search_group_));
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(stack_),
                                        kSearchPageName);
  adw_window_title_set_title(ADW_WINDOW_TITLE(window_title_),
                             _("Search Results"));
  adw_navigation_split_view_set_show_content(
      ADW_NAVIGATION_SPLIT_VIEW(split_view_), TRUE);
}

void LauncherWindow::LeaveSearch() {
  if (!searching_) return;
  searching_ = false;
  gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
  ShowSection(current_);
}

// ---- launch bar
// --------------------------------------------------------------------

GtkWidget* LauncherWindow::BuildLaunchBar() {
  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(bar, "launch-bar");

  GtkWidget* chip = BuildAccountChip(context_);
  gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(bar), chip);

  status_label_ = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0F);
  gtk_label_set_ellipsize(GTK_LABEL(status_label_), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(status_label_, TRUE);
  gtk_widget_add_css_class(status_label_, "dim-label");
  gtk_widget_add_css_class(status_label_, "launch-status");
  gtk_box_append(GTK_BOX(bar), status_label_);

  save_button_ = gtk_button_new_with_mnemonic(_("_Save"));
  gtk_widget_add_css_class(save_button_, "flat");
  gtk_widget_set_valign(save_button_, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(save_button_), "win.save");
  gtk_widget_set_tooltip_text(save_button_, _("Save changes (Ctrl+S)"));
  gtk_box_append(GTK_BOX(bar), save_button_);

  play_button_ = gtk_button_new();
  GtkWidget* play_content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(play_content),
                                   "media-playback-start-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(play_content), _("_Play"));
  adw_button_content_set_use_underline(ADW_BUTTON_CONTENT(play_content), TRUE);
  gtk_button_set_child(GTK_BUTTON(play_button_), play_content);
  gtk_widget_add_css_class(play_button_, "pill");
  gtk_widget_add_css_class(play_button_, "suggested-action");
  gtk_widget_add_css_class(play_button_, "play-button");
  gtk_widget_set_valign(play_button_, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(play_button_), "win.play");
  gtk_box_append(GTK_BOX(bar), play_button_);

  // Never shown: gives the theme's warning color for inline warnings.
  warning_probe_ = gtk_label_new("");
  gtk_widget_add_css_class(warning_probe_, "warning");
  gtk_widget_set_visible(warning_probe_, FALSE);
  gtk_box_append(GTK_BOX(bar), warning_probe_);
  return bar;
}

void LauncherWindow::Refresh() {
  // Pages are built before the banner and the launch bar exist, and may
  // already report (AddDirtySource); the constructor refreshes at its end.
  if (window_ == nullptr || finished_ || play_button_ == nullptr) return;
  BannerKind kind = BannerKind::kConfigError;
  const Banner* banner = context_->TopBanner(&kind);
  if (banner != nullptr) {
    adw_banner_set_title(ADW_BANNER(banner_), banner->title.c_str());
    adw_banner_set_button_label(
        ADW_BANNER(banner_),
        banner->button.empty() ? nullptr : banner->button.c_str());
    adw_banner_set_button_style(ADW_BANNER(banner_),
                                kind == BannerKind::kConfigError
                                    ? ADW_BANNER_BUTTON_SUGGESTED
                                    : ADW_BANNER_BUTTON_DEFAULT);
  }
  adw_banner_set_revealed(ADW_BANNER(banner_), banner != nullptr);

  const std::string status = context_->StatusText();
  gtk_label_set_text(GTK_LABEL(status_label_), status.c_str());
  gtk_widget_set_tooltip_text(status_label_, status.c_str());
  const int unsaved = context_->unsaved_count();
  gtk_widget_set_visible(save_button_, unsaved > 0);
  GAction* save = g_action_map_lookup_action(G_ACTION_MAP(window_), "save");
  if (save != nullptr) {
    g_simple_action_set_enabled(G_SIMPLE_ACTION(save), context_->can_save());
  }
  GAction* play = g_action_map_lookup_action(G_ACTION_MAP(window_), "play");
  const std::string blocker = context_->play_blocker();
  if (play != nullptr) {
    g_simple_action_set_enabled(G_SIMPLE_ACTION(play), blocker.empty());
  }
  gtk_widget_set_tooltip_text(
      play_button_, blocker.empty()
                        ? (unsaved > 0 ? _("Save and start Roblox (Ctrl+Enter)")
                                       : _("Start Roblox (Ctrl+Enter)"))
                        : blocker.c_str());
}

void LauncherWindow::OnBannerButton(AdwBanner*, gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  const Banner* banner = self->context_->TopBanner();
  if (banner != nullptr && banner->on_button) {
    // Copy: the action may replace the banner.
    const std::function<void()> action = banner->on_button;
    action();
  }
}

void LauncherWindow::PresentToast(AdwToast* toast) {
  if (finished_ || toast_overlay_ == nullptr) {
    g_object_unref(toast);
    return;
  }
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), toast);
}

// ---- breakpoint, actions, shortcuts
// -------------------------------------------------

void LauncherWindow::InstallBreakpoint() {
  AdwBreakpoint* breakpoint =
      adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 640sp"));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(split_view_), "collapsed",
                            &collapsed);
  g_value_unset(&collapsed);
  GValue mode = G_VALUE_INIT;
  g_value_init(&mode, ADW_TYPE_SIDEBAR_MODE);
  g_value_set_enum(&mode, ADW_SIDEBAR_MODE_PAGE);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(sidebar_), "mode", &mode);
  g_value_unset(&mode);
  g_signal_connect(breakpoint, "apply",
                   G_CALLBACK(+[](AdwBreakpoint*, gpointer data) {
                     auto* self = static_cast<LauncherWindow*>(data);
                     self->narrow_ = true;
                     self->context_->SetNarrow(true);
                     self->Refresh();
                   }),
                   this);
  g_signal_connect(breakpoint, "unapply",
                   G_CALLBACK(+[](AdwBreakpoint*, gpointer data) {
                     auto* self = static_cast<LauncherWindow*>(data);
                     self->narrow_ = false;
                     self->context_->SetNarrow(false);
                     self->Refresh();
                   }),
                   this);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window_),
                                        breakpoint);
}

void LauncherWindow::InstallActions(AdwApplication* application) {
  static const GActionEntry kEntries[] = {
      {"save",
       [](GSimpleAction*, GVariant*, gpointer data) {
         static_cast<LauncherWindow*>(data)->context_->Save();
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"play",
       [](GSimpleAction*, GVariant*, gpointer data) {
         static_cast<LauncherWindow*>(data)->context_->Play();
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"search",
       [](GSimpleAction*, GVariant*, gpointer data) {
         auto* self = static_cast<LauncherWindow*>(data);
         gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar_),
                                        TRUE);
         gtk_widget_grab_focus(self->search_entry_);
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"quit",
       [](GSimpleAction*, GVariant*, gpointer data) {
         static_cast<LauncherWindow*>(data)->context_->RequestClose();
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"open-config",
       [](GSimpleAction*, GVariant*, gpointer data) {
         auto* self = static_cast<LauncherWindow*>(data);
         self->context_->OpenPath(self->context_->config_file());
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"open-logs",
       [](GSimpleAction*, GVariant*, gpointer data) {
         auto* self = static_cast<LauncherWindow*>(data);
         std::error_code error;
         std::filesystem::create_directories(
             self->context_->paths().logs_root(), error);
         self->context_->OpenPath(self->context_->paths().logs_root());
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"show-shortcuts",
       [](GSimpleAction*, GVariant*, gpointer data) {
         static_cast<LauncherWindow*>(data)->ShowShortcuts();
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"about",
       [](GSimpleAction*, GVariant*, gpointer data) {
         auto* self = static_cast<LauncherWindow*>(data);
         self->ShowSection(Section::kAbout);
         adw_navigation_split_view_set_show_content(
             ADW_NAVIGATION_SPLIT_VIEW(self->split_view_), TRUE);
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
      {"reset-all",
       [](GSimpleAction*, GVariant*, gpointer data) {
         static_cast<LauncherWindow*>(data)->ConfirmResetAll();
       },
       nullptr,
       nullptr,
       nullptr,
       {}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window_), kEntries,
                                  G_N_ELEMENTS(kEntries), this);
  struct Accelerator {
    const char* action;
    const char* keys[3];
  };
  static const Accelerator kAccelerators[] = {
      {"win.save", {"<Control>s", nullptr, nullptr}},
      {"win.search", {"<Control>f", nullptr, nullptr}},
      {"win.play", {"<Control>Return", "<Control>KP_Enter", nullptr}},
      {"win.quit", {"<Control>q", "<Control>w", nullptr}},
      {"win.show-shortcuts", {"<Control>question", nullptr, nullptr}},
  };
  for (const Accelerator& accelerator : kAccelerators) {
    gtk_application_set_accels_for_action(GTK_APPLICATION(application),
                                          accelerator.action, accelerator.keys);
  }
}

void LauncherWindow::ShowShortcuts() {
  AdwDialog* dialog = adw_shortcuts_dialog_new();
  AdwShortcutsSection* section = adw_shortcuts_section_new(nullptr);
  adw_shortcuts_section_add(
      section, adw_shortcuts_item_new(_("Play"), "<Control>Return"));
  adw_shortcuts_section_add(section,
                            adw_shortcuts_item_new(_("Save"), "<Control>s"));
  adw_shortcuts_section_add(
      section, adw_shortcuts_item_new(_("Search settings"), "<Control>f"));
  adw_shortcuts_section_add(
      section,
      adw_shortcuts_item_new(_("Close without playing"), "<Control>q"));
  adw_shortcuts_section_add(
      section,
      adw_shortcuts_item_new(_("Keyboard shortcuts"), "<Control>question"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), section);
  adw_dialog_present(dialog, window_);
}

void LauncherWindow::ConfirmResetAll() {
  if (context_->read_only()) {
    context_->Toast(_("Fix config.yaml before resetting settings"));
    return;
  }
  AdwDialog* dialog = adw_alert_dialog_new(
      _("Reset all settings?"),
      _("config.yaml is replaced by Mocktail's defaults, including your "
        "comments in it, when you save. A copy of the file as it was before "
        "the settings window first saved it is kept as "
        "config.yaml.launcher-backup."));
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "reset",
                                 _("_Reset"), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "reset",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_signal_connect(
      dialog, "response::reset",
      G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
        auto* self = static_cast<LauncherWindow*>(data);
        std::string error;
        if (!self->context_->draft().ReplaceAll(
                std::string(runtime::DefaultRuntimeConfigYaml()), &error)) {
          self->context_->Toast(error);
          return;
        }
        self->context_->NotifySettingChanged("");
        LauncherContext* context = self->context_;
        context->Toast(_("All settings reset; save to keep them"), _("Undo"),
                       [context] { context->Discard(); });
      }),
      this);
  adw_dialog_present(dialog, window_);
}

// ---- lifecycle
// ---------------------------------------------------------------------

gboolean LauncherWindow::OnCloseRequest(GtkWindow*, gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  if (self->finished_) return FALSE;
  if (!self->context_->HandleCloseRequest()) return TRUE;
  self->finished_ = true;
  self->context_->AttachShell(nullptr);
  return FALSE;
}

void LauncherWindow::Finish() {
  if (finished_ || window_ == nullptr) return;
  finished_ = true;
  context_->AttachShell(nullptr);
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto** window = static_cast<GtkWidget**>(data);
        if (*window != nullptr) gtk_window_destroy(GTK_WINDOW(*window));
        return G_SOURCE_REMOVE;
      },
      &window_);
}

void LauncherWindow::OnReady(std::function<void()> callback) {
  if (ready_) {
    callback();
    return;
  }
  ready_callbacks_.push_back(std::move(callback));
}

void LauncherWindow::OnMap(GtkWidget* widget, gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  self->surface_ = gtk_native_get_surface(GTK_NATIVE(widget));
  if (self->surface_ != nullptr) {
    g_signal_connect(self->surface_, "enter-monitor",
                     G_CALLBACK(+[](GdkSurface*, GdkMonitor*, gpointer data) {
                       static_cast<LauncherWindow*>(data)->UpdateMonitor();
                     }),
                     self);
    g_signal_connect(self->surface_, "notify::scale",
                     G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                       static_cast<LauncherWindow*>(data)->UpdateMonitor();
                     }),
                     self);
  }
  self->UpdateMonitor();
  GdkRGBA warning = {};
  gtk_widget_get_color(self->warning_probe_, &warning);
  if (warning.alpha > 0.0F) {
    self->context_->SetWarningColor(ColorToHex(warning));
    RefreshRows(self->context_);
  }
  GdkFrameClock* clock = gtk_widget_get_frame_clock(widget);
  if (clock != nullptr) {
    g_signal_connect(clock, "after-paint", G_CALLBACK(OnFirstPaint), self);
  }
}

void LauncherWindow::OnFirstPaint(GdkFrameClock* clock, gpointer data) {
  auto* self = static_cast<LauncherWindow*>(data);
  g_signal_handlers_disconnect_by_func(clock, (gpointer)OnFirstPaint, data);
  if (self->ready_) return;
  self->ready_ = true;
  std::fputs("mocktail-launcher-ui ready\n", stdout);
  std::fflush(stdout);
  std::vector<std::function<void()>> callbacks;
  callbacks.swap(self->ready_callbacks_);
  for (const auto& callback : callbacks) callback();
}

void LauncherWindow::UpdateMonitor() {
  if (window_ == nullptr) return;
  GdkDisplay* display = gtk_widget_get_display(window_);
  GdkMonitor* monitor = nullptr;
  if (surface_ != nullptr) {
    monitor = gdk_display_get_monitor_at_surface(display, surface_);
  }
  if (monitor == nullptr) {
    // On Wayland there is no primary monitor; before the surface entered
    // one, the first is the best guess.
    GListModel* monitors = gdk_display_get_monitors(display);
    if (g_list_model_get_n_items(monitors) > 0) {
      monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
      g_object_unref(monitor);  // the list keeps it alive
    }
  }
  if (monitor != monitor_) {
    if (monitor_ != nullptr) {
      g_signal_handlers_disconnect_by_data(monitor_, this);
      g_object_unref(monitor_);
    }
    monitor_ =
        monitor != nullptr ? GDK_MONITOR(g_object_ref(monitor)) : nullptr;
    if (monitor_ != nullptr) {
      g_signal_connect_swapped(
          monitor_, "notify", G_CALLBACK(+[](gpointer data) {
            static_cast<LauncherWindow*>(data)->UpdateMonitor();
          }),
          this);
    }
  }
  MonitorInfo info;
  if (monitor_ != nullptr) {
    GdkRectangle geometry = {};
    gdk_monitor_get_geometry(monitor_, &geometry);
    info.valid = geometry.width > 0 && geometry.height > 0;
    const char* connector = gdk_monitor_get_connector(monitor_);
    info.connector = connector != nullptr ? connector : "";
    const char* description = gdk_monitor_get_description(monitor_);
    info.description = description != nullptr ? description : "";
    info.width = geometry.width;
    info.height = geometry.height;
    info.scale = gdk_monitor_get_scale(monitor_);
    if (info.scale <= 0 && surface_ != nullptr) {
      info.scale = gdk_surface_get_scale(surface_);
    }
    if (info.scale <= 0) info.scale = 1.0;
    info.refresh_millihertz = gdk_monitor_get_refresh_rate(monitor_);
  }
  context_->SetMonitor(info);
}

}  // namespace mocktail::launcher_ui
