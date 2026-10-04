#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/page_dialogs.h"
#include "launcher_ui/page_rules.h"
#include "launcher_ui/page_widgets.h"
#include "launcher_ui/pages.h"
#include "runtime/environment.h"

// The Discord application this build sends activity for when config.yaml
// sets none (CMakeLists.txt MOCKTAIL_DISCORD_APPLICATION_ID, the same value
// runtime_config.cc compiles in).
#ifndef MOCKTAIL_DISCORD_APPLICATION_ID
#define MOCKTAIL_DISCORD_APPLICATION_ID ""
#endif

namespace mocktail::launcher_ui {
namespace {

constexpr char kRpcEnabled[] = "integrations.discord_rpc.enabled";
constexpr char kJoinEnabled[] = "integrations.discord_rpc.join.enabled";
constexpr char kButtonLabel[] = "integrations.discord_rpc.join.button_label";
constexpr char kApplicationId[] = "integrations.discord_rpc.application_id";
constexpr char kStateText[] = "integrations.discord_rpc.text.state";
constexpr char kFleasionEnabled[] = "integrations.fleasion.enabled";
constexpr char kFleasionMode[] = "integrations.fleasion.proxy_mode";
constexpr char kFleasionPort[] = "integrations.fleasion.proxy_port";
constexpr char kFleasionCertificate[] = "integrations.fleasion.ca_certificate";

// The runtime defaults of the texts the template leaves commented out
// (runtime_config.h DiscordRpcConfig / DiscordRpcTextConfig).
constexpr char kDefaultButtonLabel[] = "Join Server";
constexpr char kDefaultStateText[] = "Playing Roblox";

// Keys the "Customize texts" dialog edits, for its summary and search.
constexpr const char* kTextKeys[] = {
    "integrations.discord_rpc.join.button_label",
    "integrations.discord_rpc.text.browsing",
    "integrations.discord_rpc.text.joining",
    "integrations.discord_rpc.text.playing",
    "integrations.discord_rpc.text.state",
    "integrations.discord_rpc.text.unknown_place",
    "integrations.discord_rpc.application_id",
};

std::string BundledApplicationId() { return MOCKTAIL_DISCORD_APPLICATION_ID; }

bool IsOn(const LauncherContext& context, const char* key,
          const char* fallback) {
  return context.EffectiveValue(key, fallback) == "true";
}

std::filesystem::path FleasionCertificatePath(const LauncherContext& context) {
  const std::optional<std::string> configured =
      context.Value(kFleasionCertificate);
  if (configured.has_value() && !configured->empty()) return *configured;
  return DefaultFleasionCertificate(runtime::ProcessEnvironment());
}

bool Exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

std::string ProblemForText(const std::string& text, std::size_t limit) {
  switch (CheckDiscordText(text, limit)) {
    case DiscordTextProblem::kEmpty:
      return _("Enter a text, or use the reset button for the default");
    case DiscordTextProblem::kTooLong:
      return Format(_("Too long: %zu of %zu bytes (letters outside the Latin "
                      "alphabet take two or more)"),
                    text.size(), limit);
    case DiscordTextProblem::kControlCharacter:
      return _("Line breaks and other control characters are not allowed");
    case DiscordTextProblem::kNone:
      break;
  }
  return {};
}

// ---- Discord ----------------------------------------------------------------

GtkWidget* BuildRichPresenceRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kRpcEnabled;
  spec.title = _("Discord Rich Presence");
  spec.fallback = "false";
  spec.keywords = {"discord",    "rpc",        "rich presence", "presence",
                   "status",     "activity",   "дискорд",       "статус",
                   "активность", "присутствие"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("Shows what you play in the Discord app on this "
                               "computer"))
               : std::string(_("Off: Discord shows nothing; the options below "
                               "apply once you turn it on"));
  };
  spec.hint.details =
      // discord_rpc.cc BuildDiscordRpcActivity.
      _("Shows your Roblox activity on your Discord profile: whether you are "
        "browsing, joining or playing, the experience's name and icon, and "
        "for how long.") +
      std::string("\n\n") +
      // discord_rpc.cc DiscordSocketDirectories / ConnectDiscord (local
      // discord-ipc sockets, Flatpak and Snap paths); the template comment
      // "never signs in to Discord and never reads an account token".
      _("Mocktail talks only to the Discord app running on this computer, "
        "through its local connection; the Flatpak and Snap versions of "
        "Discord are found too. It never signs in to Discord and never reads "
        "a Discord token.") +
      "\n\n" +
      // discord_rpc.cc GetRobloxJson: apis/games/thumbnails.roblox.com,
      // through MOCKTAIL_HTTP_PROXY_*.
      _("To show an experience's name and icon, Mocktail looks them up on "
        "Roblox's public web API without signing in, through the proxy set "
        "under Network & Updates.") +
      "\n\n" +
      // discord_rpc.cc: its own worker thread, kReconnectDelay 5 s.
      _("It runs on its own background thread and retries every few seconds "
        "while Discord is closed, so it does not slow the game down. Leave it "
        "off if you do not want friends to see what you play.");
  spec.hint.warning = [](LauncherContext& context, const std::string& value) {
    // discord_rpc.cc Start: "no Discord application ID is bundled".
    if (value == "true" && BundledApplicationId().empty() &&
        !context.Value(kApplicationId).has_value()) {
      return std::string(
          _("This build of Mocktail has no Discord application ID. Set one "
            "under “Customize texts”, or Discord shows nothing."));
    }
    return std::string();
  };
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildShowPlaceNameRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "integrations.discord_rpc.show_place_name";
  spec.title = _("Show experience name");
  spec.fallback = "true";
  spec.keywords = {"place",    "game name", "experience", "название игры",
                   "название", "место",     "игра"};
  spec.hint.subtitle_for = [](LauncherContext& context,
                              const std::string& value) {
    if (value == "true") {
      return std::string(_("Your status names the experience you play"));
    }
    return Format(
        _("Off: the first line reads “%s” instead"),
        context.EffectiveValue(kStateText, kDefaultStateText).c_str());
  };
  spec.hint.details =
      _("When on, the first line of your status while you play is the "
        "experience's name (the “While playing” text, which is just the name "
        "unless you change it).") +
      std::string("\n\n") +
      // discord_rpc.cc:561-573: details = state, state cleared; the place
      // icon and its name as hover text are set either way.
      _("When off, that line shows the second-line text (“Playing Roblox” "
        "by default) instead. The experience's icon is still shown, and "
        "Discord shows its name when someone points at the icon.") +
      "\n\n" + _("Browsing and joining look the same either way.");
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildShowTimeRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "integrations.discord_rpc.show_elapsed_time";
  spec.title = _("Show play time");
  spec.fallback = "true";
  spec.keywords = {"elapsed", "time", "timer", "время", "таймер"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("Discord counts how long you have been in the "
                               "current experience"))
               : std::string(_("Off: no timer in your status"));
  };
  // discord_rpc.cc: session_started_at_ is set when the playing phase
  // starts and cleared otherwise; start_timestamp only while playing.
  spec.hint.details =
      _("Adds Discord's elapsed-time counter to your status. It starts when "
        "you enter an experience and starts again in the next one; nothing "
        "is shown while you browse.");
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildJoinRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kJoinEnabled;
  spec.title = _("Let friends join");
  spec.fallback = "true";
  spec.keywords = {"join",           "button", "friends", "invite",
                   "присоединиться", "кнопка", "друзья",  "пригласить"};
  spec.hint.subtitle_for = [](LauncherContext& context,
                              const std::string& value) {
    if (value != "true") return std::string(_("Off: no join button"));
    return Format(
        _("Adds a “%s” button to your status while you play"),
        context.EffectiveValue(kButtonLabel, kDefaultButtonLabel).c_str());
  };
  spec.hint.details =
      // discord_rpc.cc kJoinPage and BuildDiscordJoinUrl (placeId, and the
      // gameInstanceId when Roblox reported one).
      _("Adds a button to your status while you play. A friend who clicks it "
        "opens a page on komaruworld.github.io that starts Roblox in the same "
        "experience, on your server when Roblox reported which one you are "
        "on.") +
      std::string("\n\n") +
      // BuildDiscordJoinUrl never includes access or link codes.
      _("The button never contains private server access codes. “Public "
        "servers only” decides whether it also appears on private and "
        "reserved servers.");
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildPublicOnlyRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "integrations.discord_rpc.join.public_servers_only";
  spec.title = _("Public servers only");
  spec.fallback = "true";
  spec.keywords = {"private",   "reserved",    "privacy",  "public",
                   "приватный", "приватность", "публичный"};
  spec.unavailable = [](LauncherContext& context) {
    return IsOn(context, kJoinEnabled, "true")
               ? std::string()
               : std::string(_("Turn on “Let friends join” to use this"));
  };
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("No join button on private or reserved servers"))
               : std::string(
                     _("The join button also appears on private and reserved "
                       "servers"));
  };
  spec.hint.details =
      // discord_rpc.cc IsPublicDiscordJoin: no reserved-server access code,
      // access code or link code.
      _("When on, the join button is left out while you are on a private "
        "server, a reserved server (some experiences use them for matches) or "
        "a server you joined through a link.") +
      std::string("\n\n") +
      _("When off, friends see the button there too. It carries the "
        "experience and the server, never an access code, so it mostly "
        "tells them where you are.") +
      "\n\n" + _("Recommended: on.");
  return BindSwitchRow(context, std::move(spec));
}

int ChangedTextCount(const LauncherContext& context) {
  int count = 0;
  for (const char* key : kTextKeys) {
    if (context.Value(key).has_value()) ++count;
  }
  return count;
}

GtkWidget* BuildTextsRow(LauncherContext* context) {
  GtkWidget* row = NewActionRow("go-next-symbolic",
                                [context] { OpenDiscordTextsDialog(context); });
  RowSpec spec;
  spec.title = _("Customize texts");
  spec.keywords = {"text",        "texts",          "language",  "button label",
                   "application", "application id", "browsing",  "joining",
                   "playing",     "unknown place",  "текст",     "тексты",
                   "язык",        "надпись",        "приложение"};
  for (const char* key : kTextKeys) spec.keywords.emplace_back(key);
  for (const char* name :
       {"MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
        "MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
        "MOCKTAIL_DISCORD_RPC_TEXT_JOINING",
        "MOCKTAIL_DISCORD_RPC_TEXT_PLAYING", "MOCKTAIL_DISCORD_RPC_TEXT_STATE",
        "MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
        "MOCKTAIL_DISCORD_APPLICATION_ID"}) {
    spec.keywords.emplace_back(name);
  }
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    const int count = ChangedTextCount(context);
    if (count == 0) {
      return std::string(_("Mocktail's English texts and Discord application"));
    }
    return Format(ngettext("%d text changed", "%d texts changed", count),
                  count);
  };
  // runtime_config_file.cc: 128 bytes per text, 32 for the button label.
  spec.hint.details =
      _("Rewords your Discord status and the join button, for example in "
        "your language. Each text holds up to 128 bytes and the button label "
        "32; letters outside the Latin alphabet take two or more bytes each.") +
      std::string("\n\n") +
      _("It also sets the Discord application whose name Discord shows as "
        "the game you are playing.");
  return DecorateRow(context, row, std::move(spec));
}

// ---- the texts dialog -------------------------------------------------------

struct TextField {
  const char* key;
  std::string title;
  const char* fallback;
  std::size_t limit;
  std::string details;
};

GtkWidget* BuildTextRow(LauncherContext* context, const TextField& field) {
  RowSpec spec;
  spec.key = field.key;
  spec.title = field.title;
  spec.fallback = field.fallback;
  spec.keywords = {"discord", "text", "текст"};
  const std::string fallback = field.fallback;
  const std::string key = field.key;
  spec.hint.subtitle_for = [fallback, key](LauncherContext&,
                                           const std::string& value) {
    if (key == "integrations.discord_rpc.text.playing") {
      // A preview with a well-known experience name.
      return Format(_("Shows as “%s”"),
                    RenderDiscordPlaceText(value, "Tower of Hell").c_str());
    }
    return Format(_("Default: “%s”"), fallback.c_str());
  };
  spec.hint.details = field.details;
  EntrySpec entry;
  const std::size_t limit = field.limit;
  entry.validate = [limit](const std::string& text) {
    return ProblemForText(text, limit);
  };
  return BindEntryRow(context, std::move(spec), std::move(entry));
}

GtkWidget* BuildApplicationIdRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kApplicationId;
  spec.title = _("Discord application ID");
  spec.keywords = {"discord", "application", "client id", "snowflake",
                   "приложение"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    if (!value.empty()) return std::string(_("Your own Discord application"));
    return BundledApplicationId().empty()
               ? std::string(
                     _("Empty: this build has no application of its own"))
               : Format(_("Empty: Mocktail's application, %s"),
                        BundledApplicationId().c_str());
  };
  // General Discord Rich Presence behaviour: activity type 0 ("Playing")
  // is shown under the application's name (discord_rpc.cc:333).
  spec.hint.details =
      _("Discord shows the name and artwork of this Discord application as "
        "the game you are playing. Leave it empty to use the application "
        "bundled with Mocktail.") +
      std::string("\n\n") +
      _("Only enter the ID of an application you created in Discord's "
        "developer portal: 17 to 20 digits. With a wrong ID Discord shows "
        "nothing.");
  EntrySpec entry;
  entry.empty_unsets = true;
  entry.validate = [](const std::string& text) {
    return text.empty() || IsDiscordApplicationId(text)
               ? std::string()
               : std::string(_("A Discord application ID is 17 to 20 digits"));
  };
  return BindEntryRow(context, std::move(spec), std::move(entry));
}

// ---- Fleasion ---------------------------------------------------------------

GtkWidget* BuildFleasionRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kFleasionEnabled;
  spec.title = _("Use Fleasion");
  spec.fallback = "false";
  spec.keywords = {"fleasion",   "asset", "assets",  "certificate",
                   "ca",         "mod",   "mods",    "ассеты",
                   "сертификат", "моды",  "текстуры"};
  spec.unavailable = [](LauncherContext& context) {
    // Turning it off is always possible.
    if (IsOn(context, kFleasionEnabled, "false")) return std::string();
    const runtime::ProcessEnvironment environment;
    const std::filesystem::path directory =
        FleasionConfigDirectory(environment);
    if (!context.Value(kFleasionCertificate).has_value() &&
        !Exists(directory)) {
      return Format(
          _("Fleasion was not found (%s does not exist); run Fleasion once "
            "first"),
          DisplayPath(directory, environment.GetOr("HOME", "")).c_str());
    }
    FleasionInputs inputs = FleasionInputsFrom(context);
    inputs.enabled = true;
    const FleasionConflict conflict = FindFleasionConflict(inputs);
    if (conflict != FleasionConflict::kNone) {
      return DescribeFleasionConflict(conflict) + ". " +
             _("Change the proxy under Network & Updates first");
    }
    return std::string();
  };
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("Roblox trusts Fleasion's certificate; start "
                               "Fleasion before you play"))
               : std::string(_("Trusts Fleasion's certificate so the assets "
                               "it replaces load; set up the rows below "
                               "first"));
  };
  spec.hint.warning = [](LauncherContext& context, const std::string& value) {
    if (value != "true") return std::string();
    const FleasionConflict conflict =
        FindFleasionConflict(FleasionInputsFrom(context));
    if (conflict != FleasionConflict::kNone) {
      // runtime_config_file.cc: "Fleasion configuration is invalid".
      return Format(_("%s, so Mocktail will not start. Turn one of them off."),
                    DescribeFleasionConflict(conflict).c_str());
    }
    const std::filesystem::path certificate = FleasionCertificatePath(context);
    if (!Exists(certificate)) {
      // fleasion.cc PrepareFleasion: "cannot read Fleasion CA certificate".
      return Format(_("Fleasion's certificate %s was not found, so Mocktail "
                      "will not start. Start Fleasion once, or choose its "
                      "ca.crt below."),
                    certificate.c_str());
    }
    return std::string();
  };
  spec.hint.details =
      // The template comment and fleasion.cc PrepareFleasion.
      _("Fleasion is a separate tool that swaps Roblox assets (sounds, "
        "textures and more) through a local HTTPS proxy with its own "
        "certificate authority. With this on, Mocktail adds Fleasion's "
        "certificate to the ones Roblox trusts, so the replaced assets load, "
        "without changing any Roblox files.") +
      std::string("\n\n") +
      // fleasion.cc: cache_root/fleasion/cacert.pem rebuilt at each start;
      // main.cc prints "start Fleasion before Roblox".
      _("At each start Mocktail combines your system's certificates with "
        "Fleasion's into a file in its cache folder and uses it for every "
        "connection. Start Fleasion before you press Play.") +
      "\n\n" +
      _("Only turn this on if you use Fleasion and trust your copy of it: "
        "whatever holds its certificate can read and change Roblox's "
        "encrypted traffic. Turn it off when you stop using Fleasion.") +
      "\n\n" +
      // runtime_config.cc fleasion_valid_; main.cc PrepareFleasion fails
      // fatally.
      _("It cannot be combined with the system proxy, nor with a manual proxy "
        "other than Fleasion's own. Without a readable certificate Mocktail "
        "stops at start and says why.");
  spec.hint.details_for = [](LauncherContext& context) {
    const runtime::ProcessEnvironment environment;
    const std::filesystem::path directory =
        FleasionConfigDirectory(environment);
    const std::filesystem::path certificate = FleasionCertificatePath(context);
    return (Exists(directory)
                ? Format(_("Fleasion's folder on this computer: %s"),
                         directory.c_str())
                : Format(_("Fleasion's folder %s does not exist on this "
                           "computer."),
                         directory.c_str())) +
           "\n\n" +
           (Exists(certificate)
                ? Format(_("Certificate found: %s"), certificate.c_str())
                : Format(_("No certificate at %s yet."), certificate.c_str()));
  };
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildFleasionModeRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kFleasionMode;
  spec.title = _("Routing");
  spec.fallback = "env";
  spec.keywords = {"proxy mode", "hosts", "env",
                   "routing",    "режим", "маршрутизация"};
  spec.hint.details =
      _("Match the routing mode chosen in Fleasion.") + std::string("\n\n") +
      // runtime_config.cc: env mode forces network_proxy_ to
      // 127.0.0.1:<proxy_port>; hosts mode adds no proxy.
      _("• Fleasion's proxy: Mocktail itself sends Roblox's web traffic to "
        "Fleasion's proxy on this computer.") +
      "\n\n" +
      _("• Hosts file: Fleasion points Roblox's servers at itself through the "
        "system's hosts file, and Mocktail only adds its certificate. A "
        "manual proxy cannot be used with this mode.");
  ComboSpec combo;
  combo.options = {
      {"env",
       _("Fleasion's proxy"),
       {},
       {},
       [](LauncherContext& context) {
         return Format(
             _("Roblox connects through 127.0.0.1:%s, Fleasion's local proxy"),
             context.EffectiveValue(kFleasionPort, "58443").c_str());
       },
       nullptr,
       false,
       false},
      {"hosts",
       _("Hosts file"),
       _("Fleasion redirects Roblox in the hosts file; Mocktail only trusts "
         "its certificate"),
       {},
       nullptr,
       nullptr,
       false,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

GtkWidget* BuildFleasionPortRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kFleasionPort;
  spec.title = _("Fleasion proxy port");
  spec.fallback = "58443";
  spec.keywords = {"port", "порт", "58443"};
  spec.unavailable = [](LauncherContext& context) {
    return context.EffectiveValue(kFleasionMode, "env") == "hosts"
               ? std::string(_("Only used with Fleasion's proxy routing"))
               : std::string();
  };
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return Format(_("Roblox connects to 127.0.0.1:%s"), value.c_str());
  };
  spec.hint.details =
      _("The port Fleasion's proxy listens on; use the number Fleasion shows. "
        "The default is 58443.");
  SpinSpec spin;
  spin.minimum = 1;
  spin.maximum = 65535;
  spin.unset_value = 58443;
  return BindSpinRow(context, std::move(spec), spin);
}

GtkWidget* BuildFleasionCertificateRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kFleasionCertificate;
  spec.title = _("Fleasion certificate");
  spec.keywords = {"ca.crt", "certificate", "ca", "сертификат"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    if (!value.empty()) return std::string(_("Fleasion's public certificate"));
    const runtime::ProcessEnvironment environment;
    return Format(_("Empty: %s"),
                  DisplayPath(DefaultFleasionCertificate(environment),
                              environment.GetOr("HOME", ""))
                      .c_str());
  };
  spec.hint.warning = [](LauncherContext&, const std::string& value) {
    return !value.empty() && !Exists(value)
               ? std::string(_("This file does not exist"))
               : std::string();
  };
  // The template: "Never select ca.key."
  spec.hint.details =
      _("Fleasion's public certificate, ca.crt. Leave it empty to use the "
        "place where Fleasion keeps it.") +
      std::string("\n\n") +
      _("Never choose ca.key: that is Fleasion's private key, which must stay "
        "secret.");
  spec.hint.details_for = [](LauncherContext&) {
    return Format(
        _("Fleasion's usual place: %s"),
        DefaultFleasionCertificate(runtime::ProcessEnvironment()).c_str());
  };
  EntrySpec entry;
  entry.empty_unsets = true;
  entry.validate = [](const std::string& text) {
    switch (CheckCertificatePath(text)) {
      case CertificatePathProblem::kRelative:
        return std::string(_("Use an absolute path, starting with /"));
      case CertificatePathProblem::kPrivateKey:
        return std::string(
            _("That is a private key; choose Fleasion's ca.crt instead"));
      case CertificatePathProblem::kNone:
        break;
    }
    return std::string();
  };
  GtkWidget* row = BindEntryRow(context, std::move(spec), std::move(entry));
  AddFileChooserButton(context, row, kFleasionCertificate,
                       _("Choose Fleasion's Certificate"));
  return row;
}

}  // namespace

void OpenDiscordTextsDialog(LauncherContext* context) {
  if (context->window() == nullptr) return;
  // Rows bound here are indexed under Integrations while they exist.
  context->BeginSection(Section::kIntegrations);
  AdwDialog* dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Discord Texts"));
  adw_dialog_set_content_width(dialog, 600);
  adw_dialog_set_content_height(dialog, 680);

  GtkWidget* page = adw_preferences_page_new();
  adw_preferences_page_set_description(
      ADW_PREFERENCES_PAGE(page),
      _("Reset a text to go back to Mocktail's English default. Changes are "
        "saved with the other settings."));
  GtkWidget* status =
      AddGroup(page, _("Status"),
               _("The lines of your Discord status, up to 128 bytes each"));
  const std::vector<TextField> texts = {
      {"integrations.discord_rpc.text.browsing", _("While browsing"),
       "Browsing experiences", kDiscordTextLimit,
       // discord_rpc.cc: kBrowsing uses text.browsing.
       _("The first line of your status while you are in Roblox's menus, not "
         "in an experience.")},
      {"integrations.discord_rpc.text.joining", _("While joining"),
       "Joining an experience", kDiscordTextLimit,
       // Used while joining until the experience's name is known.
       _("The first line while an experience is loading and its name is not "
         "known yet.")},
      {"integrations.discord_rpc.text.playing", _("While playing"),
       "{place_name}", kDiscordTextLimit,
       _("The first line while you play. {place_name} is replaced with the "
         "experience's name; the default is just the name. With “Show "
         "experience name” off, the second line takes its place.")},
      {"integrations.discord_rpc.text.state", _("Second line"),
       kDefaultStateText, kDiscordTextLimit,
       // discord_rpc.cc: activity.state is text.state in every phase.
       _("The second line of your status at every stage. With “Show "
         "experience name” off it moves up and replaces the experience's "
         "name.")},
      {"integrations.discord_rpc.text.unknown_place",
       _("Unknown experience name"), "Unknown experience", kDiscordTextLimit,
       // discord_rpc.cc: place_name = text.unknown_place when the lookup
       // returned no name.
       _("Used as the experience's name while you play when Roblox's web API "
         "did not return one, for example without internet access.")},
  };
  for (const TextField& field : texts) {
    AddRow(status, BuildTextRow(context, field));
  }
  GtkWidget* join = AddGroup(page, _("Join button"),
                             _("The button friends click to join you"));
  AddRow(join,
         BuildTextRow(context,
                      {kButtonLabel, _("Button label"), kDefaultButtonLabel,
                       kDiscordButtonLabelLimit,
                       _("The text on the join button, at most 32 bytes.")}));
  GtkWidget* application = AddGroup(
      page, _("Discord application"),
      _("Change only if you made your own application in Discord's portal"));
  AddRow(application, BuildApplicationIdRow(context));

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  adw_dialog_set_child(dialog, toolbar);
  adw_dialog_present(dialog, GTK_WIDGET(context->window()));
}

// Integrations: Discord Rich Presence and Fleasion (research/ux.md 4.3
// INTEGRATIONS; research/graphics.md 6.3-6.4).
GtkWidget* BuildIntegrationsPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kIntegrations,
              _("Your status in Discord and Fleasion asset replacement"));

  GtkWidget* discord = AddGroup(
      page, _("Discord"), _("What your Discord profile shows while you play"));
  AddRow(discord, BuildRichPresenceRow(context));
  AddRow(discord, BuildShowPlaceNameRow(context));
  AddRow(discord, BuildShowTimeRow(context));
  AddRow(discord, BuildJoinRow(context));
  AddRow(discord, BuildPublicOnlyRow(context));
  AddRow(discord, BuildTextsRow(context));

  GtkWidget* fleasion = AddGroup(
      page, _("Fleasion"),
      _("Only for Fleasion users: let Roblox load the assets it replaces"));
  AddRow(fleasion, BuildFleasionRow(context));
  AddRow(fleasion, BuildFleasionModeRow(context));
  AddRow(fleasion, BuildFleasionPortRow(context));
  AddRow(fleasion, BuildFleasionCertificateRow(context));
  return page;
}

}  // namespace mocktail::launcher_ui
