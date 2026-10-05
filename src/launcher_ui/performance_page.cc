#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"
#include "launcher_ui/roblox_decides.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kMultithreadedKey[] = "performance.multithreaded_rendering";
constexpr char kPhysicsKey[] = "performance.physics_worker_mode";
constexpr char kGameModeKey[] = "performance.gamemode";
constexpr char kMemoryKey[] = "performance.memory_limit_mb";

constexpr double kMebibytesPerGibibyte = 1024.0;

std::string Physics(const LauncherContext& context) {
  // runtime_config.cc: MOCKTAIL_PHYSICS_WORKER_MODE defaults to throughput.
  return context.GameValue(kPhysicsKey, "throughput");
}

std::optional<std::uint64_t> ParseMebibytes(std::string_view text) {
  std::uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc() ||
      parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::string GibibytesText(double gibibytes) {
  return Format(_("%s GiB"), DecimalText(gibibytes, 1).c_str());
}

// What the session bus says about the GameMode daemon. libgamemode talks to
// com.feralinteractive.GameMode, or inside Flatpak to the GameMode portal of
// org.freedesktop.portal.Desktop.
enum class GameModeService {
  kChecking,
  kRunning,      // the name has an owner
  kActivatable,  // D-Bus starts it on the first request
  kMissing,      // neither
  kNoBus,        // no session bus to ask
};

// Asks the session bus once, asynchronously; without a bus it reports
// kNoBus instead of letting GIO autolaunch one.
class GameModeProbe {
 public:
  explicit GameModeProbe(LauncherContext* context)
      : context_(context), cancellable_(g_cancellable_new()) {}
  ~GameModeProbe() {
    g_cancellable_cancel(cancellable_);
    g_object_unref(cancellable_);
    if (bus_ != nullptr) g_object_unref(bus_);
  }
  GameModeProbe(const GameModeProbe&) = delete;
  GameModeProbe& operator=(const GameModeProbe&) = delete;

  GameModeService service() const { return service_; }
  const char* name() const { return name_; }
  // Inside Flatpak the probe asks for the GameMode portal, which any
  // desktop with portals has: it says nothing about the host's gamemoded.
  bool portal() const { return portal_; }

  void Start() {
    const char* flatpak_id = std::getenv("FLATPAK_ID");
    if ((flatpak_id != nullptr && flatpak_id[0] != '\0') ||
        access("/.flatpak-info", F_OK) == 0) {
      name_ = "org.freedesktop.portal.Desktop";
      portal_ = true;
    }
    // GIO falls back to autolaunching a bus through X11; ask only an
    // existing one.
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::error_code error;
    const bool has_bus = (address != nullptr && address[0] != '\0') ||
                         (runtime != nullptr && runtime[0] != '\0' &&
                          std::filesystem::exists(
                              std::filesystem::path(runtime) / "bus", error));
    if (!has_bus) {
      Finish(GameModeService::kNoBus);
      return;
    }
    g_bus_get(G_BUS_TYPE_SESSION, cancellable_, OnBus, this);
  }

 private:
  void Finish(GameModeService service) {
    service_ = service;
    // The rows read the result like the rest of the machine profile.
    context_->NotifyMachineChanged();
  }

  static bool Cancelled(GError* error) {
    const bool cancelled =
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (cancelled) g_error_free(error);
    return cancelled;
  }

  static void OnBus(GObject*, GAsyncResult* result, gpointer data) {
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_finish(result, &error);
    if (bus == nullptr) {
      if (Cancelled(error)) return;
      g_clear_error(&error);
      static_cast<GameModeProbe*>(data)->Finish(GameModeService::kNoBus);
      return;
    }
    auto* self = static_cast<GameModeProbe*>(data);
    self->bus_ = bus;
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "NameHasOwner",
                           g_variant_new("(s)", self->name_),
                           G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 3000,
                           self->cancellable_, OnHasOwner, self);
  }

  static void OnHasOwner(GObject* source, GAsyncResult* result, gpointer data) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);
    if (reply == nullptr && Cancelled(error)) return;
    auto* self = static_cast<GameModeProbe*>(data);
    gboolean owned = FALSE;
    if (reply != nullptr) {
      g_variant_get(reply, "(b)", &owned);
      g_variant_unref(reply);
    }
    g_clear_error(&error);
    if (owned) {
      self->Finish(GameModeService::kRunning);
      return;
    }
    g_dbus_connection_call(self->bus_, "org.freedesktop.DBus",
                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "ListActivatableNames", nullptr,
                           G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 3000,
                           self->cancellable_, OnActivatable, self);
  }

  static void OnActivatable(GObject* source, GAsyncResult* result,
                            gpointer data) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, &error);
    if (reply == nullptr && Cancelled(error)) return;
    auto* self = static_cast<GameModeProbe*>(data);
    bool activatable = false;
    if (reply != nullptr) {
      GVariantIter* names = nullptr;
      g_variant_get(reply, "(as)", &names);
      const gchar* name = nullptr;
      while (g_variant_iter_loop(names, "&s", &name)) {
        if (g_strcmp0(name, self->name_) == 0) activatable = true;
      }
      g_variant_iter_free(names);
      g_variant_unref(reply);
    }
    g_clear_error(&error);
    self->Finish(activatable ? GameModeService::kActivatable
                             : GameModeService::kMissing);
  }

  LauncherContext* context_;
  GCancellable* cancellable_;
  GDBusConnection* bus_ = nullptr;
  const char* name_ = "com.feralinteractive.GameMode";
  bool portal_ = false;
  GameModeService service_ = GameModeService::kChecking;
};

// performance.multithreaded_rendering (performance_policy.cc
// MergePerformanceClientSettingsOverrides; research/graphics.md 4.3).
GtkWidget* BuildMultithreadedRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kMultithreadedKey;
  spec.title = _("Multithreaded rendering");
  spec.keywords = {"threads",       "cores",          "cpu",  "scheduler",
                   "multicore",     "потоки",         "ядра", "процессор",
                   "многопоточный", "многопоточность"};
  spec.hint.subtitle_for = [](LauncherContext& ctx, const std::string& value) {
    const int cores = ctx.machine().physical_cores;
    const std::string physics = Physics(ctx);
    if (physics == "throughput") {
      return cores > 0
                 ? Format(ngettext("No extra effect: Throughput physics "
                                   "workers already use %d physical core",
                                   "No extra effect: Throughput physics "
                                   "workers already use all %d physical "
                                   "cores",
                                   static_cast<unsigned long>(cores)),
                          cores)
                 : std::string(_("No extra effect: Throughput physics workers "
                                 "already use every physical core"));
    }
    if (physics == "latency") {
      return std::string(
          _("No effect while Physics workers is set to Low latency"));
    }
    if (value != "true") {
      return std::string(_("Roblox sizes its own worker pools"));
    }
    return cores > 0
               ? Format(ngettext("Roblox's scheduler uses %d physical core; "
                                 "the performance preset is on",
                                 "Roblox's scheduler uses all %d physical "
                                 "cores; the performance preset is on",
                                 static_cast<unsigned long>(cores)),
                        cores)
               : std::string(_("Roblox's scheduler uses every physical core; "
                               "the performance preset is on"));
  };
  spec.hint.details =
      // performance_policy.cc DetectAvailablePhysicalCoreCount: physical
      // cores within the process's CPU affinity.
      _("Sizes Roblox's task scheduler from this computer's physical "
        "processor cores (the cores Mocktail may use, without counting "
        "hyper-threads) instead of Roblox's own choice. A place's Lua code "
        "can still run on a single thread.") +
      std::string("\n\n") +
      _("It only matters while Physics workers is set to Automatic: "
        "Throughput already gives Roblox every physical core, and Low "
        "latency ignores this setting.") +
      "\n\n" +
      // performance_policy.cc: auto + multithreaded merges the preset.
      _("With Automatic physics workers, turning it on also turns on "
        "Mocktail's performance preset: less texture memory, no MSAA, "
        "simpler shadows and the graphics quality level from Graphics.") +
      "\n\n" +
      _("More worker threads help busy places reach a higher frame rate; "
        "on a computer doing other heavy work at the same time, they compete "
        "with it for the processor.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MachineProfile& machine = ctx.machine();
    if (machine.physical_cores <= 0) return std::string();
    return Format(_("Physical cores Mocktail may use here: %d (logical "
                    "processors: %d)."),
                  machine.physical_cores, machine.logical_cpus);
  };
  // No recommendation: it only matters with Automatic physics workers, and
  // there turning it on also brings back the performance preset that
  // Automatic avoids, a trade only the player can weigh.
  spec.hint.overrides_roblox = RobloxOverrideHint(kMultithreadedKey);
  return BindSwitchRow(context, std::move(spec));
}

// performance.physics_worker_mode (performance_policy.cc).
GtkWidget* BuildPhysicsRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kPhysicsKey;
  spec.title = _("Physics workers");
  spec.fallback = "throughput";
  spec.keywords = {
      "physics", "workers", "throughput", "latency", "preset",
      "физика",  "потоки",  "задержка",   "пресет",  "производительность"};
  spec.hint.details =
      _("How Mocktail tunes Roblox's worker threads for physics, render "
        "preparation and background tasks. The change applies the next time "
        "Roblox starts.") +
      std::string("\n\n") +
      // performance_policy.cc: throughput scheduler settings and
      // rendering_settings (MSAA 1, texture budgets, shadow mips, 32 sound
      // channels, FRM level).
      _("• Throughput (default): Roblox's worker pools may use every "
        "physical core, and Mocktail's performance preset is on: less texture "
        "memory, anti-aliasing (MSAA) off, simpler shadows and level of "
        "detail, at most 32 sounds at once, and the graphics quality level "
        "from Graphics.") +
      "\n\n" +
      // performance_policy.cc: latency only sets
      // DFIntSimMidPhaseContactPipelineBatchSize.
      // performance_policy.cc MergeTextureMemoryClientSettingsOverrides
      // runs for every mode.
      _("• Low latency: Roblox keeps its own pool sizes; Mocktail only "
        "batches physics contact work. The preset stays off, so Roblox's own "
        "graphics quality and texture limits apply (Mocktail still sizes "
        "Roblox's video memory from this computer's RAM).") +
      "\n\n" +
      _("• Automatic: no tuning at all, unless Multithreaded rendering is "
        "on; that sizes the scheduler from the physical cores and turns the "
        "preset on.") +
      "\n\n" +
      _("Keep Throughput for Mocktail's tuned, lighter rendering. Choose Low "
        "latency or Automatic to run Roblox closer to its own defaults, for "
        "example when textures look too blurry.");
  spec.hint.overrides_roblox = RobloxOverrideHint(kPhysicsKey);
  ComboSpec combo;
  combo.options = {
      {"throughput",
       _("Throughput"),
       {},
       {},
       [](LauncherContext& ctx) {
         const int cores = ctx.machine().physical_cores;
         return cores > 0
                    ? Format(ngettext("Up to %d physical core for Roblox's "
                                      "workers, plus the performance preset",
                                      "All %d physical cores for Roblox's "
                                      "workers, plus the performance preset",
                                      static_cast<unsigned long>(cores)),
                             cores)
                    : std::string(_("Every physical core for Roblox's "
                                    "workers, plus the performance preset"));
       },
       nullptr,
       false,
       false},
      {"latency",
       _("Low latency"),
       _("Roblox keeps its own pool sizes; only physics batching is tuned"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"auto",
       _("Automatic"),
       _("No Mocktail tuning unless Multithreaded rendering is on"),
       {},
       nullptr,
       nullptr,
       false,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

std::string GameModeStatus(const LauncherContext& context,
                           const GameModeProbe& probe) {
  const MachineProfile& machine = context.machine();
  // machine_profile.cc looks in fixed directories; the game's dlmopen()
  // also searches the loader's own path (NixOS and others).
  if (machine.detected && !machine.gamemode_library) {
    return _("libgamemode.so.0 was not found in the usual places");
  }
  switch (probe.service()) {
    case GameModeService::kRunning:
    case GameModeService::kActivatable:
      if (probe.portal()) {
        return _("The GameMode portal is available; whether GameMode is "
                 "installed on the host could not be checked");
      }
      return _("GameMode is installed and its service is available");
    case GameModeService::kMissing:
      return _("libgamemode is installed, but no GameMode service was found");
    case GameModeService::kNoBus:
      return _("libgamemode is installed; its service could not be checked");
    case GameModeService::kChecking:
      break;
  }
  return machine.detected ? _("libgamemode is installed")
                          : _("Looking for GameMode…");
}

// performance.gamemode (game_mode.cc, main.cc GameModeSession).
GtkWidget* BuildGameModeRow(LauncherContext* context,
                            std::shared_ptr<GameModeProbe> probe) {
  RowSpec spec;
  spec.key = kGameModeKey;
  spec.title = "GameMode";
  spec.fallback = "auto";
  spec.keywords = {"gamemode",    "feral", "gamemoded",
                   "gamemoderun", "режим", "игровой режим"};
  spec.hint.subtitle_for = [probe](LauncherContext& ctx,
                                   const std::string& value) {
    if (value == "off") return std::string(_("Never asks GameMode"));
    return GameModeStatus(ctx, *probe);
  };
  spec.hint.details =
      _("Feral GameMode is a system service that switches the computer into "
        "a faster mode while a game runs, for example the processor's "
        "performance governor; what exactly depends on its own "
        "configuration.") +
      std::string("\n\n") +
      // main.cc: GameModeSession::Start in the final game process;
      // game_mode.cc dlmopen()s libgamemode.so.0. Never fatal.
      _("Mocktail asks for it from the game process itself through "
        "libgamemode, so no wrapper such as gamemoderun is needed. A missing "
        "GameMode never stops Roblox from starting.") +
      "\n\n" +
      _("• Automatic and On send the same request; On only adds a warning to "
        "the log when GameMode is missing.") +
      "\n\n" + _("• Off never asks for it.");
  spec.hint.details_for = [probe](LauncherContext& ctx) {
    const MachineProfile& machine = ctx.machine();
    std::string text;
    if (machine.detected) {
      text = machine.gamemode_library
                 ? std::string(_("libgamemode.so.0 is installed."))
                 : std::string(_("libgamemode.so.0 was not found in the "
                                 "usual library folders; install your "
                                 "distribution's gamemode package to use "
                                 "GameMode."));
    }
    std::string service;
    switch (probe->service()) {
      case GameModeService::kRunning:
        service = Format(_("%s is running."), probe->name());
        break;
      case GameModeService::kActivatable:
        service = Format(_("%s starts on the first request."), probe->name());
        break;
      case GameModeService::kMissing:
        service = Format(_("%s is not on the session bus."), probe->name());
        break;
      case GameModeService::kNoBus:
        service =
            _("There is no session bus to ask about the GameMode "
              "service.");
        break;
      case GameModeService::kChecking:
        break;
    }
    if (!service.empty()) {
      if (!text.empty()) text += " ";
      text += service;
    }
    return text;
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return machine.detected ? std::optional<std::string>("auto") : std::nullopt;
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    return machine.gamemode_library
               ? std::string(_("GameMode is installed, and Automatic uses it "
                               "whenever its service runs."))
               : std::string(_("Automatic costs nothing without GameMode and "
                               "starts using it once it is installed."));
  };
  spec.hint.warning = [probe](LauncherContext& ctx, const std::string& value) {
    const MachineProfile& machine = ctx.machine();
    if (value == "on" && machine.detected && !machine.gamemode_library) {
      return std::string(
          _("libgamemode.so.0 was not found, so On will probably only add a "
            "warning to the log."));
    }
    if (value != "off" && machine.gamemode_library &&
        probe->service() == GameModeService::kMissing) {
      return std::string(
          _("No GameMode service answers on the session bus, so the request "
            "will fail; Roblox still starts."));
    }
    return std::string();
  };
  ComboSpec combo;
  combo.options = {
      {"auto",
       _("Automatic"),
       _("Asks GameMode for more performance when it is installed"),
       // game_mode.cc ParseGameModePolicy aliases.
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"on",
       _("On"),
       _("The same request; the log warns when GameMode is missing"),
       {"1", "true"},
       nullptr,
       nullptr,
       false,
       false},
      {"off",
       _("Off"),
       _("Never asks GameMode"),
       {"0", "false"},
       nullptr,
       nullptr,
       false,
       false},
  };
  // No detection badge ("Installed", "No service") among the suffixes: the
  // subtitle (GameModeStatus) says the same and a missing service is also
  // the row's warning, so the badge was the third copy on one row.
  return BindComboRow(context, std::move(spec), std::move(combo));
}

// performance.memory_limit_mb (memory_limit.h/.cc: an RSS + swap watchdog;
// the cgroup path is disabled). An expander with an enable switch around a
// limit in GiB, stored in MiB (research/ux.md 4.3).
class MemoryLimitRow {
 public:
  explicit MemoryLimitRow(LauncherContext* context) : context_(context) {}
  ~MemoryLimitRow() { context_->RemoveListener(listener_); }
  MemoryLimitRow(const MemoryLimitRow&) = delete;
  MemoryLimitRow& operator=(const MemoryLimitRow&) = delete;

  GtkWidget* Build() {
    expander_ = adw_expander_row_new();
    // AdwExpanderRow's own enable switch sits after the row's suffixes, so
    // the info button DecorateRow adds came before it, unlike on every
    // other row. A switch of our own, added first, keeps the info button
    // last; it drives enable-expansion as the built-in one does.
    GtkWidget* enable = gtk_switch_new();
    gtk_widget_set_valign(enable, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(enable),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Memory limit"), -1);
    g_object_bind_property(
        enable, "active", expander_, "enable-expansion",
        GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
    adw_expander_row_add_suffix(ADW_EXPANDER_ROW(expander_), enable);
    spin_ = adw_spin_row_new_with_range(0.5, 1024, 0.5);
    adw_spin_row_set_digits(ADW_SPIN_ROW(spin_), 1);
    // In the user's decimal separator, both ways ("6,5" in Russian): the
    // spin button formats and parses with LC_NUMERIC, which is "C".
    g_signal_connect(
        spin_, "output", G_CALLBACK(+[](AdwSpinRow* row, gpointer) -> gboolean {
          const std::string text = DecimalText(adw_spin_row_get_value(row), 1);
          gtk_editable_set_text(GTK_EDITABLE(row), text.c_str());
          return TRUE;
        }),
        nullptr);
    g_signal_connect(
        spin_, "input",
        G_CALLBACK(+[](AdwSpinRow* row, double* value, gpointer) -> gint {
          std::string text = gtk_editable_get_text(GTK_EDITABLE(row));
          std::replace(text.begin(), text.end(), ',', '.');
          char* end = nullptr;
          const double parsed = g_ascii_strtod(text.c_str(), &end);
          if (end == text.c_str()) return GTK_INPUT_ERROR;
          *value = parsed;
          return TRUE;
        }),
        nullptr);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin_), _("Limit"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(spin_),
                                _("GiB of resident memory plus swap"));
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander_), spin_);
    g_signal_connect(expander_, "notify::enable-expansion",
                     G_CALLBACK(OnEnabled), this);
    g_signal_connect(spin_, "notify::value", G_CALLBACK(OnValue), this);
    g_object_set_data_full(
        G_OBJECT(expander_), "mocktail-memory-limit", this,
        [](gpointer data) { delete static_cast<MemoryLimitRow*>(data); });

    RowSpec spec;
    spec.key = kMemoryKey;
    spec.title = _("Memory limit");
    spec.keywords = {"memory", "ram",    "limit", "watchdog", "oom",
                     "swap",   "память", "озу",   "лимит",    "ограничение"};
    spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
      const std::uint64_t mebibytes = ParseMebibytes(value).value_or(0);
      if (mebibytes == 0) {
        return std::string(
            _("Off: the game may use as much memory as it needs"));
      }
      return Format(_("Mocktail ends the game when it uses more than %s"),
                    GibibytesText(mebibytes / kMebibytesPerGibibyte).c_str());
    };
    spec.hint.details =
        // memory_limit.cc: kWatchdogPollMilliseconds = 100, _Exit(137).
        _("A hard cap on the memory the game process may use, counted as its "
          "resident memory plus swap. A watchdog checks it ten times a second "
          "and ends Mocktail with exit status 137 when the cap is reached, so "
          "anything unsaved in the experience is lost.") +
        std::string("\n\n") +
        // memory_limit.cc: automatic cgroup scopes are disabled.
        _("Swap stays enabled; the cap only decides when the game is "
          "stopped. Leave it off unless Roblox has made this computer run out "
          "of memory.") +
        "\n\n" +
        // config/mocktail.example.yaml: 6144 for 32 GiB RAM.
        _("A cap that is too low ends the game while it loads or in large "
          "experiences; 6 GiB is a cautious start on a computer with 32 GiB.");
    spec.hint.details_for = [](LauncherContext& ctx) {
      const std::uint64_t memory = ctx.machine().memory_bytes;
      if (memory == 0) return std::string();
      return Format(
          _("This computer has %s of memory; switching the limit on starts "
            "at %s."),
          GibibytesText(static_cast<double>(memory) /
                        (1024.0 * 1024.0 * 1024.0))
              .c_str(),
          GibibytesText(SuggestedMemoryLimitMiB(memory) / kMebibytesPerGibibyte)
              .c_str());
    };
    spec.hint.warning = [](LauncherContext& ctx, const std::string& value) {
      const std::uint64_t mebibytes = ParseMebibytes(value).value_or(0);
      const std::uint64_t memory = ctx.machine().memory_bytes;
      if (mebibytes != 0 && mebibytes < 2048) {
        return std::string(
            _("Very low: Roblox may be stopped while it is still loading."));
      }
      if (mebibytes != 0 && memory != 0 && mebibytes * 1024U * 1024U > memory) {
        // memory_limit.cc counts resident memory plus swap.
        return std::string(
            _("Larger than this computer's memory: the game is stopped only "
              "after much of it has moved to swap."));
      }
      return std::string();
    };
    DecorateRow(context_, expander_, std::move(spec));
    listener_ = context_->OnSettingChanged([this](std::string_view key) {
      if (key.empty() || key == kMemoryKey) Sync();
    });
    Sync();
    return expander_;
  }

 private:
  std::uint64_t Current() const {
    return ParseMebibytes(context_->EffectiveValue(kMemoryKey, "0"))
        .value_or(0);
  }

  void Sync() {
    updating_ = true;
    const std::uint64_t mebibytes = Current();
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(expander_),
                                          mebibytes != 0);
    if (mebibytes != 0) {
      last_enabled_ = mebibytes;
      adw_spin_row_set_value(ADW_SPIN_ROW(spin_),
                             mebibytes / kMebibytesPerGibibyte);
    }
    updating_ = false;
  }

  void Write(std::uint64_t mebibytes) {
    if (!context_->SetValue(kMemoryKey, std::to_string(mebibytes),
                            launcher::ScalarKind::kInteger)) {
      Sync();
    }
  }

  static void OnEnabled(GObject*, GParamSpec*, gpointer data) {
    auto* self = static_cast<MemoryLimitRow*>(data);
    if (self->updating_) return;
    const bool enabled = adw_expander_row_get_enable_expansion(
        ADW_EXPANDER_ROW(self->expander_));
    if (!enabled) {
      self->Write(0);
      return;
    }
    std::uint64_t mebibytes = self->last_enabled_;
    if (mebibytes == 0) {
      mebibytes =
          SuggestedMemoryLimitMiB(self->context_->machine().memory_bytes);
    }
    self->Write(mebibytes);
    adw_expander_row_set_expanded(ADW_EXPANDER_ROW(self->expander_), TRUE);
  }

  static void OnValue(GObject*, GParamSpec*, gpointer data) {
    auto* self = static_cast<MemoryLimitRow*>(data);
    if (self->updating_ || self->Current() == 0) return;
    const double gibibytes = adw_spin_row_get_value(ADW_SPIN_ROW(self->spin_));
    self->Write(static_cast<std::uint64_t>(
        std::llround(gibibytes * kMebibytesPerGibibyte)));
  }

  LauncherContext* context_;
  LauncherContext::ListenerId listener_ = 0;
  GtkWidget* expander_ = nullptr;
  GtkWidget* spin_ = nullptr;
  std::uint64_t last_enabled_ = 0;
  bool updating_ = false;
};

}  // namespace

// Performance: how Roblox uses the processor and memory, and GameMode.
GtkWidget* BuildPerformancePage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kPerformance,
                            _("How Roblox uses your processor and memory, and "
                              "whether Feral GameMode is requested"));
  GtkWidget* processor =
      AddGroup(page, _("Processor"),
               _("How Roblox spreads its work over the processor cores"));
  AddRow(processor, BuildMultithreadedRow(context));
  GtkWidget* advanced = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(advanced),
                                _("Advanced processor options"));
  adw_expander_row_set_subtitle(
      ADW_EXPANDER_ROW(advanced),
      _("Physics workers and the performance preset"));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(advanced),
                           BuildPhysicsRow(context));
  AddRow(processor, advanced);

  // The overview lives on the Advanced page; the preset decided here is
  // most of it.
  GtkWidget* roblox =
      AddGroup(page, _("Roblox's own settings"),
               _("Which of them the settings in this window take over"));
  AddRow(roblox, BuildRobloxDecidesLinkRow(context));

  GtkWidget* system =
      AddGroup(page, _("System"),
               _("Help from the operating system, and a safety cap on "
                 "memory"));
  auto probe = std::make_shared<GameModeProbe>(context);
  AddRow(system, BuildGameModeRow(context, probe));
  // The probe lives as long as the rows that read it.
  g_object_set_data_full(
      G_OBJECT(page), "mocktail-gamemode-probe",
      new std::shared_ptr<GameModeProbe>(probe), [](gpointer data) {
        delete static_cast<std::shared_ptr<GameModeProbe>*>(data);
      });
  probe->Start();
  AddRow(system, (new MemoryLimitRow(context))->Build());
  return page;
}

}  // namespace mocktail::launcher_ui
