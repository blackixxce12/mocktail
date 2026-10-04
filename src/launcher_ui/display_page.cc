#include <charconv>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "launcher/window_state_file.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kWidthKey[] = "window.width";
constexpr char kHeightKey[] = "window.height";
constexpr char kStartModeKey[] = "display.start_mode";
constexpr char kHighDpiKey[] = "window.high_dpi";
constexpr char kServerKey[] = "display.server";

std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc() ||
      parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::string SizeText(WindowSize size) {
  return Format("%d × %d", size.width, size.height);
}

int Percent(double scale) { return static_cast<int>(std::lround(scale * 100)); }

bool IsVulkanSpelling(const std::string& value) {
  return value == "direct-vulkan" || value == "vulkan" ||
         value == "native-vulkan";
}

std::string Backend(const LauncherContext& context) {
  return context.EffectiveValue("graphics.backend", "direct-vulkan");
}

DisplayServerChoice ServerChoice(const LauncherContext& context,
                                 std::string_view configured) {
  return ResolveDisplayServer(context.machine(), configured, Backend(context));
}

// Whether the game window will be a Wayland one: native resolution and the
// pixel counts only mean something there (research/graphics.md 2.2).
bool GameUsesWayland(const LauncherContext& context) {
  const DisplayServerChoice choice =
      ServerChoice(context, context.EffectiveValue(kServerKey, "auto"));
  if (choice.server.empty()) {
    return context.machine().session != SessionType::kX11;
  }
  return choice.server == "wayland";
}

// What the Display page's rows share: the window state the game remembers
// (window-state.json, which replaces window.width/height at every start,
// window.cc), and the window size chosen here, which Save writes into that
// file even when config.yaml already holds the same size. Owned by the page.
class DisplayPageState {
 public:
  explicit DisplayPageState(LauncherContext* context) : context_(context) {
    ReadRemembered();
    listeners_.push_back(
        context_->OnSettingChanged([this](std::string_view key) {
          if (key.empty()) {
            // Saved, discarded, reloaded or imported.
            ReadRemembered();
            chosen_.reset();
            if (context_->draft().IsChanged(kWidthKey) ||
                context_->draft().IsChanged(kHeightKey)) {
              chosen_ = ConfigSize();
            }
          } else if (key == kWidthKey || key == kHeightKey) {
            chosen_ = ConfigSize();
          }
          if (key.empty() || key == kWidthKey || key == kHeightKey ||
              key == kStartModeKey || key == kHighDpiKey || key == kServerKey ||
              key == "graphics.backend") {
            RebuildSizes();
          }
        }));
    listeners_.push_back(
        context_->OnMachineChanged([this] { RebuildSizes(); }));
    // The context keeps its dirty sources for good; they reach this object
    // only while it lives.
    DirtySource source;
    source.name = "window-state.json";
    source.count = [self = self_] {
      return *self != nullptr && (*self)->RememberedUpdatePending() ? 1 : 0;
    };
    source.save = [self = self_](std::string*) {
      DisplayPageState* state = *self;
      if (state == nullptr || !state->RememberedUpdatePending()) return true;
      std::string problem;
      if (!launcher::UpdateWindowedSize(state->context_->window_state_file(),
                                        state->chosen_->width,
                                        state->chosen_->height, &problem)) {
        // config.yaml is saved; only the remembered copy is stale, which
        // must not undo the rest of Save.
        state->context_->Toast(
            Format(_("The window size was saved, but the remembered size "
                     "could not be updated: %s"),
                   problem.c_str()));
      }
      return true;
    };
    source.discard = [self = self_] {
      if (*self != nullptr) (*self)->chosen_.reset();
    };
    context_->AddDirtySource(std::move(source));
  }
  ~DisplayPageState() {
    *self_ = nullptr;
    for (const LauncherContext::ListenerId id : listeners_) {
      context_->RemoveListener(id);
    }
  }
  DisplayPageState(const DisplayPageState&) = delete;
  DisplayPageState& operator=(const DisplayPageState&) = delete;

  const launcher::RememberedWindowState& remembered() const {
    return remembered_;
  }

  WindowSize ConfigSize() const {
    return {ParseInt(context_->EffectiveValue(kWidthKey, "1280"))
                .value_or(kDefaultWindowSize.width),
            ParseInt(context_->EffectiveValue(kHeightKey, "720"))
                .value_or(kDefaultWindowSize.height)};
  }

  WindowSize RememberedSize() const {
    return {remembered_.width, remembered_.height};
  }

  // The windowed size the next start uses: the one chosen here, else the
  // remembered one, else config.yaml's.
  WindowSize EffectiveSize() const {
    if (chosen_.has_value()) return *chosen_;
    if (remembered_.found) return RememberedSize();
    return ConfigSize();
  }

  // The size comes from window-state.json, not from this window or
  // config.yaml.
  bool SizeFromRemembered() const {
    return !chosen_.has_value() && remembered_.found;
  }

  WindowMode Mode() const {
    return StartWindowMode(context_->EffectiveValue(kStartModeKey, "remember"),
                           remembered_.found, remembered_.fullscreen,
                           remembered_.maximized);
  }

  GameResolution Resolution(bool high_dpi) const {
    return ComputeGameResolution(context_->machine().monitor, EffectiveSize(),
                                 Mode(), high_dpi, GameUsesWayland(*context_));
  }

  bool HighDpi() const {
    return context_->EffectiveValue(kHighDpiKey, "false") == "true";
  }

  GtkWidget* BuildSizeRow();
  GtkWidget* BuildWidthRow();
  GtkWidget* BuildHeightRow();
  // Lists the sizes for the current monitor and selects the one in effect.
  void RebuildSizes();

 private:
  struct SizeEntry {
    WindowSize size;
    std::string label;
    std::string description;
    bool custom = false;  // "Custom…"
  };

  bool RememberedUpdatePending() const {
    return chosen_.has_value() && remembered_.found &&
           *chosen_ != RememberedSize() &&
           !context_->draft().IsChanged(kWidthKey) &&
           !context_->draft().IsChanged(kHeightKey);
  }

  void ReadRemembered() {
    std::string error;
    remembered_ = {};
    launcher::ReadRememberedWindowState(context_->window_state_file(),
                                        &remembered_, &error);
  }

  std::string PixelDescription(WindowSize size) const {
    if (!GameUsesWayland(*context_)) {
      return _("In X11 pixels; native resolution has no effect");
    }
    const GameResolution resolution =
        ComputeGameResolution(context_->machine().monitor, size,
                              WindowMode::kWindowed, HighDpi(), true);
    if (resolution.upscaled) {
      return Format(_("Renders %s px; the compositor upscales it"),
                    SizeText(resolution.pixels).c_str());
    }
    return Format(_("Renders %s px"), SizeText(resolution.pixels).c_str());
  }

  void ShowCustomRows(bool show);
  static void OnSizeSelected(GObject* row, GParamSpec*, gpointer data);
  static void SetupSelectedItem(GtkSignalListItemFactory*, GObject* object,
                                gpointer);
  static void BindSelectedItem(GtkSignalListItemFactory*, GObject* object,
                               gpointer data);
  static void SetupListItem(GtkSignalListItemFactory*, GObject* object,
                            gpointer);
  static void BindListItem(GtkSignalListItemFactory*, GObject* object,
                           gpointer data);

  LauncherContext* context_;
  std::shared_ptr<DisplayPageState*> self_ =
      std::make_shared<DisplayPageState*>(this);
  std::vector<LauncherContext::ListenerId> listeners_;
  launcher::RememberedWindowState remembered_;
  std::optional<WindowSize> chosen_;
  GtkWidget* size_row_ = nullptr;
  GtkStringList* size_model_ = nullptr;
  std::vector<SizeEntry> sizes_;
  std::vector<std::string> size_labels_;
  GtkWidget* width_row_ = nullptr;
  GtkWidget* height_row_ = nullptr;
  bool custom_requested_ = false;
  bool updating_ = false;
};

// Window size (research/ux.md 4.3 Display; SPEC 5): presets from the
// window's monitor, written to window.width/height, and to window-state.json
// on Save (LauncherContext::Save and the dirty source above).
GtkWidget* DisplayPageState::BuildSizeRow() {
  GtkWidget* row = adw_combo_row_new();
  size_row_ = row;
  size_model_ = gtk_string_list_new(nullptr);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(size_model_));
  g_object_unref(size_model_);
  GtkExpression* expression =
      gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
  adw_combo_row_set_expression(ADW_COMBO_ROW(row), expression);
  gtk_expression_unref(expression);
  GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(SetupSelectedItem), nullptr);
  g_signal_connect(factory, "bind", G_CALLBACK(BindSelectedItem), this);
  adw_combo_row_set_factory(ADW_COMBO_ROW(row), factory);
  g_object_unref(factory);
  GtkListItemFactory* list_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(list_factory, "setup", G_CALLBACK(SetupListItem), nullptr);
  g_signal_connect(list_factory, "bind", G_CALLBACK(BindListItem), this);
  adw_combo_row_set_list_factory(ADW_COMBO_ROW(row), list_factory);
  g_object_unref(list_factory);
  g_signal_connect(row, "notify::selected", G_CALLBACK(OnSizeSelected), this);

  RowSpec spec;
  // window.width carries the ENV badge and search entry; window.height is
  // handled in the subtitle and keywords.
  spec.key = kWidthKey;
  spec.title = _("Window size");
  spec.keywords = {
      "resolution",       "size",       "width",    "height",
      "windowed",         "разрешение", "размер",   "ширина",
      "высота",           "окно",       kHeightKey, "MOCKTAIL_WIN_HEIGHT",
      "window-state.json"};
  spec.hint.subtitle_for = [this](LauncherContext& ctx, const std::string&) {
    std::vector<std::string> lines;
    switch (Mode()) {
      case WindowMode::kFullscreen:
        lines.push_back(
            _("The game starts fullscreen; this size applies after leaving "
              "it"));
        break;
      case WindowMode::kMaximized:
        lines.push_back(
            _("The game starts maximized; this size applies when it is "
              "restored"));
        break;
      case WindowMode::kWindowed:
        lines.push_back(PixelDescription(EffectiveSize()));
        break;
    }
    if (SizeFromRemembered() && RememberedSize() != ConfigSize()) {
      lines.push_back(Format(_("Remembered from the last session; "
                               "config.yaml says %s"),
                             SizeText(ConfigSize()).c_str()));
    }
    // The binding reports MOCKTAIL_WIN_WIDTH; the height has its own.
    const EnvOverride* height = ctx.EffectiveOverride(kHeightKey);
    if (height != nullptr && ctx.EffectiveOverride(kWidthKey) == nullptr) {
      lines.push_back(Format(_("This launch uses %s=%s instead"),
                             height->name.c_str(),
                             RedactEnvironmentValue(height->value).c_str()));
    }
    std::string text;
    for (const std::string& line : lines) {
      if (!text.empty()) text += '\n';
      text += line;
    }
    return text;
  };
  spec.hint.details =
      // research/graphics.md 2.1: SDL logical units on Wayland.
      _("The size of the game window while it is neither fullscreen nor "
        "maximized, in desktop units: on a screen scaled to 160 %, 1600 × 900 "
        "covers a 2560 × 1440 panel.") +
      std::string("\n\n") +
      // window.cc restores window-state.json over window.width/height;
      // LauncherContext::Save updates it.
      _("The game remembers the size its window had when it was last closed "
        "and uses that instead of config.yaml. Saving a size here updates "
        "both, so the next start uses it; resizing the game window changes "
        "the remembered size again.") +
      "\n\n" +
      _("Tiling compositors such as Hyprland, niri, Sway or river usually "
        "size windows themselves, so there the size matters only for a "
        "floating window.") +
      "\n\n" +
      // research/graphics.md 2.1: XWayland with force_zero_scaling.
      _("Through X11 (XWayland) the size is in X pixels, which on a scaled "
        "screen may be physical pixels, depending on how the desktop scales "
        "XWayland.");
  spec.hint.details_for = [this](LauncherContext& ctx) {
    std::string text;
    const MonitorInfo& monitor = ctx.machine().monitor;
    if (monitor.valid) {
      text += Format(
          _("This screen: %s desktop units at %d %% (%s pixels)."),
          SizeText({monitor.width, monitor.height}).c_str(),
          Percent(monitor.scale),
          SizeText({monitor.PixelWidth(), monitor.PixelHeight()}).c_str());
      text += "\n\n";
    }
    text += remembered_.found
                ? Format(_("Remembered size: %s."),
                         SizeText(RememberedSize()).c_str())
                : std::string(_("Nothing is remembered yet, so the first "
                                "window uses the size from config.yaml."));
    return text;
  };
  DecorateRow(context_, row, std::move(spec));
  return row;
}

GtkWidget* DisplayPageState::BuildWidthRow() {
  RowSpec spec;
  spec.key = kWidthKey;
  spec.title = _("Width");
  spec.keywords = {"window width", "ширина окна"};
  spec.hint.subtitle = _("Desktop units");
  spec.hint.details =
      _("The window width in desktop units, from 160 to 16384. Saving also "
        "updates the size the game remembers.");
  SpinSpec spin;
  spin.minimum = kMinimumWindowWidth;
  spin.maximum = kMaximumWindowExtent;
  spin.step = 10;
  width_row_ = BindSpinRow(context_, std::move(spec), spin);
  gtk_widget_set_visible(width_row_, FALSE);
  return width_row_;
}

GtkWidget* DisplayPageState::BuildHeightRow() {
  RowSpec spec;
  spec.key = kHeightKey;
  spec.title = _("Height");
  spec.keywords = {"window height", "высота окна"};
  spec.hint.subtitle = _("Desktop units");
  spec.hint.details =
      _("The window height in desktop units, from 120 to 16384. Saving also "
        "updates the size the game remembers.");
  SpinSpec spin;
  spin.minimum = kMinimumWindowHeight;
  spin.maximum = kMaximumWindowExtent;
  spin.step = 10;
  height_row_ = BindSpinRow(context_, std::move(spec), spin);
  gtk_widget_set_visible(height_row_, FALSE);
  return height_row_;
}

void DisplayPageState::RebuildSizes() {
  if (size_row_ == nullptr) return;
  const WindowSize current = EffectiveSize();
  std::vector<SizeEntry> sizes;
  int selected = -1;
  for (const WindowSizePreset& preset :
       WindowSizePresets(context_->machine().monitor)) {
    SizeEntry entry;
    entry.size = preset.size;
    entry.label = preset.whole_screen ? Format(_("%s (whole screen)"),
                                               SizeText(preset.size).c_str())
                  : preset.runtime_default
                      ? Format(_("%s (default)"), SizeText(preset.size).c_str())
                      : SizeText(preset.size);
    entry.description = PixelDescription(preset.size);
    if (preset.size == current && selected < 0) {
      selected = static_cast<int>(sizes.size());
    }
    sizes.push_back(std::move(entry));
  }
  const bool listed = selected >= 0;
  if (!listed) {
    SizeEntry entry;
    entry.size = current;
    if (SizeFromRemembered()) {
      entry.label = Format(_("%s (last used)"), SizeText(current).c_str());
      entry.description =
          _("The size the game window had when it was last closed");
    } else {
      entry.label = Format(_("%s (custom)"), SizeText(current).c_str());
      entry.description = PixelDescription(current);
    }
    selected = static_cast<int>(sizes.size());
    sizes.push_back(std::move(entry));
  }
  SizeEntry custom;
  custom.label = _("Custom…");
  custom.description = _("Enter a width and height");
  custom.custom = true;
  sizes.push_back(std::move(custom));

  std::vector<std::string> labels;
  for (const SizeEntry& entry : sizes) labels.push_back(entry.label);
  updating_ = true;
  sizes_ = std::move(sizes);
  if (labels != size_labels_) {
    size_labels_ = labels;
    std::vector<const char*> pointers;
    for (const std::string& label : size_labels_) {
      pointers.push_back(label.c_str());
    }
    pointers.push_back(nullptr);
    gtk_string_list_splice(size_model_, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(size_model_)),
                           pointers.data());
  }
  if (adw_combo_row_get_selected(ADW_COMBO_ROW(size_row_)) !=
      static_cast<guint>(selected)) {
    adw_combo_row_set_selected(ADW_COMBO_ROW(size_row_),
                               static_cast<guint>(selected));
  }
  updating_ = false;
  // The number rows show config.yaml, so they appear after "Custom…", or
  // when config.yaml holds the size in effect and no option lists it.
  ShowCustomRows(custom_requested_ ||
                 (!listed && EffectiveSize() == ConfigSize()));
}

void DisplayPageState::ShowCustomRows(bool show) {
  if (width_row_ != nullptr) gtk_widget_set_visible(width_row_, show);
  if (height_row_ != nullptr) gtk_widget_set_visible(height_row_, show);
}

void DisplayPageState::OnSizeSelected(GObject* row, GParamSpec*,
                                      gpointer data) {
  auto* self = static_cast<DisplayPageState*>(data);
  if (self->updating_) return;
  const guint position = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  if (position >= self->sizes_.size()) return;
  const SizeEntry entry = self->sizes_[position];
  // The size in effect: nothing changes. AdwComboRow also reports the
  // selection RebuildSizes() restores after "Custom…" once the handler
  // returns, which must not hide the number rows again.
  if (!entry.custom && entry.size == self->EffectiveSize()) return;
  LauncherContext* context = self->context_;
  // Also when config.yaml already holds the size: the remembered one may
  // differ and is replaced on Save.
  const WindowSize size = entry.custom ? self->EffectiveSize() : entry.size;
  self->custom_requested_ = entry.custom;
  context->SetValue(kWidthKey, std::to_string(size.width),
                    launcher::ScalarKind::kInteger);
  context->SetValue(kHeightKey, std::to_string(size.height),
                    launcher::ScalarKind::kInteger);
  self->chosen_ = size;
  context->NotifyDirtyChanged();
  self->RebuildSizes();
  if (entry.custom && self->width_row_ != nullptr) {
    context->Reveal(self->width_row_);
  }
}

void DisplayPageState::SetupSelectedItem(GtkSignalListItemFactory*,
                                         GObject* object, gpointer) {
  GtkWidget* label = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
  gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
}

// The row shows only the size; the list says where it comes from (whole
// screen, default, last used), which would crowd a narrow window.
void DisplayPageState::BindSelectedItem(GtkSignalListItemFactory*,
                                        GObject* object, gpointer data) {
  auto* self = static_cast<DisplayPageState*>(data);
  GtkListItem* item = GTK_LIST_ITEM(object);
  GtkStringObject* string = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
  std::string text = string != nullptr ? gtk_string_object_get_string(string)
                                       : std::string();
  // By label, not position: the row's own display item does not always
  // carry the selected position. Labels are unique.
  for (const SizeEntry& entry : self->sizes_) {
    if (entry.label == text && !entry.custom) {
      text = SizeText(entry.size);
      break;
    }
  }
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), text.c_str());
}

void DisplayPageState::SetupListItem(GtkSignalListItemFactory*, GObject* object,
                                     gpointer) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "combo-option");
  GtkWidget* title = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
  gtk_box_append(GTK_BOX(box), title);
  GtkWidget* description = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(description), 0.0F);
  gtk_label_set_wrap(GTK_LABEL(description), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(description), 40);
  gtk_widget_add_css_class(description, "dim-label");
  gtk_widget_add_css_class(description, "caption");
  gtk_box_append(GTK_BOX(box), description);
  gtk_list_item_set_child(GTK_LIST_ITEM(object), box);
}

void DisplayPageState::BindListItem(GtkSignalListItemFactory*, GObject* object,
                                    gpointer data) {
  auto* self = static_cast<DisplayPageState*>(data);
  GtkListItem* item = GTK_LIST_ITEM(object);
  GtkWidget* box = gtk_list_item_get_child(item);
  GtkWidget* title = gtk_widget_get_first_child(box);
  GtkWidget* description = gtk_widget_get_next_sibling(title);
  const guint position = gtk_list_item_get_position(item);
  if (position >= self->sizes_.size()) return;
  const SizeEntry& entry = self->sizes_[position];
  gtk_label_set_text(GTK_LABEL(title), entry.label.c_str());
  gtk_label_set_text(GTK_LABEL(description), entry.description.c_str());
  gtk_widget_set_visible(description, !entry.description.empty());
}

// display.start_mode (SPEC 2; window.cc ApplyConfiguredWindowStartMode).
GtkWidget* BuildStartModeRow(LauncherContext* context,
                             DisplayPageState* state) {
  RowSpec spec;
  spec.key = kStartModeKey;
  spec.title = _("Start in");
  spec.fallback = "remember";
  spec.keywords = {"fullscreen", "windowed",     "maximized", "start mode",
                   "f11",        "полный экран", "окно",      "развёрнутым",
                   "режим",      "запуск"};
  spec.hint.details =
      _("How the game window opens. The game still remembers the mode each "
        "session ends in, and F11 or Roblox's own fullscreen setting switch "
        "while you play.") +
      std::string("\n\n") +
      // research/graphics.md 2.4: borderless desktop fullscreen.
      _("Fullscreen covers the screen at its own resolution: it is not an "
        "exclusive video mode, so switching is quick. Native resolution on "
        "scaled displays decides how many pixels Roblox renders there.") +
      "\n\n" +
      _("Tiling compositors may still tile a window that starts windowed or "
        "maximized.");
  ComboSpec combo;
  combo.options = {
      {"remember",
       _("Last used mode"),
       {},
       {},
       [state](LauncherContext&) {
         const launcher::RememberedWindowState& remembered =
             state->remembered();
         if (!remembered.found) {
           return std::string(
               _("As the last session ended; a window the first time"));
         }
         return remembered.fullscreen
                    ? std::string(_("As the last session ended: fullscreen"))
                : remembered.maximized
                    ? std::string(_("As the last session ended: maximized"))
                    : std::string(_("As the last session ended: in a window"));
       },
       nullptr,
       false,
       false},
      {"windowed",
       _("Windowed"),
       _("A normal window of the size below"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"maximized",
       _("Maximized"),
       _("Fills the screen around panels and docks"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      // window.cc: F11 toggles fullscreen unless MOCKTAIL_F11_FULLSCREEN=0.
      {"fullscreen",
       _("Fullscreen"),
       _("Covers the whole screen; F11 switches back to a window"),
       {},
       nullptr,
       nullptr,
       false,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

// window.high_dpi (window_creation_policy.cc; research/graphics.md 2.2).
GtkWidget* BuildHighDpiRow(LauncherContext* context, DisplayPageState* state) {
  RowSpec spec;
  spec.key = kHighDpiKey;
  spec.title = _("Native resolution on scaled displays");
  spec.keywords = {"hidpi",          "high dpi", "dpi",    "scale",
                   "scaling",        "sharp",    "blurry", "retina",
                   "масштаб",        "чёткость", "чёткое", "размытое",
                   "масштабирование"};
  spec.hint.subtitle_for = [state](LauncherContext& ctx,
                                   const std::string& value) {
    if (!GameUsesWayland(ctx)) {
      return std::string(_("No effect through X11 (XWayland)"));
    }
    const MonitorInfo& monitor = ctx.machine().monitor;
    if (!monitor.valid) {
      return std::string(
          _("Renders every pixel of a scaled screen instead of upscaling"));
    }
    if (monitor.scale <= 1.0 + 1e-6) {
      return Format(_("No effect at %d %% scale"), Percent(monitor.scale));
    }
    const GameResolution resolution = state->Resolution(value == "true");
    if (value == "true") {
      return Format(_("Scale %d %%: renders all %s pixels"),
                    Percent(monitor.scale),
                    SizeText(resolution.pixels).c_str());
    }
    return Format(_("Scale %d %%: renders %s, upscaled by the compositor"),
                  Percent(monitor.scale), SizeText(resolution.pixels).c_str());
  };
  spec.hint.details =
      // SDL_WINDOW_HIGH_PIXEL_DENSITY; Roblox's density follows the
      // window's display scale (window.cc, research/graphics.md 2.2).
      _("On a screen scaled above 100 %, this decides whether Roblox renders "
        "every pixel of the screen or the smaller desktop size, which the "
        "compositor then stretches. Roblox's menus keep the same size either "
        "way.") +
      std::string("\n\n") +
      _("• On: sharp text and edges, but the graphics card draws the scale "
        "squared times as many pixels: 2.56 times as many at 160 %.") +
      "\n\n" +
      _("• Off: fewer pixels to draw, a softer picture and usually a higher "
        "frame rate.") +
      "\n\n" +
      _("It works only with Wayland. Through X11 (XWayland) the game gets no "
        "pixel density, so this setting changes nothing there.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MonitorInfo& monitor = ctx.machine().monitor;
    if (!monitor.valid) return std::string();
    return Format(
        _("This screen: %s desktop units at %d %%, %s pixels."),
        SizeText({monitor.width, monitor.height}).c_str(),
        Percent(monitor.scale),
        SizeText({monitor.PixelWidth(), monitor.PixelHeight()}).c_str());
  };
  spec.hint.recommend = [context](const MachineProfile& machine) {
    return RecommendHighDpi(machine, GameUsesWayland(*context));
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    if (machine.detected && machine.gpu.intel_only()) {
      return std::string(
          _("With Intel graphics only, the extra pixels cost more frame rate "
            "than the sharper picture is worth."));
    }
    return Format(_("Your screen is scaled to %d %%, and this keeps the "
                    "picture as sharp as the screen allows."),
                  Percent(machine.monitor.scale));
  };
  return BindSwitchRow(context, std::move(spec));
}

std::string ResolutionSummary(const LauncherContext& context,
                              const DisplayPageState& state) {
  const MonitorInfo& monitor = context.machine().monitor;
  const WindowMode mode = state.Mode();
  const GameResolution resolution = state.Resolution(state.HighDpi());
  if (!GameUsesWayland(context)) {
    switch (mode) {
      case WindowMode::kFullscreen:
        return _("Fullscreen through X11, at the X server's screen size");
      case WindowMode::kMaximized:
        return _("Maximized through X11, within the X server's screen");
      case WindowMode::kWindowed:
        return Format(_("A %s window through X11"),
                      SizeText(resolution.logical).c_str());
    }
  }
  if (mode != WindowMode::kWindowed && !monitor.valid) {
    return mode == WindowMode::kFullscreen
               ? std::string(_("Fullscreen at the screen's own size"))
               : std::string(_("Maximized to the screen's free area"));
  }
  const std::string pixels = SizeText(resolution.pixels);
  switch (mode) {
    case WindowMode::kFullscreen:
      if (resolution.upscaled) {
        return Format(
            _("%s px fullscreen, upscaled to %s"), pixels.c_str(),
            SizeText({monitor.PixelWidth(), monitor.PixelHeight()}).c_str());
      }
      return Format(_("%s px fullscreen, sharp"), pixels.c_str());
    case WindowMode::kMaximized:
      return resolution.upscaled
                 ? Format(_("Up to %s px maximized, upscaled"), pixels.c_str())
                 : Format(_("Up to %s px maximized"), pixels.c_str());
    case WindowMode::kWindowed:
      if (resolution.upscaled) {
        return Format(_("%s px in a window, upscaled"), pixels.c_str());
      }
      return resolution.pixels == resolution.logical
                 ? Format(_("%s px in a window"), pixels.c_str())
                 : Format(_("%s px in a %s window"), pixels.c_str(),
                          SizeText(resolution.logical).c_str());
  }
  return {};
}

// Read-only summary (SPEC 5: monitor geometry x scale x high_dpi x start
// mode).
GtkWidget* BuildResolutionRow(LauncherContext* context,
                              DisplayPageState* state) {
  GtkWidget* row = adw_action_row_new();
  RowSpec spec;
  spec.title = _("Game resolution");
  spec.keywords = {"resolution", "pixels", "render", "разрешение", "пиксели"};
  spec.hint.subtitle_for = [state](LauncherContext& ctx, const std::string&) {
    return ResolutionSummary(ctx, *state);
  };
  spec.hint.details =
      _("What Roblox renders with the settings above, worked out from this "
        "screen's desktop size and scale, the window size, the start mode "
        "and native resolution. Roblox lays out its interface at the desktop "
        "size; this is how many pixels it draws.") +
      std::string("\n\n") +
      // research/graphics.md 1.2 and 2.5; SPEC 6 non-goals.
      _("A lower fullscreen resolution, as games offer on Windows, is not "
        "available: fullscreen always covers the screen at its own size. To "
        "draw fewer pixels on a scaled screen, turn native resolution off.") +
      "\n\n" +
      _("A maximized window loses the space of panels and docks, which this "
        "window cannot see, so its size is an upper bound.");
  return DecorateRow(context, row, std::move(spec));
}

std::vector<ComboOption> DisplayServerOptions() {
  return {
      {"auto",
       _("Automatic"),
       {},
       {},
       [](LauncherContext& ctx) {
         switch (ServerChoice(ctx, "auto").reason) {
           case DisplayServerReason::kWaylandSession:
             return std::string(_("Uses Wayland here"));
           case DisplayServerReason::kNvidiaVulkan:
             return std::string(
                 _("Uses X11 (XWayland) here: NVIDIA with Vulkan"));
           case DisplayServerReason::kX11Only:
             return std::string(
                 _("Uses X11 here: there is no Wayland session"));
           case DisplayServerReason::kChosen:
           case DisplayServerReason::kChosenUnavailable:
           case DisplayServerReason::kUnknown:
             break;
         }
         return std::string(
             _("Wayland when available; X11 (XWayland) for NVIDIA with "
               "Vulkan"));
       },
       nullptr,
       false,
       false},
      {"wayland",
       "Wayland",
       {},
       {},
       [](LauncherContext& ctx) {
         if (ctx.machine().gpu.nvidia_kernel_driver &&
             IsVulkanSpelling(Backend(ctx))) {
           return std::string(
               _("Native Wayland; with NVIDIA and Vulkan it has hung for some "
                 "users"));
         }
         return std::string(
             _("Native Wayland: sharp on scaled screens with native "
               "resolution"));
       },
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         return machine.detected && !machine.wayland_available
                    ? std::string(_("No Wayland session: WAYLAND_DISPLAY is "
                                    "not set"))
                    : std::string();
       },
       false,
       false},
      {"x11",
       _("X11 (XWayland)"),
       _("Through an X server; native resolution has no effect"),
       {},
       nullptr,
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         return machine.detected && !machine.x11_available
                    ? std::string(_("No X server: DISPLAY is not set"))
                    : std::string();
       },
       false,
       false},
  };
}

// display.server (SPEC 2; graphics_launch_policy.cc ApplyDisplayServer,
// video_driver_policy.cc; research/graphics.md 3).
GtkWidget* BuildDisplayServerRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kServerKey;
  spec.title = _("Display server");
  spec.fallback = "auto";
  spec.keywords = {"wayland",     "x11",    "xwayland", "sdl",
                   "videodriver", "driver", "сервер",   "иксы"};
  spec.hint.details =
      _("Whether the game window talks to your desktop through Wayland or "
        "through X11, which on a Wayland desktop means XWayland. The change "
        "applies the next time Roblox starts, also to the test runs of "
        "automatic updates.") +
      std::string("\n\n") +
      // video_driver_policy.h: NVIDIA native Wayland WSI hangs and
      // explicit-sync errors (upstream issue #186).
      _("• Automatic: Wayland when the desktop offers it. The exception is "
        "NVIDIA's driver with the Vulkan backend: then XWayland, because "
        "NVIDIA's native Wayland path has hung and lost the display (upstream "
        "issue #186).") +
      "\n\n" +
      // research/graphics.md 3.3: pointer capture problems through XWayland
      // (upstream issue #135).
      _("• Wayland: the native path, sharp on scaled screens with native "
        "resolution. On NVIDIA with Vulkan it is the riskier choice, but "
        "worth a try when the mouse or camera misbehaves through XWayland.") +
      "\n\n" +
      _("• X11 (XWayland): works with every driver, but the game gets no "
        "pixel density, so native resolution has no effect and the picture "
        "may look small or soft on a scaled screen.") +
      "\n\n" +
      // graphics_launch_policy.cc UserSelectsVideoDriver.
      _("SDL_VIDEODRIVER or SDL_VIDEO_DRIVER set in the shortcut or terminal "
        "always win over this setting.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MachineProfile& machine = ctx.machine();
    if (!machine.detected) return std::string();
    std::string text;
    if (machine.wayland_available && machine.x11_available) {
      text = _("This session offers Wayland and X11 (XWayland).");
    } else if (machine.wayland_available) {
      text = _("This session offers Wayland only, without an X server.");
    } else if (machine.x11_available) {
      text = _("This session offers X11 only.");
    } else {
      text = _("This session offers no display server to the game.");
    }
    if (machine.gpu.nvidia_kernel_driver) {
      text += " ";
      text += _("NVIDIA's kernel driver is loaded.");
    }
    return text;
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return machine.detected ? std::optional<std::string>("auto") : std::nullopt;
  };
  spec.hint.recommend_reason = [context](const MachineProfile&) {
    if (ServerChoice(*context, "auto").reason ==
        DisplayServerReason::kNvidiaVulkan) {
      return std::string(
          _("It avoids NVIDIA's Wayland hangs; choose Wayland only if XWayland "
            "gives you trouble."));
    }
    return std::string(
        _("It already picks the best server this session offers."));
  };
  spec.hint.warning = [](LauncherContext& ctx, const std::string& value) {
    const DisplayServerChoice choice = ServerChoice(ctx, value);
    if (choice.reason == DisplayServerReason::kChosenUnavailable) {
      return value == "wayland"
                 ? std::string(_("This session has no Wayland, so the game "
                                 "uses the automatic choice."))
                 : std::string(_("This session has no X server, so the game "
                                 "uses the automatic choice."));
    }
    if (choice.server == "wayland" && value == "wayland" &&
        ctx.machine().gpu.nvidia_kernel_driver &&
        IsVulkanSpelling(Backend(ctx))) {
      return std::string(
          _("NVIDIA's Wayland path for Vulkan has hung or lost the display for "
            "some users (upstream issue #186). If the game freezes or closes, "
            "go back to Automatic."));
    }
    return std::string();
  };
  ComboSpec combo;
  combo.options = DisplayServerOptions();
  return BindComboRow(context, std::move(spec), std::move(combo));
}

// appearance.theme (legacy_runtime.cc theme selection; research/graphics.md
// 6.2).
GtkWidget* BuildThemeRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "appearance.theme";
  spec.title = _("Theme");
  spec.fallback = "roblox";
  spec.keywords = {"theme", "dark",   "light",   "appearance",
                   "тема",  "тёмная", "светлая", "оформление"};
  spec.hint.details =
      _("The theme of Roblox's own menus and screens, chosen when the game "
        "starts.") +
      std::string("\n\n") +
      // legacy_runtime.cc: ReadRobloxThemeCache, dark when none.
      _("• From your Roblox account: the theme saved in your Roblox settings, "
        "read from Roblox's local data for the signed-in account; dark when "
        "none is saved yet.") +
      "\n\n" +
      // legacy_runtime.cc: SDL_GetSystemTheme(); unknown counts as light.
      _("• Follow the system: your desktop's light or dark preference as SDL "
        "reports it at start; light when the desktop reports none.") +
      "\n\n" + _("• Light and Dark: always that theme.") + "\n\n" +
      // legacy_runtime.cc: ApplyRobloxThemeCacheOverride runs for every
      // mode but roblox.
      _("With any choice except “From your Roblox account”, Mocktail also "
        "writes the resulting theme into Roblox's local settings for the "
        "signed-in account at every start.");
  ComboSpec combo;
  combo.options = {
      {"roblox",
       _("From your Roblox account"),
       _("The theme saved in your Roblox settings; dark until one is saved"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"system",
       _("Follow the system"),
       _("Light or dark, as your desktop prefers when the game starts"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"light",
       _("Light"),
       _("Always light"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"dark", _("Dark"), _("Always dark"), {}, nullptr, nullptr, false, false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

// window.title, an advanced window option (research/ux.md 4.3 [ADV]).
GtkWidget* BuildWindowTitleRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "window.title";
  spec.title = _("Window title");
  spec.keywords = {"title", "name", "заголовок", "название"};
  spec.hint.subtitle = _("Shown in taskbars and window switchers");
  spec.hint.details =
      _("The title of the game window, shown in taskbars, docks and window "
        "switchers. Window rules of some compositors match on it, so change "
        "it only together with such rules.") +
      std::string("\n\n") + _("It cannot be empty; the default is Roblox.");
  EntrySpec entry;
  entry.validate = [](const std::string& text) {
    // runtime_config_file.cc: window.title must not be empty.
    return text.empty() ? std::string(_("The title cannot be empty"))
                        : std::string();
  };
  return BindEntryRow(context, std::move(spec), std::move(entry));
}

}  // namespace

// Display: the game window, sharpness on scaled screens, the display server
// and the Roblox theme.
GtkWidget* BuildDisplayPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kDisplay,
              _("The game window: its size and start mode, sharpness on scaled "
                "screens, the display server and the Roblox theme"));
  auto* state = new DisplayPageState(context);
  g_object_set_data_full(
      G_OBJECT(page), "mocktail-display-page", state,
      [](gpointer data) { delete static_cast<DisplayPageState*>(data); });

  GtkWidget* window = AddGroup(
      page, _("Window"), _("How the game window opens and how large it is"));
  AddRow(window, BuildStartModeRow(context, state));
  AddRow(window, state->BuildSizeRow());
  AddRow(window, state->BuildWidthRow());
  AddRow(window, state->BuildHeightRow());
  state->RebuildSizes();
  GtkWidget* advanced = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(advanced),
                                _("More window options"));
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(advanced), _("Window title"));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(advanced),
                           BuildWindowTitleRow(context));
  AddRow(window, advanced);

  GtkWidget* scaling =
      AddGroup(page, _("Scaling"),
               _("Sharpness and pixel count on screens scaled above 100 %"));
  AddRow(scaling, BuildHighDpiRow(context, state));
  AddRow(scaling, BuildResolutionRow(context, state));

  GtkWidget* server =
      AddGroup(page, _("Display server"),
               _("Whether the game window uses Wayland or X11"));
  AddRow(server, BuildDisplayServerRow(context));

  GtkWidget* interface =
      AddGroup(page, _("Roblox interface"), _("How Roblox's own menus look"));
  AddRow(interface, BuildThemeRow(context));
  return page;
}

}  // namespace mocktail::launcher_ui
