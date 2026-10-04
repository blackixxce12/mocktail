#include "launcher_ui/advanced_fast_flags.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "launcher/fast_flags_document.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/page_dialogs.h"
#include "launcher_ui/page_rules.h"
#include "launcher_ui/page_widgets.h"

namespace mocktail::launcher_ui {
namespace {

using launcher::FastFlagConflict;
using launcher::FastFlagConflictEffect;
using launcher::FastFlagEntry;
using launcher::FastFlagsDocument;
using launcher::FastFlagValueKind;

// Rows that show the flags refresh on this pseudo key.
constexpr char kFastFlagsKey[] = "@fast-flags";

std::string Normalized(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "1" || lower == "true" || lower == "on" || lower == "yes") {
    return "true";
  }
  if (lower == "0" || lower == "false" || lower == "off" || lower == "no") {
    return "false";
  }
  return value;
}

bool IsVulkanBackend(const std::string& value) {
  return value == "direct-vulkan" || value == "vulkan" ||
         value == "native-vulkan";
}

// fflags.json, staged until Save.
class FastFlagsState {
 public:
  static FastFlagsState* For(LauncherContext* context) {
    static std::unique_ptr<FastFlagsState> instance;
    if (instance == nullptr) {
      instance.reset(new FastFlagsState(context));
      instance->Load();
      instance->Register();
    }
    return instance.get();
  }

  const FastFlagsDocument& document() const { return document_; }
  const std::string& load_error() const { return load_error_; }
  const std::vector<FastFlagConflict>& conflicts() const { return conflicts_; }
  LauncherContext* context() const { return context_; }

  const FastFlagConflict* ConflictFor(const std::string& name) const {
    for (const FastFlagConflict& conflict : conflicts_) {
      if (conflict.name == name) return &conflict;
    }
    return nullptr;
  }

  int BlockingConflicts() const {
    return static_cast<int>(std::count_if(
        conflicts_.begin(), conflicts_.end(),
        [](const FastFlagConflict& conflict) {
          return conflict.effect == FastFlagConflictEffect::kBlocksStart;
        }));
  }

  bool Set(const std::string& name, FastFlagValueKind kind,
           const std::string& value, std::string* error) {
    if (!document_.Set(name, kind, value, error)) return false;
    Changed();
    return true;
  }

  void Remove(const std::string& name) {
    if (document_.Remove(name)) Changed();
  }

  // Adds every entry of a JSON object; false (nothing changed) when the
  // text is not one fflags.json may hold.
  bool Import(const std::string& bytes, int* imported, std::string* error) {
    FastFlagsDocument incoming;
    if (!FastFlagsDocument::FromBytes(bytes, &incoming, error)) return false;
    FastFlagsDocument merged = document_;
    for (const FastFlagEntry& entry : incoming.entries()) {
      if (!merged.Set(entry.name, entry.kind, entry.value, error)) {
        return false;
      }
    }
    document_ = std::move(merged);
    *imported = static_cast<int>(incoming.entries().size());
    Changed();
    return true;
  }

  // Discard / reload: the file as it is on disk now.
  void Load() {
    std::string error;
    FastFlagsDocument loaded;
    if (FastFlagsDocument::Load(context_->fast_flags_file(), &loaded, &error)) {
      document_ = std::move(loaded);
      load_error_.clear();
    } else {
      document_ = FastFlagsDocument();
      load_error_ = error;
    }
    saved_ = document_.entries();
    Changed();
  }

 private:
  explicit FastFlagsState(LauncherContext* context) : context_(context) {}

  void Register() {
    DirtySource source;
    source.name = "fflags.json";
    source.count = [this] {
      return load_error_.empty()
                 ? CountFastFlagChanges(saved_, document_.entries())
                 : 0;
    };
    source.problem = [this] { return Problem(); };
    source.save = [this](std::string* error) {
      if (!document_.Save(context_->fast_flags_file(), error)) return false;
      saved_ = document_.entries();
      return true;
    };
    source.discard = [this] { Load(); };
    context_->AddDirtySource(std::move(source));
    // The conflicts follow the frame rate, performance and quality
    // settings, and the GPU (Intel-only machines get level 1).
    context_->OnSettingChanged([this](std::string_view key) {
      if (key != kFastFlagsKey) UpdateConflicts(true);
    });
    context_->OnMachineChanged([this] { UpdateConflicts(true); });
  }

  // Blocks Save and Play: Roblox would not start (Mocktail's own value for
  // the flag differs and is checked), or the file would be too big.
  std::string Problem() const {
    for (const FastFlagConflict& conflict : conflicts_) {
      if (conflict.effect == FastFlagConflictEffect::kBlocksStart) {
        return Format(_("Fast Flag %s conflicts with your settings; remove it "
                        "or change the setting"),
                      conflict.name.c_str());
      }
    }
    if (document_.Serialize().size() > FastFlagsDocument::kMaximumBytes) {
      return _("Fast Flags exceed the 64 KiB fflags.json may hold");
    }
    return {};
  }

  std::string SettingValue(const char* key, const char* fallback) const {
    if (const EnvOverride* env = context_->EffectiveOverride(key)) {
      return env->value;
    }
    return context_->EffectiveValue(key, fallback);
  }

  void UpdateConflicts(bool notify) {
    FastFlagSettings settings;
    settings.frame_rate_limit = SettingValue("graphics.frame_rate_limit", "-1");
    settings.multithreaded_rendering = Normalized(
        SettingValue("performance.multithreaded_rendering", "false"));
    settings.physics_worker_mode =
        SettingValue("performance.physics_worker_mode", "throughput");
    settings.memory_limit_mb = SettingValue("performance.memory_limit_mb", "0");
    settings.gamemode = SettingValue("performance.gamemode", "auto");
    settings.graphics_quality =
        SettingValue("engine.graphics_quality", "default");
    settings.intel_only_direct_vulkan =
        context_->machine().gpu.intel_only() &&
        IsVulkanBackend(SettingValue("graphics.backend", "direct-vulkan"));
    if (settings_ == settings_key(settings) && !conflicts_dirty_) return;
    settings_ = settings_key(settings);
    conflicts_dirty_ = false;
    conflicts_ = FindFastFlagConflicts(document_, settings);
    if (notify) context_->NotifyDirtyChanged();
  }

  static std::string settings_key(const FastFlagSettings& settings) {
    return settings.frame_rate_limit + '\n' + settings.multithreaded_rendering +
           '\n' + settings.physics_worker_mode + '\n' +
           settings.memory_limit_mb + '\n' + settings.gamemode + '\n' +
           settings.graphics_quality + '\n' +
           (settings.intel_only_direct_vulkan ? "1" : "0");
  }

  void Changed() {
    conflicts_dirty_ = true;
    UpdateConflicts(false);
    context_->NotifyDirtyChanged();
    context_->NotifySettingChanged(kFastFlagsKey);
  }

  LauncherContext* context_;
  FastFlagsDocument document_;
  std::vector<FastFlagEntry> saved_;
  std::string load_error_;
  std::vector<FastFlagConflict> conflicts_;
  std::string settings_;
  bool conflicts_dirty_ = true;
};

std::string ValueText(const FastFlagEntry& entry) {
  switch (entry.kind) {
    case FastFlagValueKind::kString:
      return "“" + entry.value + "”";
    case FastFlagValueKind::kBoolean:
    case FastFlagValueKind::kInteger:
      break;
  }
  return entry.value;
}

std::string KindLabel(FastFlagValueKind kind) {
  switch (kind) {
    case FastFlagValueKind::kBoolean:
      return _("true or false");
    case FastFlagValueKind::kInteger:
      return _("a whole number");
    case FastFlagValueKind::kString:
      break;
  }
  return _("text");
}

bool IsFlagName(const std::string& name) {
  return !name.empty() &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_';
         });
}

// ---- add / edit -------------------------------------------------------------

struct FlagForm {
  FastFlagsState* state;
  std::string original;  // empty when adding
  GtkWidget* name = nullptr;
  GtkWidget* value = nullptr;
  GtkWidget* message = nullptr;
  AdwAlertDialog* dialog = nullptr;
};

// The problem with the form, or the inferred type when there is none.
bool CheckForm(FlagForm* form, std::string* text) {
  const std::string name = gtk_editable_get_text(GTK_EDITABLE(form->name));
  const std::string value = gtk_editable_get_text(GTK_EDITABLE(form->value));
  if (name.empty()) {
    *text = _("Enter the flag's name, for example FFlagDebugDisplayFPS");
    return false;
  }
  if (!IsFlagName(name)) {
    *text = _("A flag name uses only letters, digits and underscores");
    return false;
  }
  const FastFlagValueKind kind = InferFastFlagKind(name, value);
  FastFlagsDocument probe;
  std::string error;
  if (!probe.Set(name, kind, value, &error)) {
    *text = Format(_("This flag takes %s"), KindLabel(kind).c_str());
    return false;
  }
  *text = Format(_("Saved as %s"), KindLabel(kind).c_str());
  if (name != form->original && form->state->document().Find(name) != nullptr) {
    *text += " · " + std::string(_("replaces the flag already in the list"));
  }
  if (FastFlagsDocument::IsManagedFlag(name)) {
    *text += " · " + std::string(_("Mocktail sets this flag itself for some "
                                   "settings"));
  }
  return true;
}

void UpdateForm(FlagForm* form) {
  std::string text;
  const bool valid = CheckForm(form, &text);
  gtk_label_set_text(GTK_LABEL(form->message), text.c_str());
  if (valid) {
    gtk_widget_remove_css_class(form->message, "error");
  } else {
    gtk_widget_add_css_class(form->message, "error");
  }
  adw_alert_dialog_set_response_enabled(form->dialog, "apply", valid);
}

void OpenFlagForm(FastFlagsState* state, const FastFlagEntry* entry) {
  LauncherContext* context = state->context();
  const bool editing = entry != nullptr;
  AdwDialog* dialog = adw_alert_dialog_new(
      editing ? _("Edit Flag") : _("Add Flag"),
      _("Flag names start with FFlag, FInt, FString or their DF forms; the "
        "type follows the name."));
  auto* form = new FlagForm{state, editing ? entry->name : std::string()};
  form->dialog = ADW_ALERT_DIALOG(dialog);
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  form->name = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(form->name), _("Name"));
  form->value = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(form->value), _("Value"));
  if (editing) {
    gtk_editable_set_text(GTK_EDITABLE(form->name), entry->name.c_str());
    gtk_editable_set_text(GTK_EDITABLE(form->value), entry->value.c_str());
  }
  gtk_list_box_append(GTK_LIST_BOX(list), form->name);
  gtk_list_box_append(GTK_LIST_BOX(list), form->value);
  gtk_box_append(GTK_BOX(box), list);
  form->message = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(form->message), TRUE);
  gtk_label_set_xalign(GTK_LABEL(form->message), 0.0F);
  gtk_widget_add_css_class(form->message, "caption");
  gtk_box_append(GTK_BOX(box), form->message);
  adw_alert_dialog_set_extra_child(form->dialog, box);
  adw_alert_dialog_add_responses(form->dialog, "cancel", _("_Cancel"), "apply",
                                 editing ? _("_Save") : _("_Add"), nullptr);
  adw_alert_dialog_set_response_appearance(form->dialog, "apply",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(form->dialog, "apply");
  adw_alert_dialog_set_close_response(form->dialog, "cancel");
  for (GtkWidget* field : {form->name, form->value}) {
    g_signal_connect_swapped(field, "changed", G_CALLBACK(UpdateForm), form);
  }
  UpdateForm(form);
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
        auto* form = static_cast<FlagForm*>(data);
        if (g_strcmp0(response, "apply") != 0) return;
        std::string text;
        if (!CheckForm(form, &text)) return;
        const std::string name =
            gtk_editable_get_text(GTK_EDITABLE(form->name));
        const std::string value =
            gtk_editable_get_text(GTK_EDITABLE(form->value));
        std::string error;
        if (!form->original.empty() && form->original != name) {
          form->state->Remove(form->original);
        }
        if (!form->state->Set(name, InferFastFlagKind(name, value), value,
                              &error)) {
          form->state->context()->Toast(error);
        }
      }),
      form,
      [](gpointer data, GClosure*) { delete static_cast<FlagForm*>(data); },
      GConnectFlags(0));
  adw_dialog_present(dialog, GTK_WIDGET(context->window()));
}

// ---- import -----------------------------------------------------------------

void OnImportChosen(GObject* source, GAsyncResult* result, gpointer data) {
  auto* state = static_cast<FastFlagsState*>(data);
  GError* error = nullptr;
  GFile* file =
      gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    g_clear_error(&error);
    return;
  }
  gchar* path = g_file_get_path(file);
  g_object_unref(file);
  std::string bytes;
  if (path != nullptr) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error && size <= FastFlagsDocument::kMaximumBytes) {
      std::ifstream input(path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
    }
  }
  g_free(path);
  int imported = 0;
  std::string problem;
  if (bytes.empty() || !state->Import(bytes, &imported, &problem)) {
    state->context()->Toast(
        _("Nothing imported: choose a JSON object of flags of at most 64 KiB, "
          "like ClientAppSettings.json"));
    return;
  }
  state->context()->Toast(Format(
      ngettext("%d flag imported", "%d flags imported", imported), imported));
}

void ImportJson(FastFlagsState* state) {
  LauncherContext* context = state->context();
  if (context->selftest()) return;
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Import Fast Flags"));
  GtkFileFilter* json = gtk_file_filter_new();
  gtk_file_filter_set_name(json, _("JSON files"));
  gtk_file_filter_add_pattern(json, "*.json");
  GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, json);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  g_object_unref(filters);
  g_object_unref(json);
  gtk_file_dialog_open(dialog, context->window(), nullptr, OnImportChosen,
                       state);
  g_object_unref(dialog);
}

// ---- the editor -------------------------------------------------------------

class FastFlagsEditor {
 public:
  explicit FastFlagsEditor(FastFlagsState* state) : state_(state) {}

  void Present() {
    LauncherContext* context = state_->context();
    dialog_ = adw_dialog_new();
    adw_dialog_set_title(dialog_, _("Fast Flags"));
    adw_dialog_set_content_width(dialog_, 640);
    adw_dialog_set_content_height(dialog_, 680);
    g_object_set_data_full(
        G_OBJECT(dialog_), "mocktail-fast-flags-editor", this,
        [](gpointer data) { delete static_cast<FastFlagsEditor*>(data); });

    GtkWidget* header = adw_header_bar_new();
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, _("Add Flag"));
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Add Flag"), -1);
    g_signal_connect_swapped(add, "clicked", G_CALLBACK(+[](gpointer data) {
                               OpenFlagForm(static_cast<FastFlagsState*>(data),
                                            nullptr);
                             }),
                             state_);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), add);

    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Import JSON…"), "fflags.import");
    g_menu_append(menu, _("Open fflags.json"), "fflags.open");
    g_menu_append(menu, _("Reload fflags.json"), "fflags.reload");
    GtkWidget* more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(more, _("More"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(more),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("More Fast Flags actions"), -1);
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), more);
    InstallActions();

    // research/ux.md 2.7.3 and 4.3: since 29 Sep 2025 Roblox applies only
    // allowlisted client flags.
    GtkWidget* banner = adw_banner_new(
        _("Use with caution: Roblox ignores most flags that are not on its "
          "allowlist"));
    adw_banner_set_revealed(ADW_BANNER(banner), TRUE);

    page_ = adw_preferences_page_new();
    adw_preferences_page_set_description(
        ADW_PREFERENCES_PAGE(page_),
        _("Saved to fflags.json with the other settings. Where Mocktail sets "
          "a flag itself, its own value wins."));
    Rebuild();
    FollowContext(context, page_, [this] { Rebuild(); });

    GtkWidget* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), banner);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page_);
    adw_dialog_set_child(dialog_, toolbar);
    adw_dialog_present(dialog_, GTK_WIDGET(context->window()));
  }

 private:
  void InstallActions() {
    GSimpleActionGroup* group = g_simple_action_group_new();
    const GActionEntry entries[] = {
        {"import",
         [](GSimpleAction*, GVariant*, gpointer data) {
           ImportJson(static_cast<FastFlagsEditor*>(data)->state_);
         },
         nullptr,
         nullptr,
         nullptr,
         {}},
        {"open",
         [](GSimpleAction*, GVariant*, gpointer data) {
           LauncherContext* context =
               static_cast<FastFlagsEditor*>(data)->state_->context();
           std::error_code error;
           if (!std::filesystem::exists(context->fast_flags_file(), error)) {
             context->Toast(
                 _("fflags.json does not exist yet; add a flag and "
                   "save first"));
             return;
           }
           context->OpenPath(context->fast_flags_file());
         },
         nullptr,
         nullptr,
         nullptr,
         {}},
        {"reload",
         [](GSimpleAction*, GVariant*, gpointer data) {
           static_cast<FastFlagsEditor*>(data)->state_->Load();
         },
         nullptr,
         nullptr,
         nullptr,
         {}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(group), entries,
                                    G_N_ELEMENTS(entries), this);
    gtk_widget_insert_action_group(GTK_WIDGET(dialog_), "fflags",
                                   G_ACTION_GROUP(group));
    g_object_unref(group);
  }

  void Rebuild() {
    if (group_ != nullptr) {
      adw_preferences_page_remove(ADW_PREFERENCES_PAGE(page_),
                                  ADW_PREFERENCES_GROUP(group_));
    }
    const FastFlagsDocument& document = state_->document();
    const int count = static_cast<int>(document.entries().size());
    group_ = AddGroup(
        page_, _("Flags"),
        count == 0 ? std::string()
                   : Format(ngettext("%d flag", "%d flags", count), count));
    if (!state_->load_error().empty()) {
      GtkWidget* row = adw_action_row_new();
      adw_preferences_row_set_title(
          ADW_PREFERENCES_ROW(row),
          Markup(_("fflags.json cannot be read")).c_str());
      adw_action_row_set_subtitle(
          ADW_ACTION_ROW(row),
          Markup(state_->load_error() + "\n" +
                 _("Fix the file, then reload it from the menu. Until then "
                   "Mocktail stops at start."))
              .c_str());
      AddRow(group_, row);
      return;
    }
    if (count == 0) {
      GtkWidget* row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                    Markup(_("No flags")).c_str());
      adw_action_row_set_subtitle(
          ADW_ACTION_ROW(row),
          Markup(_("Add one with the + button, or import a JSON file such as "
                   "ClientAppSettings.json from the menu"))
              .c_str());
      AddRow(group_, row);
      return;
    }
    const std::string warning_color = state_->context()->warning_color();
    for (const FastFlagEntry& entry : document.entries()) {
      GtkWidget* row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                    Markup(entry.name).c_str());
      std::string subtitle = Markup(ValueText(entry));
      if (const FastFlagConflict* conflict = state_->ConflictFor(entry.name)) {
        // FastFlagsDocument::FindManagedConflicts: kBlocksStart is
        // "... policy conflicts with <flag>", kOverridden is overwritten.
        const std::string text =
            conflict->effect == FastFlagConflictEffect::kBlocksStart
                ? Format(_("Your settings make Mocktail set %s here, so "
                           "Roblox will not start with this value"),
                         conflict->managed_value.c_str())
                : Format(_("Mocktail always sets %s here; this value is "
                           "ignored"),
                         conflict->managed_value.c_str());
        subtitle += "\n<span foreground=\"" + warning_color + "\">" +
                    Markup(text) + "</span>";
      } else if (FastFlagsDocument::IsManagedFlag(entry.name)) {
        subtitle += "\n" + Markup(_("Mocktail sets this flag itself for some "
                                    "settings"));
      }
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
      adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
      const std::string name = entry.name;
      FastFlagsState* state = state_;
      adw_action_row_add_suffix(
          ADW_ACTION_ROW(row),
          NewRowIconButton("document-edit-symbolic",
                           Format(_("Edit %s"), name.c_str()), [state, name] {
                             if (const FastFlagEntry* current =
                                     state->document().Find(name)) {
                               const FastFlagEntry copy = *current;
                               OpenFlagForm(state, &copy);
                             }
                           }));
      adw_action_row_add_suffix(
          ADW_ACTION_ROW(row),
          NewRowIconButton(
              "user-trash-symbolic", Format(_("Remove %s"), name.c_str()),
              [state, name] {
                const FastFlagEntry* current = state->document().Find(name);
                if (current == nullptr) return;
                const FastFlagEntry removed = *current;
                state->Remove(name);
                state->context()->Toast(Format(_("“%s” removed"), name.c_str()),
                                        _("Undo"), [state, removed] {
                                          std::string error;
                                          state->Set(removed.name, removed.kind,
                                                     removed.value, &error);
                                        });
              }));
      AddRow(group_, row);
    }
  }

  FastFlagsState* state_;
  AdwDialog* dialog_ = nullptr;
  GtkWidget* page_ = nullptr;
  GtkWidget* group_ = nullptr;
};

}  // namespace

void OpenFastFlagsEditor(LauncherContext* context) {
  if (context->window() == nullptr) return;
  (new FastFlagsEditor(FastFlagsState::For(context)))->Present();
}

GtkWidget* BuildFastFlagsRow(LauncherContext* context) {
  FastFlagsState* state = FastFlagsState::For(context);
  GtkWidget* row = NewActionRow("go-next-symbolic",
                                [context] { OpenFastFlagsEditor(context); });
  RowSpec spec;
  spec.title = _("Fast Flags editor");
  spec.keywords = {"fflags",      "fast flags",        "fflag",  "flags",
                   "fflags.json", "clientappsettings", "engine", "флаги",
                   "фастфлаги",   "фаст флаги"};
  spec.hint.subtitle_for = [state](LauncherContext&, const std::string&) {
    if (!state->load_error().empty()) {
      return std::string(_("fflags.json cannot be read"));
    }
    const int count = static_cast<int>(state->document().entries().size());
    if (count == 0)
      return std::string(_("No flags: Roblox uses its own values"));
    return Format(
        ngettext("%d flag in fflags.json", "%d flags in fflags.json", count),
        count);
  };
  spec.hint.warning = [state](LauncherContext&, const std::string&) {
    if (!state->load_error().empty()) {
      // main.cc: "[FATAL] Cannot load FFlag overrides".
      return std::string(
          _("Mocktail will not start until fflags.json is fixed"));
    }
    const int blocking = state->BlockingConflicts();
    if (blocking > 0) {
      return Format(ngettext("%d flag conflicts with your settings, so Roblox "
                             "would not start",
                             "%d flags conflict with your settings, so Roblox "
                             "would not start",
                             blocking),
                    blocking);
    }
    return std::string();
  };
  spec.hint.details =
      // main.cc: config_root/fflags.json merged under the base overrides;
      // client_settings_service.cc LoadAndMergeFflagsFile.
      _("Fast Flags are Roblox's internal switches. Mocktail passes the "
        "flags in fflags.json to Roblox at every start, but its own settings "
        "come first: a flag Mocktail sets itself is ignored or, when the "
        "values disagree on something Mocktail checks, stops Roblox from "
        "starting.") +
      std::string("\n\n") +
      // research/ux.md 2.7.3: the allowlist of 29 Sep 2025.
      _("Since September 2025 Roblox applies only flags on its allowlist and "
        "quietly ignores most others.") +
      "\n\n" +
      _("A wrong flag can make Roblox unstable or stop it from starting; "
        "remove it if Roblox misbehaves. Most players never need any.") +
      "\n\n" +
      // client_settings_service.cc: 64 KiB, string/boolean/integer values.
      _("The file holds at most 64 KiB; values are text, true/false or whole "
        "numbers.");
  spec.hint.details_for = [](LauncherContext& context) {
    return Format(_("File: %s"), context.fast_flags_file().c_str());
  };
  return DecorateRow(context, row, std::move(spec));
}

}  // namespace mocktail::launcher_ui
