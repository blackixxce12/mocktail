#include "launcher_ui/page_widgets.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"

namespace mocktail::launcher_ui {
namespace {

struct Callback {
  std::function<void()> fn;
};

void RunCallback(GtkWidget*, gpointer data) {
  auto* callback = static_cast<Callback*>(data);
  if (callback->fn) callback->fn();
}

void FreeCallback(gpointer data, GClosure*) {
  delete static_cast<Callback*>(data);
}

void ConnectCallback(GtkWidget* widget, const char* signal,
                     std::function<void()> fn) {
  g_signal_connect_data(widget, signal, G_CALLBACK(RunCallback),
                        new Callback{std::move(fn)}, FreeCallback,
                        GConnectFlags(0));
}

// ---- FollowContext ----------------------------------------------------------

struct Follower {
  LauncherContext* context;
  std::vector<LauncherContext::ListenerId> listeners;
};

void ForgetFollower(gpointer data, GObject*) {
  auto* follower = static_cast<Follower*>(data);
  for (const LauncherContext::ListenerId id : follower->listeners) {
    follower->context->RemoveListener(id);
  }
  delete follower;
}

// ---- file chooser -----------------------------------------------------------

GtkWidget* FindDescendantWithClass(GtkWidget* widget, const char* css_class) {
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, css_class)) return child;
    if (GtkWidget* found = FindDescendantWithClass(child, css_class)) {
      return found;
    }
  }
  return nullptr;
}

struct FileRequest {
  LauncherContext* context;
  std::string key;
};

void OnFileChosen(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<FileRequest> request(static_cast<FileRequest*>(data));
  GError* error = nullptr;
  GFile* file =
      gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    // Dismissed, or no dialog could be shown at all.
    if (error != nullptr &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
      request->context->Toast(
          Format(_("The file chooser could not open: %s"), error->message));
    }
    g_clear_error(&error);
    return;
  }
  gchar* path = g_file_get_path(file);
  g_object_unref(file);
  if (path == nullptr) {
    request->context->Toast(_("Choose a file on this computer"));
    return;
  }
  request->context->SetValue(request->key, path, launcher::ScalarKind::kString);
  g_free(path);
}

// ---- choice rows ------------------------------------------------------------

class ChoiceRow {
 public:
  ChoiceRow(LauncherContext* context, std::vector<Choice> choices,
            std::function<int(LauncherContext&)> current,
            std::function<void(LauncherContext&, int)> choose)
      : context_(context),
        choices_(std::move(choices)),
        current_(std::move(current)),
        choose_(std::move(choose)) {}

  GtkWidget* Build() {
    row_ = adw_combo_row_new();
    GtkStringList* model = gtk_string_list_new(nullptr);
    for (const Choice& choice : choices_) {
      gtk_string_list_append(model, choice.label.c_str());
    }
    adw_combo_row_set_model(ADW_COMBO_ROW(row_), G_LIST_MODEL(model));
    g_object_unref(model);
    GtkExpression* expression =
        gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
    adw_combo_row_set_expression(ADW_COMBO_ROW(row_), expression);
    gtk_expression_unref(expression);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(SetupSelected), nullptr);
    g_signal_connect(factory, "bind", G_CALLBACK(BindSelected), nullptr);
    adw_combo_row_set_factory(ADW_COMBO_ROW(row_), factory);
    g_object_unref(factory);
    GtkListItemFactory* list_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(list_factory, "setup", G_CALLBACK(SetupListItem), nullptr);
    g_signal_connect(list_factory, "bind", G_CALLBACK(BindListItem), this);
    adw_combo_row_set_list_factory(ADW_COMBO_ROW(row_), list_factory);
    g_object_unref(list_factory);

    g_object_set_data_full(
        G_OBJECT(row_), "mocktail-choice-row", this,
        [](gpointer data) { delete static_cast<ChoiceRow*>(data); });
    g_signal_connect(row_, "notify::selected", G_CALLBACK(OnSelected), this);
    Sync();
    FollowContext(context_, row_, [this] { Sync(); });
    return row_;
  }

 private:
  void Sync() {
    const int index = current_(*context_);
    if (index < 0 || index >= static_cast<int>(choices_.size())) return;
    if (adw_combo_row_get_selected(ADW_COMBO_ROW(row_)) !=
        static_cast<guint>(index)) {
      updating_ = true;
      adw_combo_row_set_selected(ADW_COMBO_ROW(row_),
                                 static_cast<guint>(index));
      updating_ = false;
    }
  }

  std::string Unavailable(std::size_t index) const {
    return choices_[index].unavailable ? choices_[index].unavailable(*context_)
                                       : std::string();
  }

  static void OnSelected(GObject*, GParamSpec*, gpointer data) {
    auto* self = static_cast<ChoiceRow*>(data);
    if (self->updating_) return;
    const guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(self->row_));
    if (index >= self->choices_.size()) return;
    if (static_cast<int>(index) == self->current_(*self->context_)) return;
    const std::string reason = self->Unavailable(index);
    if (!reason.empty()) {
      self->context_->Toast(reason);
      self->Sync();
      return;
    }
    self->choose_(*self->context_, static_cast<int>(index));
    self->Sync();
  }

  static void SetupSelected(GtkSignalListItemFactory*, GObject* object,
                            gpointer) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
    gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
  }

  static void BindSelected(GtkSignalListItemFactory*, GObject* object,
                           gpointer) {
    GtkListItem* item = GTK_LIST_ITEM(object);
    GtkStringObject* string = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                       gtk_string_object_get_string(string));
  }

  // The same layout as the bound combo rows' lists (bindings.cc).
  static void SetupListItem(GtkSignalListItemFactory*, GObject* object,
                            gpointer) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "combo-option");
    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(box), title);
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
    auto* self = static_cast<ChoiceRow*>(data);
    GtkListItem* item = GTK_LIST_ITEM(object);
    GtkWidget* box = gtk_list_item_get_child(item);
    GtkWidget* title = gtk_widget_get_first_child(box);
    GtkWidget* description = gtk_widget_get_next_sibling(title);
    const guint index = gtk_list_item_get_position(item);
    if (index >= self->choices_.size()) return;
    const std::string reason = self->Unavailable(index);
    const std::string text =
        reason.empty() ? self->choices_[index].description : reason;
    gtk_label_set_text(GTK_LABEL(title), self->choices_[index].label.c_str());
    gtk_label_set_text(GTK_LABEL(description), text.c_str());
    gtk_widget_set_visible(description, !text.empty());
    gtk_widget_set_sensitive(box, reason.empty());
    gtk_list_item_set_selectable(item, reason.empty());
    gtk_list_item_set_activatable(item, reason.empty());
  }

  LauncherContext* context_;
  std::vector<Choice> choices_;
  std::function<int(LauncherContext&)> current_;
  std::function<void(LauncherContext&, int)> choose_;
  GtkWidget* row_ = nullptr;
  bool updating_ = false;
};

}  // namespace

void FollowContext(LauncherContext* context, GtkWidget* owner,
                   std::function<void()> fn) {
  auto* follower = new Follower{context, {}};
  follower->listeners.push_back(
      context->OnSettingChanged([fn](std::string_view) { fn(); }));
  follower->listeners.push_back(context->OnMachineChanged(fn));
  g_object_weak_ref(G_OBJECT(owner), ForgetFollower, follower);
}

GtkWidget* NewRowButton(const std::string& label, std::function<void()> fn) {
  GtkWidget* button = gtk_button_new_with_label(label.c_str());
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  ConnectCallback(button, "clicked", std::move(fn));
  return button;
}

GtkWidget* NewRowIconButton(const char* icon_name, const std::string& label,
                            std::function<void()> fn) {
  GtkWidget* button = gtk_button_new_from_icon_name(icon_name);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "circular");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(button, label.c_str());
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
  ConnectCallback(button, "clicked", std::move(fn));
  return button;
}

GtkWidget* NewActionRow(const char* icon_name, std::function<void()> fn) {
  GtkWidget* row = adw_action_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
  gtk_accessible_update_state(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_STATE_HIDDEN,
                              TRUE, -1);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon);
  ConnectCallback(row, "activated", std::move(fn));
  return row;
}

void AddFileChooserButton(LauncherContext* context, GtkWidget* entry_row,
                          std::string key, std::string dialog_title) {
  GtkWidget* button = NewRowIconButton(
      "document-open-symbolic", _("Choose File…"),
      [context, entry_row, key, dialog_title] {
        if (context->selftest()) return;
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, dialog_title.c_str());
        gtk_file_dialog_set_modal(dialog, TRUE);
        const char* text = gtk_editable_get_text(GTK_EDITABLE(entry_row));
        const std::filesystem::path current(text != nullptr ? text : "");
        std::error_code error;
        if (current.is_absolute() &&
            std::filesystem::is_directory(current.parent_path(), error)) {
          GFile* folder = g_file_new_for_path(current.parent_path().c_str());
          gtk_file_dialog_set_initial_folder(dialog, folder);
          g_object_unref(folder);
        }
        GtkFileFilter* certificates = gtk_file_filter_new();
        gtk_file_filter_set_name(certificates, _("Certificates (PEM)"));
        for (const char* pattern : {"*.pem", "*.crt", "*.cer"}) {
          gtk_file_filter_add_pattern(certificates, pattern);
        }
        GtkFileFilter* everything = gtk_file_filter_new();
        gtk_file_filter_set_name(everything, _("All files"));
        gtk_file_filter_add_pattern(everything, "*");
        GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, certificates);
        g_list_store_append(filters, everything);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(filters);
        g_object_unref(certificates);
        g_object_unref(everything);
        gtk_file_dialog_open(dialog, context->window(), nullptr, OnFileChosen,
                             new FileRequest{context, key});
        g_object_unref(dialog);
      });
  // Before the binding's info button, so "Learn more" stays last as on
  // every other row.
  GtkWidget* info = FindDescendantWithClass(entry_row, "info-button");
  if (info != nullptr) {
    gtk_widget_insert_before(button, gtk_widget_get_parent(info), info);
  } else {
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(entry_row), button);
  }
}

GtkWidget* NewChoiceRow(
    LauncherContext* context, std::vector<Choice> choices,
    std::function<int(LauncherContext& context)> current,
    std::function<void(LauncherContext& context, int)> choose) {
  return (new ChoiceRow(context, std::move(choices), std::move(current),
                        std::move(choose)))
      ->Build();
}

FleasionInputs FleasionInputsFrom(const LauncherContext& context) {
  FleasionInputs inputs;
  inputs.enabled = context.EffectiveValue("integrations.fleasion.enabled",
                                          "false") == "true";
  inputs.proxy_mode =
      context.EffectiveValue("integrations.fleasion.proxy_mode", "env");
  inputs.proxy_port =
      context.EffectiveValue("integrations.fleasion.proxy_port", "58443");
  inputs.use_system_proxy =
      context.EffectiveValue("network.use_system_proxy", "false") == "true";
  inputs.network_proxy_host = context.Value("network.proxy_host");
  inputs.network_proxy_port = context.Value("network.proxy_port");
  return inputs;
}

std::string DescribeFleasionConflict(FleasionConflict conflict) {
  // runtime_config.cc, the fleasion_valid_ rules.
  switch (conflict) {
    case FleasionConflict::kSystemProxy:
      return _("Fleasion cannot be combined with the system proxy");
    case FleasionConflict::kDifferentProxy:
      return _(
          "Fleasion's proxy mode cannot be combined with a different "
          "manual proxy");
    case FleasionConflict::kProxyInHostsMode:
      return _("Fleasion's hosts mode cannot be combined with a manual proxy");
    case FleasionConflict::kNone:
      break;
  }
  return {};
}

std::string DisplayPath(const std::filesystem::path& path,
                        const std::filesystem::path& home) {
  const std::string text = path.string();
  const std::string prefix = home.string();
  if (!prefix.empty() && prefix != "/" && text.rfind(prefix, 0) == 0 &&
      (text.size() == prefix.size() || text[prefix.size()] == '/')) {
    return "~" + text.substr(prefix.size());
  }
  return text;
}

std::string DescribeNvidiaWaylandBlocker(const MachineProfile& machine,
                                         window::NvidiaWaylandBlocker blocker) {
  // video_driver_policy.h NvidiaNativeWaylandBlocker, in its order, and
  // window.cc's log line for each reason.
  switch (blocker) {
    case window::NvidiaWaylandBlocker::kWaylandNotPreferred:
      return _(
          "MOCKTAIL_PREFER_WAYLAND turns the preference for Wayland "
          "off.");
    case window::NvidiaWaylandBlocker::kCommitGuardOff:
      // wayland_surface_commit_guard.h SurfaceCommitGuardAllowed.
      return _(
          "MOCKTAIL_WAYLAND_COMMIT_GUARD turns off the guard that keeps "
          "the game window's own updates out of NVIDIA's Wayland "
          "presentation (upstream issue #186).");
    case window::NvidiaWaylandBlocker::kDriverVersionUnknown:
      return _(
          "The NVIDIA driver's version could not be read, so it may "
          "lack explicit sync, which NVIDIA's Wayland presentation "
          "needs (driver 555 or newer).");
    case window::NvidiaWaylandBlocker::kDriverWithoutExplicitSync:
      // kNvidiaExplicitSyncDriverMajor.
      return Format(_("NVIDIA driver %s lacks explicit sync, which NVIDIA's "
                      "Wayland presentation needs; drivers 555 and newer have "
                      "it."),
                    machine.gpu.nvidia_driver_version.c_str());
    case window::NvidiaWaylandBlocker::kExplicitSyncDisabled:
      // kNvidiaDisableExplicitSyncVariable; NVIDIA 575.51.02 changelog.
      return _(
          "__NV_DISABLE_EXPLICIT_SYNC turns off explicit sync in NVIDIA's "
          "driver, which NVIDIA's Wayland presentation needs.");
    case window::NvidiaWaylandBlocker::kOtherGpu:
      return _(
          "This computer has an Intel or AMD card beside the NVIDIA one, "
          "and NVIDIA's Wayland presentation is untested there.");
    case window::NvidiaWaylandBlocker::kNoNvidiaGpuListed:
      return _(
          "NVIDIA's kernel driver is loaded, but no NVIDIA card is "
          "listed in /sys/class/drm.");
    case window::NvidiaWaylandBlocker::kExplicitSyncUnknown:
      return _(
          "The desktop could not be asked whether it offers explicit "
          "sync (wp_linux_drm_syncobj_manager_v1), which NVIDIA's "
          "Wayland presentation needs.");
    case window::NvidiaWaylandBlocker::kCompositorWithoutExplicitSync:
      return _(
          "The desktop does not offer explicit sync "
          "(wp_linux_drm_syncobj_manager_v1), which NVIDIA's Wayland "
          "presentation needs.");
    case window::NvidiaWaylandBlocker::kVsyncOutsideHyprland:
      // A FIFO swapchain sat in vkAcquireNextImageKHR for most of the time
      // a test window was minimized on GNOME, an immediate one kept
      // presenting (sandbox-bench syncrace gnome-min.out, video_driver_
      // policy.h); the user's 92 Hyprland sessions ran clean (commit
      // 275e8f7). recommendations.h Presentation::kUnthrottled.
      return _(
          "Frames may wait for the display here (vertical sync), and then "
          "NVIDIA's Wayland presentation can stall the game while its window "
          "is hidden, as a test on GNOME showed; only on Hyprland has it "
          "proven safe so far. Vertical sync Off, or Automatic with the 240 "
          "maximum frame rate, avoids the wait.");
    case window::NvidiaWaylandBlocker::kNone:
      break;
  }
  return {};
}

std::string ShortNvidiaWaylandBlocker(const MachineProfile& machine,
                                      window::NvidiaWaylandBlocker blocker) {
  switch (blocker) {
    case window::NvidiaWaylandBlocker::kWaylandNotPreferred:
      return _("MOCKTAIL_PREFER_WAYLAND is off");
    case window::NvidiaWaylandBlocker::kCommitGuardOff:
      return _("the surface-commit guard is off");
    case window::NvidiaWaylandBlocker::kDriverVersionUnknown:
      return _("NVIDIA's driver version is unknown");
    case window::NvidiaWaylandBlocker::kDriverWithoutExplicitSync:
      return Format(_("driver %s lacks explicit sync"),
                    machine.gpu.nvidia_driver_version.c_str());
    case window::NvidiaWaylandBlocker::kExplicitSyncDisabled:
      return _("__NV_DISABLE_EXPLICIT_SYNC is set");
    case window::NvidiaWaylandBlocker::kOtherGpu:
      return _("a second graphics card");
    case window::NvidiaWaylandBlocker::kNoNvidiaGpuListed:
      return _("no NVIDIA card is listed");
    case window::NvidiaWaylandBlocker::kExplicitSyncUnknown:
      return _("explicit sync could not be checked");
    case window::NvidiaWaylandBlocker::kCompositorWithoutExplicitSync:
      return _("the desktop lacks explicit sync");
    case window::NvidiaWaylandBlocker::kVsyncOutsideHyprland:
      return _("frames wait for the display outside Hyprland");
    case window::NvidiaWaylandBlocker::kNone:
      break;
  }
  return {};
}

}  // namespace mocktail::launcher_ui
