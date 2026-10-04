#include "launcher_ui/bindings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/i18n.h"
#include "launcher_ui/setting_kinds.h"
#include "runtime/managed_environment.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kBindingKey[] = "mocktail-binding";
constexpr int kPopoverWidthChars = 52;

std::vector<std::string> SplitParagraphs(const std::string& text) {
  std::vector<std::string> paragraphs;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find("\n\n", start);
    if (end == std::string::npos) end = text.size();
    std::string paragraph = text.substr(start, end - start);
    while (!paragraph.empty() && paragraph.back() == '\n') paragraph.pop_back();
    if (!paragraph.empty()) paragraphs.push_back(std::move(paragraph));
    start = end + 2;
  }
  return paragraphs;
}

GtkWidget* NewWrappedLabel(const std::string& text, bool selectable) {
  GtkWidget* label = gtk_label_new(text.c_str());
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_label_set_max_width_chars(GTK_LABEL(label), kPopoverWidthChars);
  gtk_label_set_width_chars(GTK_LABEL(label), 24);
  gtk_label_set_natural_wrap_mode(GTK_LABEL(label), GTK_NATURAL_WRAP_WORD);
  gtk_label_set_selectable(GTK_LABEL(label), selectable);
  return label;
}

std::vector<std::string> OverridingVariables(std::string_view key) {
  std::vector<std::string> names;
  if (key.empty()) return names;
  for (const runtime::ManagedEnvironmentVariable& variable :
       runtime::ManagedEnvironmentVariables()) {
    if (variable.yaml_key == key) names.emplace_back(variable.name);
  }
  return names;
}

std::optional<double> ParseNumber(const std::string& text) {
  long long value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return static_cast<double>(value);
}

// Selectable labels select all their text when the popover focuses them;
// start with nothing selected.
void ClearSelections(GtkWidget* widget) {
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_LABEL(child) && gtk_label_get_selectable(GTK_LABEL(child))) {
      gtk_label_select_region(GTK_LABEL(child), 0, 0);
    }
    ClearSelections(child);
  }
}

void OnHintPopoverShown(GtkWidget* popover, gpointer) {
  g_idle_add(
      [](gpointer data) -> gboolean {
        ClearSelections(GTK_WIDGET(data));
        g_object_unref(data);
        return G_SOURCE_REMOVE;
      },
      g_object_ref(popover));
}

// ---- RowBinding
// ------------------------------------------------------------------

// The state behind one bound row. Owned by the row widget.
class RowBinding {
 public:
  RowBinding(LauncherContext* context, RowSpec spec)
      : context_(context), spec_(std::move(spec)) {}
  virtual ~RowBinding() {
    for (const LauncherContext::ListenerId id : listeners_) {
      context_->RemoveListener(id);
    }
    if (!spec_.key.empty()) context_->SetProblem(this, {});
  }

  // The value the game reads from config.yaml.
  std::string Effective() const {
    return spec_.key.empty()
               ? std::string()
               : context_->EffectiveValue(spec_.key, spec_.fallback);
  }
  // The value without anything in config.yaml.
  std::string Default() const {
    const std::optional<std::string> value =
        context_->draft().TemplateValue(spec_.key);
    return value.has_value() ? *value : spec_.fallback;
  }

  // Attaches the decorations and starts following the context.
  void Attach(GtkWidget* row, RowKind kind) {
    row_ = row;
    kind_ = kind;
    g_object_set_data_full(G_OBJECT(row), kBindingKey, this, [](gpointer data) {
      delete static_cast<RowBinding*>(data);
    });
    // Titles and subtitles are markup (escaped with Markup()); combo rows
    // default to plain text, so set it for every row.
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), TRUE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  Markup(spec_.title).c_str());
    if (ADW_IS_ACTION_ROW(row)) {
      adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
    }
    env_badge_ = NewEnvBadge();
    gtk_widget_set_visible(env_badge_, FALSE);
    AddSuffix(env_badge_);
    if (!spec_.key.empty() && spec_.resettable) {
      reset_button_ = gtk_button_new_from_icon_name("edit-undo-symbolic");
      gtk_widget_add_css_class(reset_button_, "flat");
      gtk_widget_add_css_class(reset_button_, "circular");
      gtk_widget_set_valign(reset_button_, GTK_ALIGN_CENTER);
      gtk_widget_set_visible(reset_button_, FALSE);
      gtk_accessible_update_property(
          GTK_ACCESSIBLE(reset_button_), GTK_ACCESSIBLE_PROPERTY_LABEL,
          Format(_("Reset %s to default"), spec_.title.c_str()).c_str(), -1);
      g_signal_connect(reset_button_, "clicked", G_CALLBACK(OnResetClicked),
                       this);
      AddSuffix(reset_button_);
    }
    AddSuffix(NewInfoButton());

    std::vector<std::string> keywords = spec_.keywords;
    if (!spec_.key.empty()) {
      keywords.push_back(spec_.key);
      for (std::string& name : OverridingVariables(spec_.key)) {
        keywords.push_back(std::move(name));
      }
    }
    RowRecord record;
    record.row = row;
    record.kind = kind;
    record.key = spec_.key;
    record.title = spec_.title;
    record.has_subtitle = !spec_.hint.subtitle.empty() ||
                          static_cast<bool>(spec_.hint.subtitle_for) ||
                          kind == RowKind::kCombo;
    record.has_details = !spec_.hint.details.empty();
    search_id_ =
        context_->RegisterRow(std::move(record), std::move(keywords), {});

    listeners_.push_back(
        context_->OnSettingChanged([this](std::string_view key) {
          // Discard, reload or an import: drop half-typed input as well.
          if (key.empty()) ResetInput();
          Refresh();
        }));
    listeners_.push_back(context_->OnMachineChanged([this] { Refresh(); }));
    Refresh();
  }

  // Updates the control, subtitle, badges and reset button.
  void Refresh() {
    const std::string value = Effective();
    updating_ = true;
    if (!spec_.key.empty()) SyncControl(value);
    updating_ = false;
    const std::string unavailable =
        spec_.unavailable ? spec_.unavailable(*context_) : std::string();
    if (!spec_.key.empty() || spec_.unavailable) {
      gtk_widget_set_sensitive(
          row_,
          unavailable.empty() && (spec_.key.empty() || !context_->read_only()));
    }

    std::string base = spec_.hint.subtitle_for
                           ? spec_.hint.subtitle_for(*context_, value)
                           : BaseSubtitle(value);
    std::vector<std::string> lines;
    std::string recommendation_line;
    if (spec_.hint.recommend && context_->machine().detected) {
      const std::optional<std::string> recommended =
          spec_.hint.recommend(context_->machine());
      if (recommended.has_value() && *recommended == value) {
        base += base.empty() ? "" : " · ";
        base += _("Recommended for this computer");
      } else if (recommended.has_value()) {
        recommendation_line = Format(_("Recommended for this computer: %s"),
                                     LabelFor(*recommended).c_str());
      }
    }
    if (!base.empty()) lines.push_back(base);
    if (!recommendation_line.empty()) lines.push_back(recommendation_line);
    const EnvOverride* env = context_->EffectiveOverride(spec_.key);
    if (env != nullptr) {
      lines.push_back(Format(_("This launch uses %s=%s instead"),
                             env->name.c_str(),
                             RedactEnvironmentValue(env->value).c_str()));
    }
    if (!unavailable.empty()) lines.push_back(unavailable);
    const std::string warning =
        spec_.hint.warning ? spec_.hint.warning(*context_, value) : "";

    std::string markup;
    for (const std::string& line : lines) {
      if (!markup.empty()) markup += '\n';
      markup += Markup(line);
    }
    if (!warning.empty()) {
      if (!markup.empty()) markup += '\n';
      markup += "<span foreground=\"" + context_->warning_color() + "\">" +
                Markup(warning) + "</span>";
    }
    SetSubtitle(markup);
    if (warning.empty()) {
      gtk_widget_remove_css_class(row_, "has-warning");
    } else {
      gtk_widget_add_css_class(row_, "has-warning");
    }

    gtk_widget_set_visible(env_badge_, env != nullptr);
    if (env != nullptr) {
      gtk_widget_set_tooltip_text(
          env_badge_,
          Format(_("Set by %s=%s: this launch uses that value instead of "
                   "config.yaml"),
                 env->name.c_str(), RedactEnvironmentValue(env->value).c_str())
              .c_str());
    }
    if (reset_button_ != nullptr) {
      const std::string default_value = Default();
      gtk_widget_set_visible(reset_button_,
                             !context_->read_only() && value != default_value);
      gtk_widget_set_tooltip_text(
          reset_button_,
          Format(_("Reset to default (%s)"), LabelFor(default_value).c_str())
              .c_str());
    }
    context_->UpdateRowSubtitle(search_id_, base);
  }

 protected:
  // Shows `value` in the control without writing it back.
  virtual void SyncControl(const std::string& value) = 0;
  // Forgets input that was never written (an invalid entry).
  virtual void ResetInput() {}
  // The subtitle when the hint has no subtitle_for.
  virtual std::string BaseSubtitle(const std::string& value) const {
    (void)value;
    return spec_.hint.subtitle;
  }
  // How a value reads in the UI (option labels, On/Off).
  virtual std::string LabelFor(const std::string& value) const { return value; }
  virtual void AddSuffix(GtkWidget* widget) {
    if (ADW_IS_ENTRY_ROW(row_)) {
      adw_entry_row_add_suffix(ADW_ENTRY_ROW(row_), widget);
    } else if (ADW_IS_ACTION_ROW(row_)) {
      adw_action_row_add_suffix(ADW_ACTION_ROW(row_), widget);
    }
  }
  virtual void SetSubtitle(const std::string& markup) {
    if (ADW_IS_ACTION_ROW(row_)) {
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row_), markup.c_str());
    }
  }

  // Writes a value chosen in the control.
  void Write(const std::string& value, launcher::ScalarKind kind) {
    if (updating_) return;
    if (!context_->SetValue(spec_.key, value, kind)) {
      Refresh();  // put the control back
    }
  }

  LauncherContext* context_;
  RowSpec spec_;
  GtkWidget* row_ = nullptr;
  bool updating_ = false;

 private:
  static void OnResetClicked(GtkButton*, gpointer data) {
    auto* binding = static_cast<RowBinding*>(data);
    LauncherContext* context = binding->context_;
    const std::string key = binding->spec_.key;
    const std::optional<std::string> previous = context->Value(key);
    const std::string title = binding->spec_.title;
    if (!context->ResetValue(key)) return;
    context->Toast(Format(_("“%s” reset to default"), title.c_str()), _("Undo"),
                   [context, key, previous] {
                     if (previous.has_value()) {
                       context->SetValue(key, *previous);
                     } else {
                       context->UnsetValue(key);
                     }
                   });
  }

  GtkWidget* NewInfoButton() {
    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button),
                                  "dialog-information-symbolic");
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "circular");
    gtk_widget_add_css_class(button, "info-button");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, _("Learn more"));
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
        Format(_("Learn more about %s"), spec_.title.c_str()).c_str(), -1);
    GtkWidget* popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "hint-popover");
    g_signal_connect(popover, "show", G_CALLBACK(OnHintPopoverShown), nullptr);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    gtk_menu_button_set_create_popup_func(
        GTK_MENU_BUTTON(button),
        [](GtkMenuButton* menu_button, gpointer data) {
          static_cast<RowBinding*>(data)->FillPopover(
              gtk_menu_button_get_popover(menu_button));
        },
        this, nullptr);
    return button;
  }

  // Rebuilt every time the popover opens, so it shows the current value,
  // machine and environment.
  void FillPopover(GtkPopover* popover) {
    const std::string value = Effective();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    GtkWidget* heading = NewWrappedLabel(spec_.title, false);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(box), heading);

    std::string details = spec_.hint.details;
    if (spec_.hint.details_for) {
      const std::string extra = spec_.hint.details_for(*context_);
      if (!extra.empty()) {
        if (!details.empty()) details += "\n\n";
        details += extra;
      }
    }
    for (const std::string& paragraph : SplitParagraphs(details)) {
      gtk_box_append(GTK_BOX(box), NewWrappedLabel(paragraph, true));
    }

    if (spec_.hint.recommend && context_->machine().detected) {
      const std::optional<std::string> recommended =
          spec_.hint.recommend(context_->machine());
      if (recommended.has_value()) {
        GtkWidget* line = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* badge = NewRecommendedBadge();
        gtk_widget_set_halign(badge, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(line), badge);
        std::string text =
            Format(_("For this computer: %s."), LabelFor(*recommended).c_str());
        if (spec_.hint.recommend_reason) {
          const std::string reason =
              spec_.hint.recommend_reason(context_->machine());
          if (!reason.empty()) text += " " + reason;
        }
        gtk_box_append(GTK_BOX(line), NewWrappedLabel(text, true));
        gtk_box_append(GTK_BOX(box), line);
      }
    }
    const std::string warning =
        spec_.hint.warning ? spec_.hint.warning(*context_, value) : "";
    if (!warning.empty()) {
      gtk_box_append(GTK_BOX(box), NewWarningLabel(warning));
    }
    if (const EnvOverride* env = context_->EffectiveOverride(spec_.key)) {
      gtk_box_append(
          GTK_BOX(box),
          NewWrappedLabel(
              Format(_("%s=%s is set in the shortcut or terminal that "
                       "started Mocktail, so this launch uses it instead of "
                       "the setting. Use Details in the banner to move it "
                       "into settings."),
                     env->name.c_str(),
                     RedactEnvironmentValue(env->value).c_str()),
              true));
    }
    if (!spec_.key.empty()) {
      std::string footer =
          Format(_("config.yaml: %s · default: %s"), spec_.key.c_str(),
                 LabelFor(Default()).c_str());
      const std::vector<std::string> names = OverridingVariables(spec_.key);
      if (!names.empty()) {
        std::string joined;
        for (const std::string& name : names) {
          if (!joined.empty()) joined += ", ";
          joined += name;
        }
        footer += "\n" + Format(_("Environment: %s"), joined.c_str());
      }
      GtkWidget* label = NewWrappedLabel(footer, true);
      gtk_widget_add_css_class(label, "dim-label");
      gtk_widget_add_css_class(label, "caption");
      gtk_box_append(GTK_BOX(box), label);
    }

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller),
                                               460);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
    gtk_popover_set_child(popover, scroller);
  }

  std::vector<LauncherContext::ListenerId> listeners_;
  RowKind kind_ = RowKind::kAction;
  GtkWidget* env_badge_ = nullptr;
  GtkWidget* reset_button_ = nullptr;
  int search_id_ = -1;
};

// ---- switch
// ------------------------------------------------------------------------

class SwitchBinding final : public RowBinding {
 public:
  using RowBinding::RowBinding;

  GtkWidget* Build() {
    GtkWidget* row = adw_switch_row_new();
    g_signal_connect(row, "notify::active", G_CALLBACK(OnActive), this);
    Attach(row, RowKind::kSwitch);
    return row;
  }

 protected:
  void SyncControl(const std::string& value) override {
    adw_switch_row_set_active(ADW_SWITCH_ROW(row_), value == "true");
  }
  std::string LabelFor(const std::string& value) const override {
    if (value == "true") return _("On");
    if (value == "false") return _("Off");
    return value;
  }

 private:
  static void OnActive(GObject* row, GParamSpec*, gpointer data) {
    static_cast<SwitchBinding*>(data)->Write(
        adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? "true" : "false",
        launcher::ScalarKind::kBool);
  }
};

// ---- combo
// ---------------------------------------------------------------------------

class ComboBinding final : public RowBinding {
 public:
  ComboBinding(LauncherContext* context, RowSpec spec, ComboSpec combo)
      : RowBinding(context, std::move(spec)), combo_(std::move(combo)) {}

  GtkWidget* Build() {
    GtkWidget* row = adw_combo_row_new();
    model_ = gtk_string_list_new(nullptr);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model_));
    g_object_unref(model_);
    GtkExpression* expression =
        gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
    adw_combo_row_set_expression(ADW_COMBO_ROW(row), expression);
    gtk_expression_unref(expression);
    adw_combo_row_set_enable_search(ADW_COMBO_ROW(row), combo_.enable_search);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(SetupSelected), nullptr);
    g_signal_connect(factory, "bind", G_CALLBACK(BindSelected), this);
    adw_combo_row_set_factory(ADW_COMBO_ROW(row), factory);
    g_object_unref(factory);
    GtkListItemFactory* list_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(list_factory, "setup", G_CALLBACK(SetupListItem), nullptr);
    g_signal_connect(list_factory, "bind", G_CALLBACK(BindListItem), this);
    adw_combo_row_set_list_factory(ADW_COMBO_ROW(row), list_factory);
    g_object_unref(list_factory);

    row_ = row;  // Rebuild() needs it before Attach().
    g_signal_connect(row, "notify::selected", G_CALLBACK(OnSelected), this);
    Attach(row, RowKind::kCombo);
    return row;
  }

 protected:
  void SyncControl(const std::string& value) override {
    Rebuild(value);
    const int position = PositionOf(value);
    if (position >= 0 && adw_combo_row_get_selected(ADW_COMBO_ROW(row_)) !=
                             static_cast<guint>(position)) {
      adw_combo_row_set_selected(ADW_COMBO_ROW(row_),
                                 static_cast<guint>(position));
    }
  }

  std::string BaseSubtitle(const std::string& value) const override {
    if (!spec_.hint.subtitle.empty()) return spec_.hint.subtitle;
    const int option = OptionIndexFor(value);
    if (option < 0) {
      return _("Set in config.yaml; not one of the choices listed here");
    }
    return Describe(static_cast<std::size_t>(option));
  }

  std::string LabelFor(const std::string& value) const override {
    const int option = OptionIndexFor(value);
    return option < 0 ? value
                      : combo_.options[static_cast<std::size_t>(option)].label;
  }

 private:
  // Index into combo_.options of the option `value` selects, or -1.
  int OptionIndexFor(const std::string& value) const {
    for (std::size_t index = 0; index < combo_.options.size(); ++index) {
      const ComboOption& option = combo_.options[index];
      if (option.custom) continue;
      if (option.value == value ||
          std::find(option.aliases.begin(), option.aliases.end(), value) !=
              option.aliases.end()) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  std::string Describe(std::size_t option) const {
    const ComboOption& entry = combo_.options[option];
    return entry.describe ? entry.describe(*context_) : entry.description;
  }

  std::string Unavailable(std::size_t option) const {
    const ComboOption& entry = combo_.options[option];
    return entry.unavailable ? entry.unavailable(*context_) : std::string();
  }

  // The list shows every option that is not hidden, the hidden one that is
  // the current value, and the current value itself when no option matches.
  void Rebuild(const std::string& value) {
    const int current = OptionIndexFor(value);
    std::vector<int> visible;
    for (std::size_t index = 0; index < combo_.options.size(); ++index) {
      if (!combo_.options[index].hidden || static_cast<int>(index) == current) {
        visible.push_back(static_cast<int>(index));
      }
    }
    std::string custom_value;
    if (current < 0 && !value.empty()) {
      visible.push_back(-1);
      custom_value = value;
    }
    if (visible == visible_ && custom_value == custom_value_ &&
        g_list_model_get_n_items(G_LIST_MODEL(model_)) == visible.size()) {
      return;
    }
    visible_ = std::move(visible);
    custom_value_ = std::move(custom_value);
    std::vector<std::string> labels;
    for (const int index : visible_) {
      labels.push_back(
          index < 0 ? Format(_("%s (from config.yaml)"), custom_value_.c_str())
                    : combo_.options[static_cast<std::size_t>(index)].label);
    }
    std::vector<const char*> pointers;
    for (const std::string& label : labels) pointers.push_back(label.c_str());
    pointers.push_back(nullptr);
    const bool was_updating = updating_;
    updating_ = true;
    gtk_string_list_splice(model_, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(model_)),
                           pointers.data());
    updating_ = was_updating;
  }

  int PositionOf(const std::string& value) const {
    const int option = OptionIndexFor(value);
    for (std::size_t position = 0; position < visible_.size(); ++position) {
      if (visible_[position] == option) return static_cast<int>(position);
    }
    return -1;
  }

  static void OnSelected(GObject*, GParamSpec*, gpointer data) {
    auto* binding = static_cast<ComboBinding*>(data);
    if (binding->updating_) return;
    const guint position =
        adw_combo_row_get_selected(ADW_COMBO_ROW(binding->row_));
    if (position >= binding->visible_.size()) return;
    const int option = binding->visible_[position];
    if (option < 0) return;  // the value config.yaml already holds
    const auto index = static_cast<std::size_t>(option);
    const ComboOption& entry = binding->combo_.options[index];
    const std::string reason = binding->Unavailable(index);
    if (!reason.empty()) {
      binding->context_->Toast(reason);
      binding->Refresh();
      return;
    }
    if (entry.custom) {
      binding->Refresh();
      if (binding->combo_.on_custom)
        binding->combo_.on_custom(*binding->context_);
      return;
    }
    binding->Write(entry.value,
                   binding->combo_.kind.has_value()
                       ? *binding->combo_.kind
                       : ScalarKindFor(binding->spec_.key, entry.value));
  }

  static void SetupSelected(GtkSignalListItemFactory*, GObject* object,
                            gpointer) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
  }

  static void BindSelected(GtkSignalListItemFactory*, GObject* object,
                           gpointer) {
    GtkListItem* item = GTK_LIST_ITEM(object);
    GtkStringObject* string = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                       gtk_string_object_get_string(string));
  }

  static void SetupListItem(GtkSignalListItemFactory*, GObject* object,
                            gpointer) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "combo-option");
    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(line), title);
    GtkWidget* badge = NewRecommendedBadge();
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(line), badge);
    gtk_box_append(GTK_BOX(box), line);
    GtkWidget* description = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(description), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 40);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_widget_add_css_class(description, "caption");
    gtk_box_append(GTK_BOX(box), description);
    gtk_list_item_set_child(GTK_LIST_ITEM(object), box);
  }

  static void BindListItem(GtkSignalListItemFactory*, GObject* object,
                           gpointer data) {
    auto* binding = static_cast<ComboBinding*>(data);
    GtkListItem* item = GTK_LIST_ITEM(object);
    GtkWidget* box = gtk_list_item_get_child(item);
    GtkWidget* line = gtk_widget_get_first_child(box);
    GtkWidget* title = gtk_widget_get_first_child(line);
    GtkWidget* badge = gtk_widget_get_next_sibling(title);
    GtkWidget* description = gtk_widget_get_next_sibling(line);
    const guint position = gtk_list_item_get_position(item);
    std::string label;
    std::string text;
    bool recommended = false;
    bool available = true;
    if (position < binding->visible_.size() &&
        binding->visible_[position] >= 0) {
      const auto index = static_cast<std::size_t>(binding->visible_[position]);
      const ComboOption& entry = binding->combo_.options[index];
      label = entry.label;
      text = binding->Describe(index);
      const std::string reason = binding->Unavailable(index);
      if (!reason.empty()) {
        available = false;
        text = reason;
      }
      if (binding->spec_.hint.recommend &&
          binding->context_->machine().detected) {
        const std::optional<std::string> value =
            binding->spec_.hint.recommend(binding->context_->machine());
        recommended = value.has_value() && *value == entry.value;
        if (recommended && binding->spec_.hint.recommend_reason) {
          const std::string why = binding->spec_.hint.recommend_reason(
              binding->context_->machine());
          if (!why.empty()) text += (text.empty() ? "" : " ") + why;
        }
      }
    } else {
      label =
          Format(_("%s (from config.yaml)"), binding->custom_value_.c_str());
      text = _("Set in config.yaml; not one of the choices listed here");
    }
    gtk_label_set_text(GTK_LABEL(title), label.c_str());
    gtk_label_set_text(GTK_LABEL(description), text.c_str());
    gtk_widget_set_visible(description, !text.empty());
    gtk_widget_set_visible(badge, recommended);
    gtk_widget_set_sensitive(box, available);
    gtk_list_item_set_selectable(item, available);
    gtk_list_item_set_activatable(item, available);
  }

  ComboSpec combo_;
  GtkStringList* model_ = nullptr;
  std::vector<int> visible_;
  std::string custom_value_;
};

// ---- spin
// --------------------------------------------------------------------------

class SpinBinding final : public RowBinding {
 public:
  SpinBinding(LauncherContext* context, RowSpec spec, SpinSpec spin)
      : RowBinding(context, std::move(spec)), spin_(spin) {}

  GtkWidget* Build() {
    GtkWidget* row =
        adw_spin_row_new_with_range(spin_.minimum, spin_.maximum, spin_.step);
    adw_spin_row_set_digits(ADW_SPIN_ROW(row), 0);
    adw_spin_row_set_numeric(ADW_SPIN_ROW(row), TRUE);
    g_signal_connect(row, "notify::value", G_CALLBACK(OnValue), this);
    Attach(row, RowKind::kSpin);
    return row;
  }

 protected:
  void SyncControl(const std::string& value) override {
    std::optional<double> number = ParseNumber(value);
    if (!number.has_value()) number = spin_.unset_value;
    if (number.has_value() &&
        adw_spin_row_get_value(ADW_SPIN_ROW(row_)) != *number) {
      adw_spin_row_set_value(ADW_SPIN_ROW(row_), *number);
    }
  }

 private:
  static void OnValue(GObject* row, GParamSpec*, gpointer data) {
    const double value = adw_spin_row_get_value(ADW_SPIN_ROW(row));
    static_cast<SpinBinding*>(data)->Write(
        std::to_string(static_cast<long long>(std::llround(value))),
        launcher::ScalarKind::kInteger);
  }

  SpinSpec spin_;
};

// ---- entry
// ---------------------------------------------------------------------------

class EntryBinding final : public RowBinding {
 public:
  EntryBinding(LauncherContext* context, RowSpec spec, EntrySpec entry)
      : RowBinding(context, std::move(spec)), entry_(std::move(entry)) {}

  GtkWidget* Build() {
    GtkWidget* row = adw_entry_row_new();
    g_signal_connect(row, "changed", G_CALLBACK(OnChanged), this);
    // An entry row has no subtitle; the hint subtitle becomes its tooltip
    // and the details stay one click away.
    Attach(row, RowKind::kEntry);
    return row;
  }

 protected:
  void SyncControl(const std::string& value) override {
    // Never overwrite what the user is typing, even if it is invalid.
    if (has_problem_) return;
    const char* current = gtk_editable_get_text(GTK_EDITABLE(row_));
    if (value != (current != nullptr ? current : "")) {
      gtk_editable_set_text(GTK_EDITABLE(row_), value.c_str());
    }
  }

  void SetSubtitle(const std::string& markup) override {
    gtk_widget_set_tooltip_markup(row_,
                                  markup.empty() ? nullptr : markup.c_str());
  }

  void ResetInput() override {
    if (!has_problem_) return;
    has_problem_ = false;
    context_->SetProblem(this, {});
    gtk_widget_remove_css_class(row_, "error");
  }

 private:
  static void OnChanged(GtkEditable* editable, gpointer data) {
    auto* binding = static_cast<EntryBinding*>(data);
    if (binding->updating_) return;
    const std::string text = gtk_editable_get_text(editable);
    const std::string problem =
        binding->entry_.validate ? binding->entry_.validate(text) : "";
    binding->has_problem_ = !problem.empty();
    binding->context_->SetProblem(
        binding,
        problem.empty()
            ? std::string()
            : Format("%s: %s", binding->spec_.title.c_str(), problem.c_str()));
    if (!problem.empty()) {
      gtk_widget_add_css_class(binding->row_, "error");
      return;
    }
    gtk_widget_remove_css_class(binding->row_, "error");
    if (text.empty() && binding->entry_.empty_unsets) {
      if (!binding->updating_)
        binding->context_->UnsetValue(binding->spec_.key);
      return;
    }
    binding->Write(text, binding->entry_.kind);
  }

  EntrySpec entry_;
  bool has_problem_ = false;
};

// ---- decorated (unbound) rows
// ----------------------------------------------------------

class DecoratedRow final : public RowBinding {
 public:
  using RowBinding::RowBinding;
  void Decorate(GtkWidget* row) { Attach(row, RowKind::kAction); }

 protected:
  void SyncControl(const std::string&) override {}
};

}  // namespace

std::string Markup(std::string_view text) {
  gchar* escaped =
      g_markup_escape_text(text.data(), static_cast<gssize>(text.size()));
  std::string result(escaped != nullptr ? escaped : "");
  g_free(escaped);
  return result;
}

GtkWidget* BindSwitchRow(LauncherContext* context, RowSpec spec) {
  return (new SwitchBinding(context, std::move(spec)))->Build();
}

GtkWidget* BindComboRow(LauncherContext* context, RowSpec spec,
                        ComboSpec combo) {
  return (new ComboBinding(context, std::move(spec), std::move(combo)))
      ->Build();
}

GtkWidget* BindSpinRow(LauncherContext* context, RowSpec spec, SpinSpec spin) {
  return (new SpinBinding(context, std::move(spec), spin))->Build();
}

GtkWidget* BindEntryRow(LauncherContext* context, RowSpec spec,
                        EntrySpec entry) {
  return (new EntryBinding(context, std::move(spec), std::move(entry)))
      ->Build();
}

GtkWidget* DecorateRow(LauncherContext* context, GtkWidget* row, RowSpec spec) {
  spec.resettable = false;
  (new DecoratedRow(context, std::move(spec)))->Decorate(row);
  return row;
}

void RefreshRows(LauncherContext* context) {
  context->NotifySettingChanged("");
}

GtkWidget* NewPage(LauncherContext* context, Section section,
                   const std::string& intro) {
  (void)context;
  const SectionInfo& info = GetSectionInfo(section);
  GtkWidget* page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), _(info.title));
  adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), info.icon);
  adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(page), info.id);
  if (!intro.empty()) {
    adw_preferences_page_set_description(ADW_PREFERENCES_PAGE(page),
                                         intro.c_str());
  }
  return page;
}

GtkWidget* AddGroup(GtkWidget* page, const std::string& title,
                    const std::string& intro) {
  GtkWidget* group = adw_preferences_group_new();
  if (!title.empty()) {
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    Markup(title).c_str());
  }
  if (!intro.empty()) {
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          Markup(intro).c_str());
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(group));
  return group;
}

void AddRow(GtkWidget* group, GtkWidget* row) {
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
}

GtkWidget* NewRecommendedBadge() {
  GtkWidget* badge = gtk_label_new(_("Recommended"));
  gtk_widget_add_css_class(badge, "recommended-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
  return badge;
}

GtkWidget* NewEnvBadge() {
  GtkWidget* badge = gtk_label_new("ENV");
  gtk_widget_add_css_class(badge, "env-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(badge), GTK_ACCESSIBLE_PROPERTY_LABEL,
      _("Overridden by an environment variable"), -1);
  return badge;
}

GtkWidget* NewWarningLabel(const std::string& text) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "launcher-warning");
  GtkWidget* icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_set_valign(icon, GTK_ALIGN_START);
  gtk_widget_add_css_class(icon, "warning");
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Warning"), -1);
  gtk_box_append(GTK_BOX(box), icon);
  GtkWidget* label = NewWrappedLabel(text, true);
  gtk_widget_add_css_class(label, "warning");
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);
  return box;
}

}  // namespace mocktail::launcher_ui
