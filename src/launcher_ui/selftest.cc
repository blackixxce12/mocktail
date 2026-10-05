#include "launcher_ui/selftest.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

#include "launcher/config_document.h"
#include "launcher/window_state_file.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/page_dialogs.h"
#include "launcher_ui/recommendations.h"
#include "launcher_ui/roblox_decides.h"
#include "launcher_ui/roblox_overrides.h"
#include "runtime/launcher_ui_launch.h"
#include "runtime/runtime_config_bootstrap.h"
#include "window/video_driver_policy.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kBindingPageName[] = "selftest-bindings";
constexpr guint kSettleMilliseconds = 450;
constexpr guint kResizeMilliseconds = 1800;
constexpr int kMachineWaitLimit = 150;  // x 100 ms

// The rows the self-test adds on its hidden page; they have no hints.
constexpr const char* kSelftestKeys[] = {"launcher.show_on_start",
                                         "performance.memory_limit_mb",
                                         "window.title", "appearance.theme"};

// Every icon the window and the pages use.
constexpr const char* kIcons[] = {
    "video-display-symbolic",
    "view-fullscreen-symbolic",
    "power-profile-performance-symbolic",
    "audio-speakers-symbolic",
    "avatar-default-symbolic",
    "application-x-addon-symbolic",
    "network-transmit-receive-symbolic",
    "preferences-other-symbolic",
    "help-about-symbolic",
    "system-search-symbolic",
    "open-menu-symbolic",
    "media-playback-start-symbolic",
    "dialog-information-symbolic",
    "dialog-warning-symbolic",
    "edit-undo-symbolic",
    "go-next-symbolic",
    "view-refresh-symbolic",
    "document-open-symbolic",
    "folder-open-symbolic",
    "edit-copy-symbolic",
    "document-edit-symbolic",
    "user-trash-symbolic",
    "list-add-symbolic",
    "view-more-symbolic",
    "input-gaming-symbolic",
};

std::string Quote(const std::string& text) {
  return nlohmann::json(text).dump(-1, ' ', false,
                                   nlohmann::json::error_handler_t::replace);
}

bool WriteFile(const std::filesystem::path& path, const std::string& bytes,
               std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
  if (!output) {
    *error = "cannot write " + path.string();
    return false;
  }
  return true;
}

// The first descendant of `widget` with the style class `css`.
GtkWidget* FindByClass(GtkWidget* widget, const char* css) {
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, css)) return child;
    if (GtkWidget* found = FindByClass(child, css)) return found;
  }
  return nullptr;
}

// The last action row inside `widget`, in tree order: an expander row's
// last child row.
GtkWidget* LastActionRow(GtkWidget* widget) {
  GtkWidget* last = nullptr;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (ADW_IS_ACTION_ROW(child)) last = child;
    if (GtkWidget* found = LastActionRow(child)) last = found;
  }
  return last;
}

guint FindModelPosition(GtkWidget* combo_row, const std::string& label) {
  GListModel* model = adw_combo_row_get_model(ADW_COMBO_ROW(combo_row));
  const guint count = g_list_model_get_n_items(model);
  for (guint position = 0; position < count; ++position) {
    GtkStringObject* item =
        GTK_STRING_OBJECT(g_list_model_get_item(model, position));
    const std::string text = gtk_string_object_get_string(item);
    g_object_unref(item);
    if (text == label) return position;
  }
  return GTK_INVALID_LIST_POSITION;
}

// The dialog behind a broken config.yaml's Fix… button.
void OpenConfigErrorDialog(LauncherContext* context) {
  BannerKind kind = BannerKind::kEnvironmentOverrides;
  const Banner* banner = context->TopBanner(&kind);
  if (banner != nullptr && kind == BannerKind::kConfigError &&
      banner->on_button) {
    // Copy: the action may replace the banner.
    const std::function<void()> open = banner->on_button;
    open();
  }
}

}  // namespace

bool PrepareSelftestEnvironment(const std::filesystem::path& out_dir,
                                std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::path home = out_dir / "home";
  for (const char* directory :
       {"config/mocktail", "data", "state/mocktail", "cache"}) {
    std::filesystem::create_directories(home / directory, filesystem_error);
    if (filesystem_error) {
      *error = "cannot create " + (home / directory).string() + ": " +
               filesystem_error.message();
      return false;
    }
  }
  // Every Mocktail path follows these; nothing may reach the real ones.
  for (const char* name :
       {"MOCKTAIL_CONFIG_ROOT", "MOCKTAIL_DATA_ROOT", "MOCKTAIL_STATE_ROOT",
        "MOCKTAIL_CACHE_ROOT", "MOCKTAIL_AUTH_ROOT", "MOCKTAIL_COOKIE_FILE",
        "MOCKTAIL_ROBLOX_COOKIES", "MOCKTAIL_LAUNCHER_RESULT_FD"}) {
    unsetenv(name);
  }
  setenv("XDG_CONFIG_HOME", (home / "config").c_str(), 1);
  setenv("XDG_DATA_HOME", (home / "data").c_str(), 1);
  setenv("XDG_STATE_HOME", (home / "state").c_str(), 1);
  setenv("XDG_CACHE_HOME", (home / "cache").c_str(), 1);
  const std::filesystem::path config = home / "config/mocktail/config.yaml";
  setenv(std::string(runtime::kLauncherUiConfigFileVariable).c_str(),
         config.c_str(), 1);
  // A fresh template each run, mode 0600 like the bootstrap writes it.
  std::filesystem::remove(config, filesystem_error);
  std::filesystem::remove(launcher::ConfigDocument::BackupPath(config),
                          filesystem_error);
  if (!WriteFile(config, std::string(runtime::DefaultRuntimeConfigYaml()),
                 error)) {
    return false;
  }
  std::filesystem::permissions(
      config,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      filesystem_error);
  // A remembered windowed size, so Save has to update it.
  if (!WriteFile(home / "state/mocktail/window-state.json",
                 "{\"schema_version\":1,\"fullscreen\":false,"
                 "\"maximized\":false,\"windowed\":{\"has_position\":false,"
                 "\"width\":1600,\"height\":900}}\n",
                 error)) {
    return false;
  }
  // One override, so the banner and the ENV badge are exercised.
  setenv("MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan", 1);
  setenv(std::string(runtime::kLauncherUiEnvOverridesVariable).c_str(),
         "MOCKTAIL_GRAPHICS_BACKEND", 1);
  setenv(std::string(runtime::kLauncherUiConfigCreatedVariable).c_str(), "1",
         1);
  return true;
}

Selftest::Selftest(AdwApplication* application, LauncherContext* context,
                   LauncherWindow* window, std::filesystem::path out_dir)
    : application_(application),
      context_(context),
      window_(window),
      out_dir_(std::move(out_dir)) {}

void Selftest::Start() {
  steps_.push_back([this] { return WaitForMachine(); });
  steps_.push_back([this] { return RecordWindow(); });
  for (const SectionInfo& info : Sections()) {
    const Section section = info.section;
    steps_.push_back([this, section] {
      window_->ShowSection(section);
      return kSettleMilliseconds;
    });
    steps_.push_back([this, section] { return RenderSection(section, ""); });
  }
  steps_.push_back([this] {
    window_->ShowSection(Section::kGraphics);
    return kSettleMilliseconds;
  });
  // The worked example, plus any rows named in
  // MOCKTAIL_LAUNCHER_SELFTEST_HINTS (comma-separated keys or titles), a
  // developer aid for checking hint text by eye.
  std::vector<std::string> hints = {"graphics.backend"};
  if (const char* extra = std::getenv("MOCKTAIL_LAUNCHER_SELFTEST_HINTS")) {
    std::string list = extra;
    std::size_t start = 0;
    while (start <= list.size()) {
      const std::size_t comma = std::min(list.find(',', start), list.size());
      if (comma > start) hints.push_back(list.substr(start, comma - start));
      start = comma + 1;
    }
  }
  for (const std::string& hint : hints) {
    steps_.push_back([this, hint] { return OpenHint(hint); });
    steps_.push_back([this, hint] { return RenderHint(hint); });
  }
  steps_.push_back([this] {
    window_->ShowSection(Section::kGraphics);
    return kSettleMilliseconds;
  });
  steps_.push_back([this] { return BuildBindingPage(); });
  steps_.push_back([this] { return ChangeRows(); });
  steps_.push_back([this] { return RenderBindingPage(); });
  steps_.push_back([this] { return SearchStep(); });
  steps_.push_back([this] { return SaveStep(); });
  steps_.push_back([this] { return ChangedOnDisk(); });
  steps_.push_back([this] { return OpenEnvironmentDialog(); });
  steps_.push_back([this] { return RenderEnvironmentDialog(); });
  steps_.push_back([this] { return MoveEnvironment(); });
  // What Mocktail decides and what Roblox decides: every kind of override
  // at once, with a level above 3 under the performance preset, shown on
  // the Advanced page's overview and as badges on the Graphics page.
  steps_.push_back([this] { return ShowRobloxDecides(); });
  steps_.push_back([this] {
    const int width = gtk_widget_get_width(window_->widget());
    const std::filesystem::path path =
        out_dir_ / ("decides-" + std::to_string(width) + ".png");
    std::string detail;
    if (!Render(window_->content(), path, &detail)) {
      Error("cannot render " + path.string());
    } else {
      rendered_.push_back(path.filename().string() + " " + detail);
    }
    // Both lists open, scrolled to the end of the second.
    GtkWidget* lists = nullptr;
    for (const RowRecord& record : context_->rows()) {
      if (record.row != nullptr && ADW_IS_EXPANDER_ROW(record.row) &&
          record.section == Section::kAdvanced &&
          (record.title == _("Left to Roblox") ||
           record.title == _("Always set by Mocktail"))) {
        adw_expander_row_set_expanded(ADW_EXPANDER_ROW(record.row), TRUE);
        lists = record.row;
      }
    }
    if (lists == nullptr) Error("the overview's lists are missing");
    decides_lists_ = lists;
    return kSettleMilliseconds * 2;
  });
  // Once they are open, their last row can take focus.
  steps_.push_back([this] {
    if (decides_lists_ != nullptr) {
      GtkWidget* last = LastActionRow(decides_lists_);
      window_->Reveal(last != nullptr ? last : decides_lists_);
    }
    return kSettleMilliseconds * 2;
  });
  steps_.push_back([this] {
    const int width = gtk_widget_get_width(window_->widget());
    const std::filesystem::path path =
        out_dir_ / ("decides-lists-" + std::to_string(width) + ".png");
    std::string detail;
    if (!Render(window_->content(), path, &detail)) {
      Error("cannot render " + path.string());
    } else {
      rendered_.push_back(path.filename().string() + " " + detail);
    }
    window_->ShowSection(Section::kGraphics);
    return kSettleMilliseconds;
  });
  steps_.push_back(
      [this] { return RenderSection(Section::kGraphics, "overrides"); });
  steps_.push_back([this] {
    window_->ShowSection(Section::kDisplay);
    return kSettleMilliseconds;
  });
  steps_.push_back(
      [this] { return RenderSection(Section::kDisplay, "overrides"); });
  steps_.push_back([this] {
    context_->Discard();
    return kSettleMilliseconds;
  });
  // The proxy choice spans three keys: a manual proxy without a host must
  // block Save and Play, and "No proxy" must remove the host and port.
  steps_.push_back([this] {
    GtkWidget* proxy = nullptr;
    for (const RowRecord& record : context_->rows()) {
      if (record.title == _("Proxy") && ADW_IS_COMBO_ROW(record.row)) {
        proxy = record.row;
      }
    }
    if (proxy == nullptr) {
      Error("the proxy row is missing");
      return guint{50};
    }
    adw_combo_row_set_selected(ADW_COMBO_ROW(proxy), 2);  // Manual
    if (context_->problem_count() != 1 || context_->can_play()) {
      Error("a manual proxy without a host did not block Play");
    }
    context_->SetValue("network.proxy_host", "127.0.0.1",
                       launcher::ScalarKind::kString);
    std::string error;
    if (context_->problem_count() != 0 || !context_->draft().Validate(&error)) {
      Error("a complete manual proxy is not accepted: " + error);
    }
    adw_combo_row_set_selected(ADW_COMBO_ROW(proxy), 0);  // No proxy
    if (context_->Value("network.proxy_host").has_value() ||
        context_->Value("network.proxy_port").has_value()) {
      Error("\"No proxy\" left the manual proxy in config.yaml");
    }
    Note("proxy_rows_checked", "true");
    context_->Discard();
    return kSettleMilliseconds;
  });
  // The dialogs pages open from a row, rendered with the whole window like
  // the environment dialog (<out-dir>/dialog-<name>.png).
  std::vector<std::pair<const char*, void (*)(LauncherContext*)>> dialogs = {
      {"discord-texts", OpenDiscordTextsDialog},
      {"fast-flags", OpenFastFlagsEditor},
      {"about", OpenAboutDialog},
  };
  // A broken config.yaml (the matrix's empty-file runs): its Fix… dialog.
  if (context_->read_only()) {
    dialogs.emplace_back("config-error", OpenConfigErrorDialog);
  }
  for (const auto& [name, open] : dialogs) {
    steps_.push_back([this, open = open] {
      open(context_);
      return kSettleMilliseconds * 2;
    });
    steps_.push_back([this, name = std::string(name)] {
      AdwDialog* dialog = adw_application_window_get_visible_dialog(
          ADW_APPLICATION_WINDOW(window_->widget()));
      if (dialog == nullptr) {
        Error("the " + name + " dialog did not open");
        return guint{50};
      }
      const std::filesystem::path path = out_dir_ / ("dialog-" + name + ".png");
      std::string detail;
      GtkWidget* root = gtk_window_get_child(GTK_WINDOW(window_->widget()));
      if (root == nullptr || !Render(root, path, &detail)) {
        Error("cannot render " + path.string());
      } else {
        rendered_.push_back(path.filename().string() + " " + detail);
      }
      adw_dialog_force_close(dialog);
      return kSettleMilliseconds;
    });
  }
  steps_.push_back([this] { return Resize(480, 720); });
  steps_.push_back([this] { return RecordResize(480); });
  for (const SectionInfo& info : Sections()) {
    const Section section = info.section;
    steps_.push_back([this, section] {
      window_->ShowSection(section);
      return kSettleMilliseconds;
    });
    steps_.push_back(
        [this, section] { return RenderSection(section, "narrow"); });
  }
  steps_.push_back([this] { return Resize(980, 700); });
  steps_.push_back([this] { return RecordResize(980); });
  steps_.push_back([this] { return CheckHints(); });
  steps_.push_back([this] { return Finish(); });
  RunNext();
}

void Selftest::RunNext() {
  if (next_step_ >= steps_.size()) return;
  // A copy: a step may insert further steps, moving the vector.
  const Step step = steps_[next_step_++];
  const guint delay = step();
  if (next_step_ >= steps_.size()) return;
  g_timeout_add(
      delay,
      [](gpointer data) -> gboolean {
        static_cast<Selftest*>(data)->RunNext();
        return G_SOURCE_REMOVE;
      },
      this);
}

void Selftest::Error(std::string message) {
  std::fprintf(stderr, "mocktail-launcher-ui selftest: error: %s\n",
               message.c_str());
  errors_.push_back(std::move(message));
}

void Selftest::Note(std::string key, std::string json_value) {
  notes_.emplace_back(std::move(key), std::move(json_value));
}

guint Selftest::WaitForMachine() {
  if (!context_->machine().detected && ++machine_waits_ < kMachineWaitLimit) {
    --next_step_;  // run this step again
    return 100;
  }
  if (!context_->machine().detected) {
    Error("machine profile detection did not finish");
  }
  return 50;
}

guint Selftest::RecordWindow() {
  GtkWidget* widget = window_->widget();
  GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
  GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(widget));
  const MonitorInfo& monitor = context_->machine().monitor;
  const MachineProfile& machine = context_->machine();
  Note("window_mapped", gtk_widget_get_mapped(widget) ? "true" : "false");
  if (!gtk_widget_get_mapped(widget)) Error("window is not mapped");
  Note("window_width", std::to_string(gtk_widget_get_width(widget)));
  Note("window_height", std::to_string(gtk_widget_get_height(widget)));
  Note("surface_scale",
       surface != nullptr
           ? nlohmann::json(gdk_surface_get_scale(surface)).dump()
           : "null");
  Note("renderer",
       Quote(renderer != nullptr ? G_OBJECT_TYPE_NAME(renderer) : "none"));
  Note("collapsed", window_->collapsed() ? "true" : "false");
  nlohmann::json monitor_json = {
      {"valid", monitor.valid}, {"connector", monitor.connector},
      {"width", monitor.width}, {"height", monitor.height},
      {"scale", monitor.scale}, {"refresh_hz", monitor.RefreshHz()},
  };
  Note("monitor", monitor_json.dump(-1, ' ', false,
                                    nlohmann::json::error_handler_t::replace));
  const std::string backend =
      context_->GameValue("graphics.backend", "direct-vulkan");
  const std::string gpu_preference = context_->GameValue("engine.gpu", "auto");
  const bool unthrottled =
      ResolvePresentation(
          context_->GameValue("graphics.vsync", "auto"),
          context_->GameValue("graphics.frame_rate_limit", "-1")) ==
      Presentation::kUnthrottled;
  const char* explicit_sync = "unknown";
  if (machine.wayland_explicit_sync == window::WaylandExplicitSync::kOffered) {
    explicit_sync = "offered";
  } else if (machine.wayland_explicit_sync ==
             window::WaylandExplicitSync::kAbsent) {
    explicit_sync = "absent";
  }
  nlohmann::json machine_json = {
      {"detected", machine.detected},
      {"gpu", machine.GpuVendorsLabel()},
      {"nvidia_kernel_driver", machine.gpu.nvidia_kernel_driver},
      {"nvidia_driver_version", machine.gpu.nvidia_driver_version},
      {"explicit_sync", explicit_sync},
      {"hyprland_compositor", machine.hyprland_compositor},
      {"nvidia_explicit_sync_disabled", machine.nvidia_explicit_sync_disabled},
      {"unthrottled_presentation", unthrottled},
      {"nvidia_rule", machine.NvidiaRuleApplies(backend, gpu_preference)},
      {"nvidia_wayland_blocker",
       static_cast<int>(machine.NvidiaNativeWaylandBlocker(unthrottled))},
      {"automatic_display_server",
       machine.AutomaticDisplayServer(backend, gpu_preference, unthrottled)},
      {"desktop", machine.desktop},
      {"wayland", machine.wayland_available},
      {"x11", machine.x11_available},
      {"physical_cores", machine.physical_cores},
      {"memory_mib", machine.memory_bytes / (1024U * 1024U)},
      {"gamemode_library", machine.gamemode_library},
      {"vulkan_icd", machine.VulkanDriver(gpu_preference).icd},
      {"angle", machine.angle.has_value() ? machine.angle->directory.string()
                                          : std::string()},
  };
  Note("machine", machine_json.dump(-1, ' ', false,
                                    nlohmann::json::error_handler_t::replace));
  const char* messages = setlocale(LC_MESSAGES, nullptr);
  Note("locale", Quote(messages != nullptr ? messages : ""));
  Note("translation_sample", Quote(_("Graphics")));
  if (messages != nullptr && std::string(messages).rfind("ru", 0) == 0 &&
      std::string(_("Graphics")) == "Graphics") {
    Error("Russian locale but the catalogue is not loaded");
  }
  BannerKind kind = BannerKind::kConfigError;
  const Banner* banner = context_->TopBanner(&kind);
  Note("banner", Quote(banner != nullptr ? banner->title : ""));
  if (banner == nullptr || kind != BannerKind::kEnvironmentOverrides) {
    Error("the environment override banner is not shown");
  }
  Note("read_only", context_->read_only() ? "true" : "false");
  if (context_->read_only()) Error("scratch config.yaml loaded read-only");
  return 50;
}

bool Selftest::Render(GtkWidget* content, const std::filesystem::path& path,
                      std::string* detail) {
  GtkNative* native = gtk_widget_get_native(content);
  GskRenderer* renderer = gtk_native_get_renderer(native);
  GdkSurface* surface = gtk_native_get_surface(native);
  const double scale = surface != nullptr ? gdk_surface_get_scale(surface) : 1;
  const int width = gtk_widget_get_width(content);
  const int height = gtk_widget_get_height(content);
  if (renderer == nullptr || width <= 0 || height <= 0) {
    *detail = "nothing to render";
    return false;
  }
  GdkPaintable* paintable = gtk_widget_paintable_new(content);
  GtkSnapshot* snapshot = gtk_snapshot_new();
  gtk_snapshot_scale(snapshot, static_cast<float>(scale),
                     static_cast<float>(scale));
  // The window draws the background, not the content.
  GdkRGBA background;
  gdk_rgba_parse(&background, "#222226");
  const graphene_rect_t bounds = GRAPHENE_RECT_INIT(
      0, 0, static_cast<float>(width), static_cast<float>(height));
  gtk_snapshot_append_color(snapshot, &background, &bounds);
  gdk_paintable_snapshot(paintable, snapshot, width, height);
  GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
  bool ok = false;
  if (node != nullptr) {
    const graphene_rect_t viewport =
        GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width * scale),
                           static_cast<float>(height * scale));
    GdkTexture* texture =
        gsk_renderer_render_texture(renderer, node, &viewport);
    if (texture != nullptr) {
      ok = gdk_texture_save_to_png(texture, path.c_str());
      *detail = std::to_string(gdk_texture_get_width(texture)) + "x" +
                std::to_string(gdk_texture_get_height(texture));
      g_object_unref(texture);
    }
    gsk_render_node_unref(node);
  }
  g_object_unref(paintable);
  return ok;
}

guint Selftest::RenderSection(Section section, const std::string& suffix) {
  const SectionInfo& info = GetSectionInfo(section);
  const int width = gtk_widget_get_width(window_->widget());
  const std::filesystem::path path =
      out_dir_ / (std::string(info.id) + "-" + std::to_string(width) +
                  (suffix.empty() ? "" : "-" + suffix) + ".png");
  std::string detail;
  if (window_->current_section() != section) {
    Error(std::string("section did not open: ") + info.id);
  }
  if (!Render(window_->content(), path, &detail)) {
    Error("cannot render " + path.string() + ": " + detail);
  } else {
    rendered_.push_back(path.filename().string() + " " + detail);
  }
  return 50;
}

namespace {

GtkWidget* FindInfoButton(GtkWidget* widget) {
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_MENU_BUTTON(child) &&
        gtk_widget_has_css_class(child, "info-button")) {
      return child;
    }
    if (GtkWidget* found = FindInfoButton(child)) return found;
  }
  return nullptr;
}

}  // namespace

// Opens a row's "Learn more" popover and renders it, so the hint text can
// be checked by eye.
guint Selftest::OpenHint(const std::string& key) {
  GtkWidget* row = nullptr;
  for (const RowRecord& record : context_->rows()) {
    if (row == nullptr && record.row != nullptr &&
        (record.key == key || record.title == key)) {
      row = record.row;
    }
  }
  hint_button_ = row != nullptr ? FindInfoButton(row) : nullptr;
  if (hint_button_ == nullptr) {
    Error("no row with an info button for " + key);
    return 50;
  }
  // Opens its page (and any expander around it) first.
  window_->Reveal(row);
  gtk_menu_button_popup(GTK_MENU_BUTTON(hint_button_));
  return kSettleMilliseconds * 2;
}

guint Selftest::RenderHint(const std::string& key) {
  if (hint_button_ == nullptr) return 50;
  GtkPopover* popover =
      gtk_menu_button_get_popover(GTK_MENU_BUTTON(hint_button_));
  // The scrolled window's content, so text below its fold is rendered too.
  GtkWidget* child =
      popover != nullptr ? gtk_popover_get_child(popover) : nullptr;
  std::string name = key;
  for (char& c : name) {
    if (!g_ascii_isalnum(c)) c = '-';
  }
  if (child != nullptr && GTK_IS_SCROLLED_WINDOW(child)) {
    // How tall the text may get here (bindings.cc HintContentHeight) and
    // how tall the popover is, for checking short screens.
    Note("hint_" + name + "_height",
         "{\"max_text\": " +
             std::to_string(gtk_scrolled_window_get_max_content_height(
                 GTK_SCROLLED_WINDOW(child))) +
             ", \"popover\": " +
             std::to_string(gtk_widget_get_height(GTK_WIDGET(popover))) + "}");
    child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(child));
  }
  if (child != nullptr && GTK_IS_VIEWPORT(child)) {
    child = gtk_viewport_get_child(GTK_VIEWPORT(child));
  }
  const std::filesystem::path path = out_dir_ / ("hint-" + name + ".png");
  std::string detail;
  if (child == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(popover))) {
    Error("the hint popover did not open");
  } else if (!Render(child, path, &detail)) {
    Error("cannot render " + path.string() + ": " + detail);
  } else {
    rendered_.push_back(path.filename().string() + " " + detail);
  }
  gtk_menu_button_popdown(GTK_MENU_BUTTON(hint_button_));
  return kSettleMilliseconds;
}

guint Selftest::BuildBindingPage() {
  // One row of each kind on a page the sidebar does not show.
  context_->BeginSection(Section::kAdvanced);
  binding_page_ = NewPage(context_, Section::kAdvanced, "Self-test rows");
  GtkWidget* group = AddGroup(binding_page_, "Bindings", "");
  RowSpec show;
  show.key = "launcher.show_on_start";
  show.title = "Show this window on start";
  switch_row_ = BindSwitchRow(context_, show);
  AddRow(group, switch_row_);
  RowSpec memory;
  memory.key = "performance.memory_limit_mb";
  memory.title = "Memory limit (MiB)";
  SpinSpec spin;
  spin.maximum = 1048576;
  spin.step = 256;
  spin_row_ = BindSpinRow(context_, memory, spin);
  AddRow(group, spin_row_);
  RowSpec title;
  title.key = "window.title";
  title.title = "Window title";
  EntrySpec entry;
  entry.validate = [](const std::string& text) {
    return text.empty() ? std::string("must not be empty") : std::string();
  };
  entry_row_ = BindEntryRow(context_, title, entry);
  AddRow(group, entry_row_);
  RowSpec theme;
  theme.key = "appearance.theme";
  theme.title = "Theme";
  ComboSpec combo;
  combo.options = {{"roblox", "Roblox", "", {}, nullptr, nullptr, false, false},
                   {"system", "System", "", {}, nullptr, nullptr, false, false},
                   {"light", "Light", "", {}, nullptr, nullptr, false, false},
                   {"dark", "Dark", "", {}, nullptr, nullptr, false, false}};
  combo_row_ = BindComboRow(context_, theme, combo);
  AddRow(group, combo_row_);
  window_->AddHiddenPage(kBindingPageName, binding_page_);
  return 100;
}

guint Selftest::ChangeRows() {
  const auto expect = [this](const char* key, const std::string& value) {
    const std::optional<std::string> actual = context_->Value(key);
    if (actual != value) {
      Error(std::string(key) + " is " + actual.value_or("(absent)") +
            " after the change, expected " + value);
    }
  };
  const bool show = adw_switch_row_get_active(ADW_SWITCH_ROW(switch_row_));
  adw_switch_row_set_active(ADW_SWITCH_ROW(switch_row_), !show);
  expect("launcher.show_on_start", show ? "false" : "true");
  adw_spin_row_set_value(ADW_SPIN_ROW(spin_row_), 2048);
  expect("performance.memory_limit_mb", "2048");

  // An invalid entry blocks Save until it is fixed.
  gtk_editable_set_text(GTK_EDITABLE(entry_row_), "");
  if (context_->problem_count() != 1 || context_->can_play()) {
    Error("an invalid entry did not block Play");
  }
  gtk_editable_set_text(GTK_EDITABLE(entry_row_), "Roblox self-test");
  if (context_->problem_count() != 0) Error("the entry problem stayed");
  expect("window.title", "Roblox self-test");

  const guint dark = FindModelPosition(combo_row_, "Dark");
  if (dark == GTK_INVALID_LIST_POSITION) {
    Error("theme combo has no Dark option");
  } else {
    adw_combo_row_set_selected(ADW_COMBO_ROW(combo_row_), dark);
    expect("appearance.theme", "dark");
  }

  GtkWidget* backend = nullptr;
  for (const RowRecord& record : context_->rows()) {
    if (record.key == "graphics.backend") backend = record.row;
  }
  if (backend == nullptr) {
    Error("the graphics backend row is missing");
  } else {
    const guint opengl = FindModelPosition(backend, _("OpenGL ES"));
    if (opengl == GTK_INVALID_LIST_POSITION) {
      Error("the graphics backend row has no OpenGL ES option");
    } else {
      adw_combo_row_set_selected(ADW_COMBO_ROW(backend), opengl);
      expect("graphics.backend", "opengl");
    }
  }
  context_->SetValue("window.width", "1366");
  context_->SetValue("window.height", "768");
  Note("unsaved_before_save", std::to_string(context_->unsaved_count()));
  if (context_->unsaved_count() < 7) {
    Error("expected at least 7 unsaved changes, have " +
          std::to_string(context_->unsaved_count()));
  }
  return 100;
}

guint Selftest::RenderBindingPage() {
  window_->ShowHiddenPage(kBindingPageName);
  steps_.insert(
      steps_.begin() + static_cast<std::ptrdiff_t>(next_step_), [this] {
        const int width = gtk_widget_get_width(window_->widget());
        const std::filesystem::path path =
            out_dir_ / ("bindings-" + std::to_string(width) + ".png");
        std::string detail;
        if (!Render(window_->content(), path, &detail)) {
          Error("cannot render " + path.string());
        } else {
          rendered_.push_back(path.filename().string() + " " + detail);
        }
        return guint{50};
      });
  return kSettleMilliseconds;
}

guint Selftest::SearchStep() {
  window_->Search("vulkan");
  Note("search_vulkan_results", std::to_string(window_->search_result_count()));
  if (window_->search_result_count() < 1) {
    Error("searching for vulkan found nothing");
  }
  steps_.insert(
      steps_.begin() + static_cast<std::ptrdiff_t>(next_step_), [this] {
        const int width = gtk_widget_get_width(window_->widget());
        const std::filesystem::path path =
            out_dir_ / ("search-" + std::to_string(width) + ".png");
        std::string detail;
        if (!Render(window_->content(), path, &detail)) {
          Error("cannot render " + path.string());
        } else {
          rendered_.push_back(path.filename().string() + " " + detail);
        }
        // A variable name finds the setting it overrides.
        window_->Search("MOCKTAIL_GRAPHICS_BACKEND");
        Note("search_variable_results",
             std::to_string(window_->search_result_count()));
        if (window_->search_result_count() < 1) {
          Error("searching for a variable name found nothing");
        }
        window_->Search("");
        return kSettleMilliseconds;
      });
  return kSettleMilliseconds;
}

guint Selftest::SaveStep() {
  if (!context_->Save()) {
    Error("Save failed");
    return 50;
  }
  if (context_->unsaved_count() != 0) Error("changes left after Save");
  launcher::ConfigDocument saved;
  std::string error;
  if (!launcher::ConfigDocument::Load(context_->config_file(), &saved,
                                      &error)) {
    Error("cannot read the saved config.yaml: " + error);
    return 50;
  }
  const bool valid = saved.Validate(&error);
  Note("save_validated", valid ? "true" : "false");
  if (!valid) Error("the saved config.yaml does not load: " + error);
  const auto expect = [this, &saved](const char* key,
                                     const std::string& value) {
    if (saved.Get(key) != value) {
      Error(std::string("saved ") + key + " is " +
            saved.Get(key).value_or("(absent)") + ", expected " + value);
    }
  };
  expect("performance.memory_limit_mb", "2048");
  expect("window.title", "Roblox self-test");
  expect("appearance.theme", "dark");
  expect("graphics.backend", "opengl");
  expect("window.width", "1366");
  if (saved.bytes().rfind("# Mocktail configuration.", 0) != 0) {
    Error("the template's comments did not survive Save");
  }
  launcher::RememberedWindowState state;
  if (!launcher::ReadRememberedWindowState(context_->window_state_file(),
                                           &state, &error) ||
      !state.found || state.width != 1366 || state.height != 768) {
    Error("window-state.json does not hold the saved window size");
  }
  Note("window_state_updated", state.width == 1366 ? "true" : "false");
  return 100;
}

// config.yaml changed in an editor while the window had unsaved changes:
// Save is refused, and the banner with Reload must be the one shown even
// while variables override settings (that banner only informs).
guint Selftest::ChangedOnDisk() {
  if (context_->read_only()) return 50;
  context_->SetValue("window.title", "Roblox changed on disk",
                     launcher::ScalarKind::kString);
  {
    std::ofstream output(context_->config_file(),
                         std::ios::binary | std::ios::app);
    output << "# Edited in a text editor.\n";
  }
  if (context_->Save()) {
    Error("Save wrote over a config.yaml changed outside the window");
  }
  BannerKind kind = BannerKind::kConfigError;
  const Banner* banner = context_->TopBanner(&kind);
  const bool shown = banner != nullptr && kind == BannerKind::kChangedOnDisk;
  Note("changed_on_disk_banner", shown ? "true" : "false");
  if (!shown) {
    Error("the changed-on-disk banner is hidden behind " +
          Quote(banner != nullptr ? banner->title : ""));
  }
  context_->Discard();
  context_->Reload();
  kind = BannerKind::kConfigError;
  banner = context_->TopBanner(&kind);
  if (banner == nullptr || kind != BannerKind::kEnvironmentOverrides) {
    Error("the environment banner did not come back after Reload");
  }
  return kSettleMilliseconds;
}

// The banner's Details button opens the dialog that lists the overriding
// variables; it is rendered with the whole window, which hosts dialogs.
guint Selftest::OpenEnvironmentDialog() {
  BannerKind kind = BannerKind::kConfigError;
  const Banner* banner = context_->TopBanner(&kind);
  if (banner == nullptr || kind != BannerKind::kEnvironmentOverrides ||
      !banner->on_button) {
    Error("no environment banner to open");
    return 50;
  }
  const std::function<void()> open = banner->on_button;
  open();
  return kSettleMilliseconds * 2;
}

guint Selftest::RenderEnvironmentDialog() {
  AdwDialog* dialog = adw_application_window_get_visible_dialog(
      ADW_APPLICATION_WINDOW(window_->widget()));
  if (dialog == nullptr) {
    Error("the environment dialog did not open");
    return 50;
  }
  const std::filesystem::path path = out_dir_ / "environment-dialog.png";
  std::string detail;
  GtkWidget* root = gtk_window_get_child(GTK_WINDOW(window_->widget()));
  if (root == nullptr || !Render(root, path, &detail)) {
    Error("cannot render " + path.string());
  } else {
    rendered_.push_back(path.filename().string() + " " + detail);
  }
  adw_dialog_force_close(dialog);
  return kSettleMilliseconds;
}

// "Move into settings": the override's value lands in the draft, the
// banner goes, and Play would report "play ignore-env".
guint Selftest::MoveEnvironment() {
  context_->MoveEnvironmentIntoSettings();
  if (!context_->ignoring_environment()) {
    Error("moving the environment did not mark it ignored");
  }
  BannerKind kind = BannerKind::kConfigError;
  if (context_->TopBanner(&kind) != nullptr &&
      kind == BannerKind::kEnvironmentOverrides) {
    Error("the environment banner stayed after moving the variables");
  }
  if (context_->Value("graphics.backend") !=
      std::optional<std::string>("direct-vulkan")) {
    Error("the overriding value was not moved into the draft");
  }
  if (!context_->Save()) Error("Save after moving the variables failed");
  if (!context_->can_play()) {
    Error("Play is blocked after moving the variables: " +
          context_->play_blocker());
  }
  GAction* play =
      g_action_map_lookup_action(G_ACTION_MAP(window_->widget()), "play");
  if (play == nullptr || !g_action_get_enabled(play)) {
    Error("the play action is disabled");
  }
  Note("environment_moved",
       context_->ignoring_environment() ? "true" : "false");
  return kSettleMilliseconds;
}

guint Selftest::ShowRobloxDecides() {
  context_->SetValue("engine.graphics_quality", "12",
                     launcher::ScalarKind::kInteger);
  context_->SetValue("graphics.frame_rate_limit", "144",
                     launcher::ScalarKind::kInteger);
  context_->SetValue("display.start_mode", "fullscreen");
  context_->SetValue("appearance.theme", "dark");
  context_->SetValue("audio.input_device", "disabled");
  const GameSettings settings = CurrentGameSettings(*context_);
  const std::vector<RobloxOverride> overrides =
      ResolveRobloxOverrides(settings);
  Note("roblox_overrides", std::to_string(overrides.size()));
  // Quality, frame rate, start mode, theme, the preset, the microphone and
  // the PC profile.
  if (overrides.size() != 7) {
    Error("expected 7 overrides of Roblox's settings, have " +
          std::to_string(overrides.size()));
  }
  if (FindOverrideConflicts(settings).size() != 1) {
    Error("level 12 under the preset is not reported as a conflict");
  }
  // Every key whose value causes an override shows the badge on a row
  // (a custom level or frame rate under its list leaves it to the list),
  // and no other row shows it.
  int badges = 0;
  std::map<std::string, bool> badge_for_key;
  for (const RowRecord& record : context_->rows()) {
    // The self-test's own rows have no hints.
    if (record.row == nullptr || record.kind == RowKind::kAction ||
        (binding_page_ != nullptr &&
         gtk_widget_is_ancestor(record.row, binding_page_))) {
      continue;
    }
    GtkWidget* badge = FindByClass(record.row, "override-badge");
    const bool shown = badge != nullptr && gtk_widget_get_visible(badge);
    const bool expected = RobloxOverrideOf(settings, record.key).has_value();
    if (expected) {
      badge_for_key[record.key] = badge_for_key[record.key] || shown;
    } else if (shown) {
      Error("the override badge of " + record.key + " is shown");
    }
    if (shown) ++badges;
    // Under the subtitle, in the box of the row's title and subtitle.
    if (shown && record.key == "engine.graphics_quality") {
      GtkWidget* parent = gtk_widget_get_parent(badge);
      const bool under_subtitle =
          parent != nullptr && gtk_widget_has_css_class(parent, "title");
      Note("override_badge_under_subtitle", under_subtitle ? "true" : "false");
      if (!under_subtitle) {
        warnings_.push_back("the override badge is among the row's suffixes");
      }
    }
  }
  for (const auto& [key, shown] : badge_for_key) {
    if (!shown) Error("no row shows the override badge of " + key);
  }
  Note("override_badges", std::to_string(badges));
  GtkWidget* first = nullptr;
  for (const RowRecord& record : context_->rows()) {
    if (first == nullptr && record.row != nullptr &&
        record.section == Section::kAdvanced &&
        record.title == _("Graphics Quality slider")) {
      first = record.row;
    }
  }
  if (first == nullptr || !gtk_widget_get_visible(first)) {
    Error("the overview does not list the graphics quality override");
  } else {
    window_->Reveal(first);
  }
  return kSettleMilliseconds * 2;
}

guint Selftest::Resize(int width, int height) {
  GtkWindow* window = GTK_WINDOW(window_->widget());
  gtk_window_unmaximize(window);
  gtk_window_set_default_size(window, width, height);
  return kResizeMilliseconds;
}

guint Selftest::RecordResize(int requested_width) {
  const int width = gtk_widget_get_width(window_->widget());
  const bool collapsed = window_->collapsed();
  // AdwBreakpoint max-width: 640sp; sp follows the text scale (1 here).
  const bool expected = width <= 640;
  resize_observations_.push_back(
      nlohmann::json({{"requested", requested_width},
                      {"width", width},
                      {"collapsed", collapsed},
                      {"narrow", context_->narrow()}})
          .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  if (collapsed != expected) {
    Error("at width " + std::to_string(width) + " collapsed is " +
          (collapsed ? "true" : "false"));
  }
  if (width != requested_width) {
    warnings_.push_back("the compositor kept the window at " +
                        std::to_string(width) + " px instead of " +
                        std::to_string(requested_width));
  }
  return 100;
}

guint Selftest::CheckHints() {
  std::string missing_details;
  for (const RowRecord& record : context_->rows()) {
    bool selftest_row = false;
    for (const char* key : kSelftestKeys) {
      if (record.key == key) selftest_row = true;
    }
    if (selftest_row) continue;
    if (!record.has_details || !record.has_subtitle) {
      warnings_.push_back(
          "row without " +
          std::string(!record.has_details ? "details" : "subtitle") + ": " +
          record.title);
    }
  }
  GtkIconTheme* theme =
      gtk_icon_theme_get_for_display(gtk_widget_get_display(window_->widget()));
  for (const char* icon : kIcons) {
    if (!gtk_icon_theme_has_icon(theme, icon)) {
      Error(std::string("missing icon ") + icon);
    }
  }
  Note("rows", std::to_string(context_->rows().size()));
  return 50;
}

guint Selftest::Finish() {
  nlohmann::json report;
  for (const auto& [key, value] : notes_) {
    report[key] = nlohmann::json::parse(value, nullptr, false);
  }
  report["pages_rendered"] = rendered_;
  nlohmann::json resizes = nlohmann::json::array();
  for (const std::string& observation : resize_observations_) {
    resizes.push_back(nlohmann::json::parse(observation, nullptr, false));
  }
  report["resize"] = resizes;
  report["warnings"] = warnings_;
  report["errors"] = errors_;
  report["ok"] = errors_.empty();
  std::string error;
  if (!WriteFile(
          out_dir_ / "report.json",
          report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) +
              "\n",
          &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    errors_.push_back(error);
  }
  std::printf(
      "mocktail-launcher-ui selftest: %zu pages rendered, %zu "
      "warnings, %zu errors\n",
      rendered_.size(), warnings_.size(), errors_.size());
  std::fflush(stdout);
  window_->Finish();
  return 0;
}

}  // namespace mocktail::launcher_ui
