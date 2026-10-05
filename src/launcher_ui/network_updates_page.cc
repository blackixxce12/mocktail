#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/bindings.h"
#include "launcher_ui/env_overrides.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/network_updates_status.h"
#include "launcher_ui/page_rules.h"
#include "launcher_ui/page_widgets.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kAutomatic[] = "updates.automatic";
constexpr char kSystemProxy[] = "network.use_system_proxy";
constexpr char kProxyHost[] = "network.proxy_host";
constexpr char kProxyPort[] = "network.proxy_port";
constexpr char kCaBundle[] = "network.ca_bundle";
// The template's example port (`# proxy_port: 8080`).
constexpr char kExamplePort[] = "8080";

// The owner of the "complete the manual proxy" problem.
constexpr char kProxyProblemOwner = 0;

ProxyMode CurrentProxyMode(const LauncherContext& context) {
  return ProxyModeFor(context.EffectiveValue(kSystemProxy, "false"),
                      context.Value(kProxyHost).has_value(),
                      context.Value(kProxyPort).has_value());
}

bool Exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

// What the manual proxy held before another choice removed it, so choosing
// Manual again brings it back.
struct RememberedProxy {
  std::optional<std::string> host;
  std::optional<std::string> port;
};

RememberedProxy& Remembered() {
  static RememberedProxy remembered;
  return remembered;
}

// ---- updates ----------------------------------------------------------------

GtkWidget* BuildAutomaticRow(LauncherContext* context) {
  RobloxStatus* status = RobloxStatus::For(context);
  RowSpec spec;
  spec.key = kAutomatic;
  spec.title = _("Update Roblox automatically");
  spec.fallback = "true";
  spec.keywords = {"update",     "updates",    "automatic",    "canary",
                   "обновление", "обновления", "автоматически"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value == "true"
               ? std::string(_("New versions are tested first; the working "
                               "one is kept if a test fails"))
               : std::string(_("Off: Roblox stays at the installed version"));
  };
  spec.hint.details =
      // The template's updates.automatic comment; update_coordinator.cc.
      // update_coordinator.cc: one canary run for a catalogued version,
      // two for a derived HostAbi profile.
      _("At each start Mocktail asks APKPure for the newest Roblox for "
        "Android. A new version is matched to a known compatibility profile, "
        "or gets one derived for it, and must pass a test start with your "
        "graphics backend (two for a derived profile), in a separate "
        "profile, before it replaces the current one.") +
      std::string("\n\n") +
      _("If a test fails, you keep playing the version that works. Checking "
        "and testing add some time to a start; the progress window shows "
        "it.") +
      "\n\n" +
      // update_coordinator.cc:732-740.
      _("Turn it off to stay on the installed version, for example while a "
        "new one misbehaves. Roblox's servers may eventually require a newer "
        "version. With nothing installed yet, Roblox cannot start while this "
        "is off.") +
      "\n\n" + _("Recommended: on.");
  spec.hint.warning = [status](LauncherContext&, const std::string& value) {
    if (value != "true" &&
        status->installed_state() == RobloxStatus::Installed::kReady &&
        !status->installed().installed) {
      return std::string(
          _("No Roblox is installed yet, so Roblox cannot "
            "start while this is off"));
    }
    return std::string();
  };
  return BindSwitchRow(context, std::move(spec));
}

GtkWidget* BuildSourceRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "updates.source";
  spec.title = _("Download source");
  spec.fallback = "apk-pure";
  spec.keywords = {"source",   "apkpure",  "apk",     "provider",
                   "download", "источник", "загрузка"};
  // update_config.cc and update_coordinator.cc: both values build the same
  // provider chain; the template says it "currently starts with APKPure".
  spec.hint.details =
      _("Where Mocktail downloads Roblox for Android from. Both choices use "
        "the same list of providers, which currently starts with APKPure, so "
        "there is nothing else to pick yet.");
  ComboSpec combo;
  combo.options = {
      {"apk-pure",
       _("APKPure"),
       _("Downloads Roblox for Android from APKPure"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"auto",
       _("Automatic"),
       _("Lets Mocktail choose; today the same APKPure source"),
       {},
       nullptr,
       nullptr,
       false,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

// ---- proxy ------------------------------------------------------------------

std::string ProxyChoiceUnavailable(LauncherContext& context, ProxyMode mode) {
  FleasionInputs inputs = FleasionInputsFrom(context);
  if (!inputs.enabled) return {};
  inputs.use_system_proxy = mode == ProxyMode::kSystem;
  if (mode == ProxyMode::kManual) {
    inputs.network_proxy_host = "proxy";
    inputs.network_proxy_port = kExamplePort;
  } else {
    inputs.network_proxy_host.reset();
    inputs.network_proxy_port.reset();
  }
  return DescribeFleasionConflict(FindFleasionConflict(inputs));
}

void ChooseProxy(LauncherContext& context, int index) {
  RememberedProxy& remembered = Remembered();
  const ProxyMode mode = static_cast<ProxyMode>(index);
  if (mode != ProxyMode::kManual) {
    if (context.Value(kProxyHost).has_value()) {
      remembered.host = context.Value(kProxyHost);
    }
    if (context.Value(kProxyPort).has_value()) {
      remembered.port = context.Value(kProxyPort);
    }
    context.UnsetValue(kProxyHost);
    context.UnsetValue(kProxyPort);
    context.SetValue(kSystemProxy,
                     mode == ProxyMode::kSystem ? "true" : "false");
    return;
  }
  context.SetValue(kSystemProxy, "false");
  if (remembered.host.has_value() && !context.Value(kProxyHost).has_value()) {
    context.SetValue(kProxyHost, *remembered.host,
                     launcher::ScalarKind::kString);
  }
  if (!context.Value(kProxyPort).has_value()) {
    context.SetValue(kProxyPort, remembered.port.value_or(kExamplePort),
                     launcher::ScalarKind::kInteger);
  }
}

// Save and Play wait until a manual proxy has both a valid host and port
// (the loader refuses one without the other, runtime_config_file.cc).
void UpdateProxyProblem(LauncherContext* context) {
  std::string problem;
  if (!context->read_only() &&
      CurrentProxyMode(*context) == ProxyMode::kManual) {
    const std::optional<std::string> host = context->Value(kProxyHost);
    const std::optional<std::string> port = context->Value(kProxyPort);
    if (!host.has_value() || host->empty()) {
      problem = _("Proxy: enter the proxy's host, or choose another proxy");
    } else if (CheckProxyHost(*host) != ProxyHostProblem::kNone) {
      problem = _("Proxy: the host is not valid");
    } else if (!port.has_value() || !IsValidPort(*port)) {
      problem = _("Proxy: enter a port from 1 to 65535");
    }
  }
  context->SetProblem(&kProxyProblemOwner, problem);
}

GtkWidget* BuildProxyRow(LauncherContext* context) {
  std::vector<Choice> choices = {
      {_("No proxy"), _("Roblox connects directly"), nullptr},
      {_("System proxy"),
       _("The system's proxy configuration, read at every start"),
       [](LauncherContext& context) {
         return ProxyChoiceUnavailable(context, ProxyMode::kSystem);
       }},
      {_("Manual"), _("An HTTP proxy at the host and port below"),
       [](LauncherContext& context) {
         return ProxyChoiceUnavailable(context, ProxyMode::kManual);
       }},
  };
  GtkWidget* row = NewChoiceRow(
      context, std::move(choices),
      [](LauncherContext& context) {
        return static_cast<int>(CurrentProxyMode(context));
      },
      ChooseProxy);
  FollowContext(context, row, [context] { UpdateProxyProblem(context); });
  UpdateProxyProblem(context);

  RowSpec spec;
  spec.title = _("Proxy");
  spec.keywords = {"proxy",
                   "socks",
                   "http proxy",
                   "system proxy",
                   "network",
                   "прокси",
                   "сеть",
                   "системный прокси",
                   kSystemProxy,
                   kProxyHost,
                   kProxyPort,
                   "MOCKTAIL_USE_SYSTEM_PROXY",
                   "MOCKTAIL_HTTP_PROXY_HOST",
                   "MOCKTAIL_HTTP_PROXY_PORT"};
  spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
    std::string text;
    switch (CurrentProxyMode(context)) {
      case ProxyMode::kNone:
        // http_client.cc and discord_rpc.cc leave libcurl's environment
        // proxy alone when no MOCKTAIL_HTTP_PROXY_* is set.
        text = _("Roblox connects directly; Mocktail's own requests follow an "
                 "https_proxy variable if your environment sets one");
        break;
      case ProxyMode::kSystem:
        text = _("The system's proxy configuration, read at every start");
        break;
      case ProxyMode::kManual:
        text = _("The HTTP proxy below");
        break;
    }
    // The fixed host and port rows carry their own ENV badges.
    if (const EnvOverride* env = context.EffectiveOverride(kSystemProxy)) {
      text +=
          "\n" + Format(_("This launch uses %s=%s instead"), env->name.c_str(),
                        RedactEnvironmentValue(env->value).c_str());
    }
    return text;
  };
  spec.hint.warning = [](LauncherContext& context, const std::string&) {
    const FleasionConflict conflict =
        FindFleasionConflict(FleasionInputsFrom(context));
    return conflict == FleasionConflict::kNone
               ? std::string()
               : Format(_("%s, so Mocktail will not start. Turn one of them "
                          "off."),
                        DescribeFleasionConflict(conflict).c_str());
  };
  spec.hint.details =
      // Consumers of MOCKTAIL_HTTP_PROXY_*: legacy_runtime.cc (Roblox's
      // HTTP client), services/http_client.cc, mocktail_webview_helper.cc,
      // discord_rpc.cc; src/update sets no CURLOPT_PROXY.
      _("Which proxy Roblox's own web requests, Mocktail's sign-in, the web "
        "sign-in window and the Discord status lookups use. Roblox downloads "
        "and update checks do not use it.") +
      std::string("\n\n") +
      _("• No proxy: Roblox connects directly. Mocktail's own requests and "
        "Roblox downloads still honour an https_proxy or ALL_PROXY variable, "
        "and the website sign-in window your desktop's proxy, if you have "
        "one.") +
      "\n\n" +
      // system_proxy.cc SelectSystemProxy: g_proxy_resolver_get_default for
      // https://www.roblox.com/ (credentials refused); main.cc stops when
      // it fails. legacy_runtime.cc hands Roblox's engine host and port
      // only.
      _("• System proxy: at every start Mocktail asks GLib which proxy to use "
        "for roblox.com (GNOME or KDE proxy settings, a PAC file, or the "
        "https_proxy variable), so a changing port is followed. Use an HTTP "
        "proxy: Roblox itself gets only the host and port, so SOCKS5 and "
        "HTTPS proxies may not work for it. Proxies that need a password are "
        "not supported, and Mocktail stops at start when the proxy cannot be "
        "used. Without glib-networking this always means a direct "
        "connection.") +
      "\n\n" +
      // runtime_config_file.cc exports MOCKTAIL_HTTP_PROXY_SCHEME=http.
      _("• Manual: an HTTP proxy at the host and port below.") + "\n\n" +
      _("Leave it at No proxy unless your network needs one. Fleasion, when "
        "on, sets up its own proxy instead.");
  return DecorateRow(context, row, std::move(spec));
}

std::string ManualOnly(LauncherContext& context, const char* reason) {
  return CurrentProxyMode(context) == ProxyMode::kManual ? std::string()
                                                         : std::string(reason);
}

GtkWidget* BuildProxyHostRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kProxyHost;
  spec.title = _("Proxy host");
  spec.keywords = {"host", "proxy host", "address", "хост", "адрес прокси"};
  // Host and port go together; "reset" would leave half a proxy.
  spec.resettable = false;
  spec.unavailable = [](LauncherContext& context) {
    return ManualOnly(context, _("Choose “Manual” above to set a host"));
  };
  spec.hint.subtitle = _("Host name or IP address, without http://");
  spec.hint.details =
      // runtime_config.cc ParseNetworkProxyConfig.
      _("The proxy server's host name or IP address, without a scheme or a "
        "port, for example 127.0.0.1 or proxy.example.org.") +
      std::string("\n\n") +
      _("Host and port are saved together: Save and Play wait until both are "
        "filled in.");
  EntrySpec entry;
  entry.empty_unsets = true;
  entry.validate = [](const std::string& text) {
    switch (CheckProxyHost(text)) {
      case ProxyHostProblem::kScheme:
        return std::string(_("Leave out http:// or https://"));
      case ProxyHostProblem::kInvalidCharacter:
        return std::string(_("A host cannot contain spaces or / \\ @ [ ] ? #"));
      case ProxyHostProblem::kEmpty:
      case ProxyHostProblem::kNone:
        break;
    }
    return std::string();
  };
  return BindEntryRow(context, std::move(spec), std::move(entry));
}

GtkWidget* BuildProxyPortRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kProxyPort;
  spec.title = _("Proxy port");
  spec.keywords = {"port", "порт"};
  spec.resettable = false;
  spec.unavailable = [](LauncherContext& context) {
    return ManualOnly(context, _("Choose “Manual” above to set a port"));
  };
  spec.hint.subtitle_for = [](LauncherContext& context,
                              const std::string& value) {
    const std::optional<std::string> host = context.Value(kProxyHost);
    if (CurrentProxyMode(context) == ProxyMode::kManual && host.has_value() &&
        !value.empty()) {
      return Format(_("Connects to http://%s:%s"), host->c_str(),
                    value.c_str());
    }
    return std::string(_("The port the proxy listens on"));
  };
  spec.hint.details = _("The TCP port the proxy listens on, from 1 to 65535.");
  SpinSpec spin;
  spin.minimum = 1;
  spin.maximum = 65535;
  spin.unset_value = 8080;
  return BindSpinRow(context, std::move(spec), spin);
}

GtkWidget* BuildCaBundleRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = kCaBundle;
  spec.title = _("Custom CA bundle");
  spec.keywords = {"ca",  "certificate", "certificates", "pem",        "tls",
                   "ssl", "https",       "сертификат",   "сертификаты"};
  spec.hint.subtitle_for = [](LauncherContext&, const std::string& value) {
    return value.empty() ? std::string(_("Empty: your system's certificates"))
                         : std::string(_("Roblox and Mocktail's own requests "
                                         "trust only the certificates in this "
                                         "file"));
  };
  spec.hint.warning = [](LauncherContext&, const std::string& value) {
    // main.cc: an unreadable MOCKTAIL_CA_BUNDLE stops the start (status 2).
    return !value.empty() && !Exists(value)
               ? std::string(
                     _("This file does not exist, so Mocktail will not start"))
               : std::string();
  };
  spec.hint.details =
      // Readers of MOCKTAIL_CA_BUNDLE: libc_shim.cc (Roblox),
      // http_client.cc, discord_rpc.cc; the updater and the WebKit window
      // use the system's store.
      _("A PEM file of certificate authorities that Roblox, Mocktail's "
        "sign-in checks and the Discord lookups trust for HTTPS instead of "
        "the system's list. Roblox downloads, update checks and the website "
        "sign-in window still use the system's list, so on a network that "
        "inspects HTTPS also add its certificate to the system. The file must "
        "also hold the usual public authorities.") +
      std::string("\n\n") +
      // The template: "Mocktail reads this file directly and does not
      // replace it during Roblox payload updates".
      _("Mocktail reads the file at every start and never changes it, not "
        "even during Roblox updates. With Fleasion on, Fleasion's certificate "
        "is added to it.") +
      "\n\n" +
      // main.cc ResolveHostCaBundle kInvalidOverride.
      _("Only use a file you trust: any authority in it can pose as any "
        "website to Roblox and Mocktail. If the file cannot be read, Mocktail "
        "stops at start.");
  EntrySpec entry;
  entry.empty_unsets = true;
  entry.validate = [](const std::string& text) {
    switch (CheckCertificatePath(text)) {
      case CertificatePathProblem::kRelative:
        return std::string(_("Use an absolute path, starting with /"));
      case CertificatePathProblem::kPrivateKey:
        return std::string(_("That is a private key, not a certificate file"));
      case CertificatePathProblem::kNone:
        break;
    }
    return std::string();
  };
  GtkWidget* row = BindEntryRow(context, std::move(spec), std::move(entry));
  AddFileChooserButton(context, row, kCaBundle, _("Choose a CA Bundle"));
  return row;
}

}  // namespace

// Network & Updates (research/ux.md 4.3 NETWORK & UPDATES; SPEC 5 About
// for the updater calls).
GtkWidget* BuildNetworkUpdatesPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kNetworkUpdates,
              _("How Roblox is kept up to date and which proxy it uses"));

  GtkWidget* updates =
      AddGroup(page, _("Updates"),
               _("Mocktail downloads Roblox for you and keeps a working copy"));
  AddRow(updates, BuildAutomaticRow(context));
  AddRow(updates, BuildInstalledRobloxRow(context, false));
  AddRow(updates, BuildLatestRobloxRow(context));
  AddRow(updates, BuildSourceRow(context));

  GtkWidget* proxy =
      AddGroup(page, _("Proxy"),
               _("Which proxy Roblox and Mocktail's sign-in use"));
  AddRow(proxy, BuildProxyRow(context));
  AddRow(proxy, BuildProxyHostRow(context));
  AddRow(proxy, BuildProxyPortRow(context));
  AddRow(proxy, BuildCaBundleRow(context));
  return page;
}

}  // namespace mocktail::launcher_ui
