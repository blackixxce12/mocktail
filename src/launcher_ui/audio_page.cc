#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "launcher_ui/audio_page_devices.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kOutputKey[] = "audio.output_device";
constexpr char kInputKey[] = "audio.input_device";
// Plugging a headset in and coming back lists the devices again, at most
// this often.
constexpr gint64 kRelistMicroseconds = 2 * G_USEC_PER_SEC;

// The device list behind both rows, refreshed on demand and when the window
// becomes active again. Owned by the page.
class AudioPageState {
 public:
  explicit AudioPageState(LauncherContext* context)
      : context_(context), cancellable_(g_cancellable_new()) {
    // The window exists once the machine profile arrives.
    listener_ = context_->OnMachineChanged([this] { WatchWindow(); });
  }
  ~AudioPageState() {
    g_cancellable_cancel(cancellable_);
    g_object_unref(cancellable_);
    context_->RemoveListener(listener_);
    if (window_ != nullptr) {
      g_signal_handler_disconnect(window_, active_handler_);
      g_object_remove_weak_pointer(G_OBJECT(window_),
                                   reinterpret_cast<gpointer*>(&window_));
    }
  }
  AudioPageState(const AudioPageState&) = delete;
  AudioPageState& operator=(const AudioPageState&) = delete;

  // nullptr until the first listing finished.
  const AudioDeviceList* devices() const {
    return devices_.has_value() ? &*devices_ : nullptr;
  }
  bool listing() const { return listing_; }

  void List() {
    if (listing_) return;
    listing_ = true;
    if (refresh_button_ != nullptr) {
      gtk_widget_set_sensitive(refresh_button_, FALSE);
    }
    ListAudioDevicesAsync(cancellable_, [this](AudioDeviceList list) {
      listing_ = false;
      listed_at_ = g_get_monotonic_time();
      devices_ = std::move(list);
      if (refresh_button_ != nullptr) {
        gtk_widget_set_sensitive(refresh_button_, TRUE);
      }
      // The rows read the list like the rest of the machine profile.
      context_->NotifyMachineChanged();
    });
  }

  GtkWidget* BuildRefreshButton() {
    refresh_button_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_add_css_class(refresh_button_, "flat");
    gtk_widget_set_valign(refresh_button_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(refresh_button_,
                                _("Look for sound devices again"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh_button_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Look for sound devices again"), -1);
    g_signal_connect(refresh_button_, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                       static_cast<AudioPageState*>(data)->List();
                     }),
                     this);
    return refresh_button_;
  }

 private:
  void WatchWindow() {
    GtkWindow* window = context_->window();
    if (window_ != nullptr || window == nullptr) return;
    window_ = window;
    g_object_add_weak_pointer(G_OBJECT(window_),
                              reinterpret_cast<gpointer*>(&window_));
    active_handler_ = g_signal_connect(
        window_, "notify::is-active",
        G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
          auto* self = static_cast<AudioPageState*>(data);
          if (gtk_window_is_active(GTK_WINDOW(object)) &&
              g_get_monotonic_time() - self->listed_at_ >=
                  kRelistMicroseconds) {
            self->List();
          }
        }),
        this);
  }

  LauncherContext* context_;
  GCancellable* cancellable_;
  LauncherContext::ListenerId listener_ = 0;
  std::optional<AudioDeviceList> devices_;
  bool listing_ = false;
  gint64 listed_at_ = 0;
  GtkWidget* refresh_button_ = nullptr;
  GtkWindow* window_ = nullptr;
  gulong active_handler_ = 0;
};

const std::vector<std::string>* Names(const AudioPageState& state, bool input) {
  const AudioDeviceList* list = state.devices();
  if (list == nullptr || !list->ok) return nullptr;
  return input ? &list->recording : &list->playback;
}

// "System default", every device name once ("Disabled" for the microphone),
// and the saved value when no device has it now.
std::vector<ComboOption> DeviceOptions(LauncherContext& context,
                                       const AudioPageState& state,
                                       bool input) {
  std::vector<ComboOption> options;
  // sdl_audio_sink.cc / sdl_audio_capture.cc: "default" follows the host
  // system default.
  options.push_back(
      {"default",
       _("System default"),
       input ? _("The microphone your desktop's sound settings choose")
             : _("The output your desktop's sound settings choose"),
       {},
       nullptr,
       nullptr,
       false,
       false});
  const std::vector<std::string>* names = Names(state, input);
  if (names != nullptr) {
    std::vector<std::string> unique;
    for (const std::string& name : *names) {
      if (std::find(unique.begin(), unique.end(), name) == unique.end()) {
        unique.push_back(name);
      }
    }
    for (const std::string& name : unique) {
      const bool shared = std::count(names->begin(), names->end(), name) > 1;
      options.push_back(
          {name,
           name,
           input ? _("Only this microphone; Roblox does not start without it")
                 : _("Only this device; Roblox does not start without it"),
           {},
           nullptr,
           // The runtime refuses a name several devices have.
           shared ? [](LauncherContext&) {
             return std::string(
                 _("Several devices have this name, so Roblox cannot tell "
                   "them apart"));
           }
                  : std::function<std::string(LauncherContext&)>(),
           false,
           false});
    }
  }
  if (input) {
    // runtime_config.h microphone_enabled(); webrtc_jni_audio_bridge.cc.
    options.push_back({"disabled",
                       _("Disabled"),
                       _("No microphone; Roblox is told it may not record"),
                       {},
                       nullptr,
                       nullptr,
                       false,
                       false});
  }
  const std::string value =
      context.EffectiveValue(input ? kInputKey : kOutputKey, "default");
  switch (ClassifyAudioDevice(value, names, input)) {
    case AudioDeviceState::kMissing:
      options.push_back({value,
                         Format(_("%s (not connected)"), value.c_str()),
                         _("Saved earlier; not connected now"),
                         {},
                         nullptr,
                         nullptr,
                         true,
                         false});
      break;
    case AudioDeviceState::kNotListed:
      options.push_back({value,
                         value,
                         state.devices() == nullptr
                             ? _("Looking for sound devices…")
                             : _("Sound devices could not be listed"),
                         {},
                         nullptr,
                         nullptr,
                         true,
                         false});
      break;
    case AudioDeviceState::kNumericId:
      // sdl_audio_capture.cc: "id:N" is an SDL device id of one start.
      options.push_back(
          {value,
           Format(_("Device number %s (from config.yaml)"),
                  value.substr(3).c_str()),
           _("A number from an earlier start; numbers change between starts"),
           {},
           nullptr,
           nullptr,
           true,
           false});
      break;
    case AudioDeviceState::kDefault:
    case AudioDeviceState::kDisabled:
    case AudioDeviceState::kConnected:
    case AudioDeviceState::kAmbiguous:
      break;
  }
  return options;
}

// Unlike other rows' short option labels, a device name can be long
// ("Ryzen HD Audio Controller Analog Stereo (not connected)"): shown in
// full it would push the row wider than a narrow window. The current device
// is ellipsized in the middle, keeping both ends, with the full name in the
// tooltip; the list itself still shows whole names.
void EllipsizeCurrentDevice(GtkWidget* row) {
  GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
  g_signal_connect(
      factory, "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* object, gpointer) {
        GtkWidget* label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        // Room for a recognizable name even in a narrow window.
        gtk_label_set_width_chars(GTK_LABEL(label), 12);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
        gtk_list_item_set_child(GTK_LIST_ITEM(object), label);
      }),
      nullptr);
  g_signal_connect(
      factory, "bind",
      G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* object, gpointer) {
        GtkListItem* item = GTK_LIST_ITEM(object);
        auto* string = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
        const char* text =
            string != nullptr ? gtk_string_object_get_string(string) : "";
        GtkWidget* label = gtk_list_item_get_child(item);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_widget_set_tooltip_text(label, text);
      }),
      nullptr);
  adw_combo_row_set_factory(ADW_COMBO_ROW(row), factory);
  g_object_unref(factory);
}

std::string ListingDetails(const AudioPageState& state, bool input) {
  const AudioDeviceList* list = state.devices();
  if (list == nullptr) return _("Looking for sound devices…");
  if (!list->ok) {
    return Format(_("Sound devices could not be listed: %s"),
                  list->error.c_str());
  }
  const std::size_t count =
      input ? list->recording.size() : list->playback.size();
  return Format(input ? ngettext("Sound system: %s, %zu microphone found.",
                                 "Sound system: %s, %zu microphones found.",
                                 static_cast<unsigned long>(count))
                      : ngettext("Sound system: %s, %zu output found.",
                                 "Sound system: %s, %zu outputs found.",
                                 static_cast<unsigned long>(count)),
                list->driver.c_str(), count);
}

// audio.output_device (sdl_audio_sink.cc ResolveSdlPlaybackDevice,
// fmod_jni_audio_bridge.cc; research/graphics.md 5.1).
GtkWidget* BuildOutputRow(LauncherContext* context, AudioPageState* state) {
  RowSpec spec;
  spec.key = kOutputKey;
  spec.title = _("Output device");
  spec.fallback = "default";
  spec.keywords = {"sound",  "speakers", "headphones", "output",
                   "device", "звук",     "динамики",   "наушники",
                   "вывод",  "колонки",  "устройство"};
  spec.hint.details =
      _("Where Roblox plays its sound. Mocktail opens the device through "
        "SDL, which names devices exactly as this list does.") +
      std::string("\n\n") +
      _("• System default: the output your desktop's sound settings choose. "
        "It keeps working when devices come and go.") +
      "\n\n" +
      // main.cc: a failed FMOD audio composition is fatal.
      _("• A named device: always that one. If it is unplugged, renamed or "
        "shares its name with another device, Roblox stops at start with an "
        "audio error instead of playing elsewhere.") +
      "\n\n" +
      // roblox_output_device_bridge.cc: Roblox's own menu switches live
      // without saving (research/graphics.md 5.1).
      _("Roblox's own audio settings can still switch the output while you "
        "play; that choice is not saved here.");
  spec.hint.details_for = [state](LauncherContext&) {
    return ListingDetails(*state, false);
  };
  spec.hint.recommend = [](const MachineProfile&) {
    return std::optional<std::string>("default");
  };
  spec.hint.recommend_reason = [](const MachineProfile&) {
    return std::string(
        _("A pinned device stops Roblox from starting whenever it is "
          "disconnected; System default follows your desktop instead."));
  };
  spec.hint.warning = [state](LauncherContext&, const std::string& value) {
    switch (ClassifyAudioDevice(value, Names(*state, false), false)) {
      case AudioDeviceState::kMissing:
        return std::string(
            _("This device is not connected, so Roblox will not start until "
              "it is back or System default is chosen."));
      case AudioDeviceState::kAmbiguous:
        return std::string(
            _("Several devices have this name, so Roblox refuses it. Choose "
              "System default."));
      default:
        break;
    }
    return std::string();
  };
  ComboSpec combo;
  combo.enable_search = true;
  combo.options_for = [state](LauncherContext& ctx) {
    return DeviceOptions(ctx, *state, false);
  };
  GtkWidget* row = BindComboRow(context, std::move(spec), std::move(combo));
  EllipsizeCurrentDevice(row);
  return row;
}

// audio.input_device (sdl_audio_capture.cc ResolveSdlRecordingDevice,
// webrtc_jni_audio_bridge.cc; research/graphics.md 5.2).
GtkWidget* BuildInputRow(LauncherContext* context, AudioPageState* state) {
  RowSpec spec;
  spec.key = kInputKey;
  spec.title = _("Microphone");
  spec.fallback = "default";
  spec.keywords = {"microphone", "mic",   "input", "voice",        "voice chat",
                   "микрофон",   "голос", "ввод",  "голосовой чат"};
  spec.hint.details =
      _("The microphone Roblox voice chat records from, opened through SDL "
        "by its exact name.") +
      std::string("\n\n") +
      _("• System default: the microphone your desktop's sound settings "
        "choose.") +
      "\n\n" +
      // main.cc: a failed WebRTC audio composition is fatal.
      _("• A named microphone: always that one. If it is missing at start or "
        "shares its name with another device, Roblox does not start.") +
      "\n\n" +
      // runtime_config.h microphone_enabled(); performance_policy.cc
      // MergeAudioCaptureClientSettingsOverrides.
      _("• Disabled: nothing is recorded, and Roblox is told it has no "
        "permission to use a microphone.") +
      "\n\n" +
      _("Whether voice chat is available at all depends on your Roblox "
        "account and Roblox's own settings.");
  spec.hint.details_for = [state](LauncherContext&) {
    return ListingDetails(*state, true);
  };
  spec.hint.recommend = [](const MachineProfile&) {
    return std::optional<std::string>("default");
  };
  spec.hint.recommend_reason = [](const MachineProfile&) {
    return std::string(
        _("A pinned microphone stops Roblox from starting whenever it is "
          "disconnected; System default follows your desktop instead."));
  };
  spec.hint.warning = [state](LauncherContext&, const std::string& value) {
    switch (ClassifyAudioDevice(value, Names(*state, true), true)) {
      case AudioDeviceState::kMissing:
        return std::string(
            _("This microphone is not connected, so Roblox will not start "
              "until it is back or System default is chosen."));
      case AudioDeviceState::kAmbiguous:
        return std::string(
            _("Several devices have this name, so Roblox refuses it. Choose "
              "System default."));
      case AudioDeviceState::kNumericId:
        return std::string(
            _("Device numbers change between starts. Choose the microphone "
              "by its name instead."));
      default:
        break;
    }
    return std::string();
  };
  ComboSpec combo;
  combo.enable_search = true;
  combo.options_for = [state](LauncherContext& ctx) {
    return DeviceOptions(ctx, *state, true);
  };
  GtkWidget* row = BindComboRow(context, std::move(spec), std::move(combo));
  EllipsizeCurrentDevice(row);
  return row;
}

}  // namespace

// Audio: the output device and the microphone, listed as the game sees
// them.
GtkWidget* BuildAudioPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kAudio,
              _("Where game sound plays and which microphone voice chat uses"));
  auto* state = new AudioPageState(context);
  g_object_set_data_full(
      G_OBJECT(page), "mocktail-audio-page", state,
      [](gpointer data) { delete static_cast<AudioPageState*>(data); });
  GtkWidget* devices =
      AddGroup(page, _("Sound devices"),
               _("Listed by the names the game uses; changes apply at the "
                 "next start"));
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(devices),
                                          state->BuildRefreshButton());
  AddRow(devices, BuildOutputRow(context, state));
  AddRow(devices, BuildInputRow(context, state));
  state->List();
  return page;
}

}  // namespace mocktail::launcher_ui
