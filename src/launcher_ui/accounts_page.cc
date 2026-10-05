#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launcher_ui/accounts_controller.h"
#include "launcher_ui/accounts_model.h"
#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "runtime/browser_sign_in.h"

// The Accounts page and the launch bar's account chip (research/ux.md 4.2,
// 4.3 ACCOUNTS, 4.6; research/auth.md 5). Both show the state of the
// AccountsController and change only its staged selection and removals;
// the store is written on Save and Play.
namespace mocktail::launcher_ui {
namespace {

constexpr int kRowAvatarSize = 40;
constexpr int kSmallAvatarSize = 32;
constexpr char kViewKey[] = "mocktail-accounts-view";

runtime::ActiveAccountPointer AccountPointer(std::int64_t user_id) {
  runtime::ActiveAccountPointer pointer;
  pointer.guest = false;
  pointer.user_id = user_id;
  return pointer;
}

// "native" or "browser": what the next start uses, the environment first
// (runtime_config.cc reads MOCKTAIL_NATIVE_LOGIN == "0" as browser).
std::string EffectiveSignIn(LauncherContext& context) {
  if (const EnvOverride* env = context.EffectiveOverride("account.sign_in")) {
    return env->value == "0" ? "browser" : "native";
  }
  return context.EffectiveValue("account.sign_in", "native");
}

std::string Join(const std::vector<std::string>& parts, const char* glue) {
  std::string joined;
  for (const std::string& part : parts) {
    if (part.empty()) continue;
    if (!joined.empty()) joined += glue;
    joined += part;
  }
  return joined;
}

// last_used_at is set whenever Save or Play selects the account
// (AccountStore::SelectForLaunch), not by website joins, which start without
// this window: "used" is what the timestamp can promise.
std::string LastUsedText(std::int64_t used_at) {
  const LastPlayed used =
      DescribeLastPlayed(used_at, g_get_real_time() / G_USEC_PER_SEC);
  const auto count = static_cast<unsigned long>(used.count);
  const int shown = static_cast<int>(used.count);
  switch (used.kind) {
    case LastPlayed::Kind::kNever:
      return _("not used here yet");
    case LastPlayed::Kind::kJustNow:
      return _("last used just now");
    case LastPlayed::Kind::kMinutes:
      return Format(ngettext("last used %d minute ago",
                             "last used %d minutes ago", count),
                    shown);
    case LastPlayed::Kind::kHours:
      return Format(
          ngettext("last used %d hour ago", "last used %d hours ago", count),
          shown);
    case LastPlayed::Kind::kDays:
      if (used.count == 1) return _("last used yesterday");
      return Format(
          ngettext("last used %d day ago", "last used %d days ago", count),
          shown);
    case LastPlayed::Kind::kDate:
      break;
  }
  GDateTime* time = g_date_time_new_from_unix_local(used_at);
  if (time == nullptr) return {};
  gchar* date = g_date_time_format(time, "%x");
  g_date_time_unref(time);
  std::string text = Format(_("last used on %s"), date != nullptr ? date : "");
  g_free(date);
  return text;
}

std::string Handle(const runtime::SavedAccount& account) {
  return account.username.empty() ? std::string() : "@" + account.username;
}

std::string AccountSubtitle(const runtime::SavedAccount& account) {
  std::string state;
  switch (StatusOf(account)) {
    case AccountStatus::kSignedOut:
      // research/ux.md 4.6: a refused session is kept as signed out.
      state = _("signed out — sign in again");
      break;
    case AccountStatus::kUnverified:
      state = _("not checked: Roblox could not be reached");
      break;
    case AccountStatus::kReady:
      state = LastUsedText(account.last_used_at);
      break;
  }
  return Join({Handle(account), state}, " · ");
}

// An avatar showing the account's headshot, else its initials; without an
// account the generic person icon.
GtkWidget* NewAvatar(AccountsController& controller,
                     const runtime::SavedAccount* account, int size) {
  // Without an account the text only picks a steady background color.
  GtkWidget* avatar = adw_avatar_new(
      size, account != nullptr ? ShownName(*account).c_str() : _("Guest"),
      account != nullptr);
  if (account != nullptr) {
    adw_avatar_set_custom_image(ADW_AVATAR(avatar),
                                controller.AvatarFor(*account));
  }
  gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
  return avatar;
}

GtkWidget* NewCheck(bool visible) {
  GtkWidget* check = gtk_image_new_from_icon_name("object-select-symbolic");
  gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(check),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Selected for Play"), -1);
  gtk_widget_set_visible(check, visible);
  return check;
}

GtkWidget* NewSpinner() {
  GtkWidget* spinner = adw_spinner_new();
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  return spinner;
}

GtkWidget* NewStatusRow(const std::string& title, const std::string& subtitle) {
  GtkWidget* row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
  return row;
}

// A suffix button. When the row's title names what the button does, the
// whole row activates it (and the title becomes its accessible name);
// otherwise the button keeps its own label.
GtkWidget* AddRowButton(GtkWidget* row, const char* mnemonic_label,
                        bool activates_row) {
  GtkWidget* button = gtk_button_new_with_mnemonic(mnemonic_label);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
  if (activates_row) {
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), button);
  }
  return button;
}

// A name for screen readers that says more than the visible label. GtkButton
// is labelled by its label child, which wins over a label property.
void SetAccessibleName(GtkWidget* widget, const std::string& name) {
  gtk_accessible_reset_relation(GTK_ACCESSIBLE(widget),
                                GTK_ACCESSIBLE_RELATION_LABELLED_BY);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
}

void SetUnavailable(GtkWidget* widget, const std::string& reason) {
  gtk_widget_set_sensitive(widget, reason.empty());
  gtk_widget_set_tooltip_text(widget,
                              reason.empty() ? nullptr : reason.c_str());
}

using Callback = std::function<void()>;

void ConnectCallback(gpointer instance, const char* signal, Callback callback) {
  g_signal_connect_data(
      instance, signal, G_CALLBACK(+[](GObject*, gpointer data) {
        // Copy: the callback may rebuild the widget that owns it.
        const Callback run = *static_cast<Callback*>(data);
        run();
      }),
      new Callback(std::move(callback)),
      +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); },
      GConnectFlags(0));
}

// Runs `action` on the controller from the main loop, after the signal that
// asked for it: choosing rebuilds the rows, the popover and the menus that
// emitted it.
Callback Later(std::weak_ptr<AccountsController> controller,
               std::function<void(AccountsController&)> action) {
  return [controller = std::move(controller), action = std::move(action)] {
    auto* pending = new Callback([controller, action] {
      if (std::shared_ptr<AccountsController> locked = controller.lock()) {
        action(*locked);
      }
    });
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          (*static_cast<Callback*>(data))();
          return G_SOURCE_REMOVE;
        },
        pending, [](gpointer data) { delete static_cast<Callback*>(data); });
  };
}

// "activate" of a GSimpleAction carries a parameter.
void ConnectAction(GSimpleActionGroup* group, const char* name, bool enabled,
                   Callback callback) {
  GSimpleAction* action = g_simple_action_new(name, nullptr);
  g_simple_action_set_enabled(action, enabled);
  g_signal_connect_data(
      action, "activate",
      G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
        const Callback run = *static_cast<Callback*>(data);
        run();
      }),
      new Callback(std::move(callback)),
      +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); },
      GConnectFlags(0));
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action);
}

// ---- the page
// ---------------------------------------------------------------

class AccountsPageView {
 public:
  AccountsPageView(LauncherContext* context,
                   std::shared_ptr<AccountsController> controller)
      : context_(context), controller_(std::move(controller)) {}

  ~AccountsPageView() {
    if (guest_row_ != nullptr) g_object_unref(guest_row_);
  }

  GtkWidget* Build() {
    page_ = NewPage(context_, Section::kAccounts,
                    _("The Roblox accounts saved on this computer and how "
                      "you sign in"));
    BuildSavedAccounts();
    BuildSignIn();
    // research/auth.md 5.6: 0600 files in 0700 folders; sessions go only to
    // Roblox (users.roblox.com for the checks) and never to the UI.
    AddGroup(page_, {},
             // runtime_config.cc / fleasion.cc: with Fleasion on, Roblox's
             // web traffic goes through its proxy, which trusts its own CA.
             _("Sign-in sessions are stored only on this computer, in files "
               "only your user can read, and are sent only to Roblox (and "
               "pass through Fleasion, which can read them, while it is "
               "on). Mocktail never shows them."));

    g_object_set_data_full(G_OBJECT(page_), kViewKey, this, [](gpointer data) {
      delete static_cast<AccountsPageView*>(data);
    });
    g_signal_connect(page_, "destroy", G_CALLBACK(OnDestroy), this);
    observer_ = controller_->AddObserver([this] { Rebuild(); });
    Rebuild();
    return page_;
  }

 private:
  static void OnDestroy(GtkWidget*, gpointer data) {
    auto* view = static_cast<AccountsPageView*>(data);
    view->controller_->RemoveObserver(view->observer_);
    view->destroyed_ = true;
    // Its bindings talk to the context: let it go with the page.
    g_clear_object(&view->guest_row_);
    // The window is closing: stop the sign-in window and the worker.
    view->controller_->Shutdown();
  }

  void BuildSavedAccounts() {
    accounts_group_ = AddGroup(
        page_, _("Saved accounts"),
        // main.cc ResolveActiveAccountAuthRoot runs for website joins too.
        _("Play and joins from roblox.com start with the selected account. "
          "Switching needs no new sign-in."));
    add_button_ = gtk_button_new();
    GtkWidget* content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content),
                                     "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(content),
                                 _("_Add Account"));
    adw_button_content_set_use_underline(ADW_BUTTON_CONTENT(content), TRUE);
    gtk_button_set_child(GTK_BUTTON(add_button_), content);
    gtk_widget_add_css_class(add_button_, "flat");
    gtk_widget_set_valign(add_button_, GTK_ALIGN_CENTER);
    ConnectCallback(add_button_, "clicked",
                    Later(controller_, [](AccountsController& controller) {
                      controller.StartSignIn();
                    }));
    adw_preferences_group_set_header_suffix(
        ADW_PREFERENCES_GROUP(accounts_group_), add_button_);

    if (controller_->environment_override()) {
      // Nothing to choose: no guest row (it would also be found by search).
      AddRow(accounts_group_, BuildOverrideRow());
      return;
    }
    guest_row_ = BuildGuestRow();
    g_object_ref_sink(guest_row_);
  }

  GtkWidget* BuildOverrideRow() {
    const std::string names = Join(controller_->override_variables(), ", ");
    GtkWidget* row = adw_action_row_new();
    RowSpec spec;
    spec.title = _("Account managed by environment override");
    spec.hint.subtitle =
        Format(_("%s is set, so Roblox uses that sign-in and not the saved "
                 "accounts"),
               names.c_str());
    // runtime_paths.h AccountStoreOverriddenByEnvironment; main.cc then
    // resolves no saved account (research/auth.md 5.5.6).
    spec.hint.details =
        std::string(_("A variable in the shortcut or terminal that started "
                      "Mocktail chooses the Roblox session, so this launch "
                      "does not use, check or change the saved accounts.")) +
        "\n\n" +
        _("• MOCKTAIL_AUTH_ROOT: a folder with its own saved session") + "\n" +
        _("• MOCKTAIL_COOKIE_FILE: a session file") + "\n" +
        _("• MOCKTAIL_ROBLOX_COOKIES: a session given directly") + "\n\n" +
        _("Start Mocktail without the variable to use the saved accounts "
          "again. Its value is not shown here because it can contain a "
          "session.");
    spec.keywords = controller_->override_variables();
    for (const char* keyword : {"environment", "override", "cookie",
                                "переменная окружения", "куки"}) {
      spec.keywords.emplace_back(keyword);
    }
    return DecorateRow(context_, row, std::move(spec));
  }

  GtkWidget* BuildGuestRow() {
    GtkWidget* row = adw_action_row_new();
    adw_action_row_add_prefix(ADW_ACTION_ROW(row),
                              NewAvatar(*controller_, nullptr, kRowAvatarSize));
    guest_check_ = NewCheck(false);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), guest_check_);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    ConnectCallback(row, "activated",
                    Later(controller_, [](AccountsController& controller) {
                      controller.Choose(runtime::ActiveAccountPointer{});
                    }));

    RowSpec spec;
    spec.title = _("Play as guest");
    spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
      // main.cc PromptFirstLaunchSignIn: the website window opens first for
      // a signed-out start with account.sign_in browser.
      return EffectiveSignIn(context) == "browser"
                 ? std::string(_("Starts signed out; Mocktail's website "
                                 "sign-in window opens first"))
                 : std::string(_("Starts signed out; Roblox opens on its "
                                 "welcome screen, where you can sign in"));
    };
    spec.hint.details =
        std::string(_("Roblox starts without a saved account, for example to "
                      "look around signed out or to sign in inside Roblox.")) +
        "\n\n" +
        // account_store.h SelectForLaunch / ClearSessionArtifacts.
        _("Before a guest start Mocktail clears the website session and the "
          "player data Roblox cached for the last account, so nothing of "
          "another account carries over.") +
        "\n\n" +
        // account_store.cc Reconcile: a guest-slot session is filed under
        // its account, which becomes active.
        _("If you sign in during a guest start, the account is kept: the "
          "next time this window opens, Mocktail asks Roblox whose it is, "
          "adds it to this list and selects it for Play. Remove it here "
          "afterwards if you do not want to keep it.");
    spec.hint.details_for = [](LauncherContext& context) {
      return EffectiveSignIn(context) == "browser"
                 ? std::string(_("Sign-in method is Website window, so "
                                 "Mocktail's sign-in window opens before "
                                 "Roblox. Close it to stay signed out."))
                 : std::string();
    };
    spec.keywords = {"guest", "signed out", "no account",   "logout",
                     "гость", "без входа",  "без аккаунта", "выйти"};
    std::weak_ptr<AccountsController> weak = controller_;
    spec.unavailable = [weak](LauncherContext&) {
      std::shared_ptr<AccountsController> controller = weak.lock();
      return controller != nullptr ? controller->SelectionUnavailableReason()
                                   : std::string();
    };
    return DecorateRow(context_, row, std::move(spec));
  }

  void BuildSignIn() {
    GtkWidget* group =
        AddGroup(page_, _("Signing in"),
                 _("Add accounts here or inside Roblox, and choose which "
                   "sign-in screen Roblox uses"));
    AddRow(group, BuildWebsiteRow());
    AddRow(group, BuildInsideRobloxRow());
    AddRow(group, BuildSignInMethodRow());
  }

  GtkWidget* BuildWebsiteRow() {
    GtkWidget* row = adw_action_row_new();
    website_button_ = AddRowButton(row, _("Sign _In…"), true);
    ConnectCallback(website_button_, "clicked",
                    Later(controller_, [](AccountsController& controller) {
                      controller.StartSignIn();
                    }));
    RowSpec spec;
    spec.title = _("Sign in on the Roblox website");
    spec.hint.subtitle =
        _("Opens roblox.com's sign-in page in a Mocktail window and saves "
          "the account");
    spec.hint.details =
        std::string(_("Opens Roblox's own sign-in page (roblox.com/login) in "
                      "a Mocktail window titled “Add Roblox account”. Sign in "
                      "there as in a web browser.")) +
        "\n\n" +
        // browser_sign_in.h BrowserSignInSession.
        _("Mocktail asks Roblox whether the new session works before saving "
          "it. A session Roblox refuses is cleared and the window stays open "
          "for another try; when Roblox cannot be reached, Mocktail asks "
          "again a few seconds later.") +
        "\n\n" +
        // AccountStore::PrepareBrowserSignIn, clear_jar.
        _("Before the window opens, Mocktail forgets the website session it "
          "kept from earlier sign-ins, so the page starts signed out.") +
        "\n\n" +
        _("The new account is selected for the next Play, and Save or Play "
          "keeps that choice. Close the window to cancel.") +
        "\n\n" +
        // research/auth.md 5.6; browser_sign_in.h.
        _("The session goes only to Roblox and is stored in a file only "
          "your user can read. It never appears in this window, in logs or "
          "on the command line.");
    spec.keywords = {"add account", "sign in", "log in",  "login",
                     "website",     "browser", "webview", "добавить",
                     "войти",       "вход",    "сайт",    "браузер"};
    return DecorateRow(context_, row, std::move(spec));
  }

  GtkWidget* BuildInsideRobloxRow() {
    GtkWidget* row = adw_action_row_new();
    inside_button_ = AddRowButton(row, _("_Start Roblox"), true);
    ConnectCallback(inside_button_, "clicked",
                    Later(controller_, [](AccountsController& controller) {
                      controller.PlaySignedOut();
                    }));
    RowSpec spec;
    spec.title = _("Sign in inside Roblox");
    spec.hint.subtitle_for = [](LauncherContext& context, const std::string&) {
      return EffectiveSignIn(context) == "browser"
                 ? std::string(_("Starts Roblox signed out; with the Website "
                                 "window method Mocktail's sign-in window "
                                 "opens first"))
                 : std::string(_("Starts Roblox signed out so you can sign in "
                                 "on its welcome screen, for example with "
                                 "Quick Log In"));
    };
    spec.hint.details =
        std::string(_("Starts Roblox right away without a saved account, so "
                      "you sign in on Roblox's own welcome screen, for "
                      "example with Quick Log In and a device where you are "
                      "already signed in.")) +
        "\n\n" +
        // roblox_web_view_bridge.cc DispatchOpenRequest (detached sign-in)
        // and the restart after website sign-in (main.cc).
        _("When Roblox's own sign-in asks for a Google Play Integrity device "
          "check, which nothing on Linux can pass, Mocktail opens the "
          "roblox.com sign-in page instead and restarts Roblox signed in "
          "once you are done.") +
        "\n\n" +
        // account_store.cc Reconcile files the guest slot's session.
        _("The account you sign in with is kept: the next time this window "
          "opens, Mocktail asks Roblox whose it is, adds it to Saved "
          "accounts and selects it for Play.") +
        "\n\n" +
        // accounts_controller.cc PlaySignedOut writes accounts/active=guest.
        _("This also selects Guest for later starts, website joins included, "
          "until you sign in or choose an account here.") +
        "\n\n" + _("Start Roblox saves your changes first, like Play.");
    spec.hint.details_for = [](LauncherContext& context) {
      return EffectiveSignIn(context) == "browser"
                 ? std::string(_("Sign-in method is Website window, so "
                                 "Mocktail's sign-in window opens before the "
                                 "welcome screen. Close it to reach "
                                 "Roblox's own screen."))
                 : std::string();
    };
    spec.keywords = {"quick sign-in", "quick log in",   "quick login",  "qr",
                     "code",          "in-app",         "быстрый вход", "код",
                     "в приложении",  "войти в roblox", "гость"};
    return DecorateRow(context_, row, std::move(spec));
  }

  GtkWidget* BuildSignInMethodRow() {
    RowSpec spec;
    spec.key = "account.sign_in";
    spec.title = _("Sign-in method");
    // runtime_config.h: native is the built-in default.
    spec.fallback = "native";
    spec.hint.details =
        std::string(
            _("Decides where you sign in when Roblox starts without a "
              "working saved session: as a guest, or with a saved account "
              "whose sign-in expired. A signed-in account starts directly "
              "either way.")) +
        "\n\n" +
        // roblox_web_view_bridge.cc DispatchOpenRequest; command_line.cc
        // usage text "If it asks for device attestation such as Google Play
        // Integrity, website sign-in opens instead".
        _("• Roblox app: Roblox's own welcome screen. Its sign-in may ask for "
          "a Google Play Integrity device check, which nothing on Linux can "
          "pass; Mocktail then opens the roblox.com sign-in page instead and "
          "restarts Roblox signed in once you are done.") +
        "\n\n" +
        // main.cc PromptFirstLaunchSignIn; roblox_web_view_bridge.cc routes
        // login challenges to /login when MOCKTAIL_NATIVE_LOGIN=0.
        // roblox_web_view_bridge.cc: only login challenges are routed to
        // www.roblox.com/login; other pages keep Roblox's web window.
        _("• Website window: before Roblox starts, Mocktail opens "
          "roblox.com's sign-in page in its own window; close it to continue "
          "signed out. If you later sign in on Roblox's own screen and it "
          "asks for a verification step, the roblox.com sign-in page opens "
          "instead.") +
        "\n\n" +
        // main.cc: no first-launch sign-in for an external launch request.
        _("Joining from a link on roblox.com never opens the sign-in window "
          "first; it starts with the selected account.") +
        "\n\n" +
        _("Either way the session is saved for the account Play starts with, "
          "or, after a guest start, added to Saved accounts the next time "
          "this window opens.");
    spec.hint.warning = [](LauncherContext& context, const std::string&) {
      // main.cc PromptFirstLaunchSignIn returns when the helper is missing.
      if (EffectiveSignIn(context) == "browser" &&
          runtime::ResolveWebViewHelperPath().empty()) {
        return std::string(
            _("The sign-in window is not installed (mocktail_webview_helper "
              "is missing), so Roblox's welcome screen appears instead."));
      }
      return std::string();
    };
    spec.keywords = {"login",       "log in",  "sign in", "browser",
                     "webview",     "website", "native",  "play integrity",
                     "вход",        "войти",   "браузер", "сайт",
                     "способ входа"};
    ComboSpec combo;
    combo.options = {
        {"native",
         _("Roblox app"),
         _("Roblox's welcome screen; the website opens when Roblox asks for "
           "Play Integrity"),
         {},
         nullptr,
         nullptr,
         false,
         false},
        {"browser",
         _("Website window"),
         _("Mocktail's roblox.com window opens on start whenever no account "
           "is signed in"),
         {},
         nullptr,
         nullptr,
         false,
         false},
    };
    return BindComboRow(context_, std::move(spec), std::move(combo));
  }

  // ---- the list, rebuilt on every change
  // ----------------------------------

  void Rebuild() {
    if (destroyed_) return;
    for (GtkWidget* row : dynamic_rows_) {
      adw_preferences_group_remove(ADW_PREFERENCES_GROUP(accounts_group_), row);
    }
    dynamic_rows_.clear();
    if (guest_row_ != nullptr && gtk_widget_get_parent(guest_row_) != nullptr) {
      adw_preferences_group_remove(ADW_PREFERENCES_GROUP(accounts_group_),
                                   guest_row_);
    }

    AccountsController& controller = *controller_;
    const AccountsModel& model = controller.model();
    bool empty_state = false;
    if (!controller.environment_override()) {
      if (!controller.load_error().empty()) {
        AddDynamic(NewStatusRow(_("Saved accounts cannot be read"),
                                controller.load_error()));
      }
      if (controller.checking()) {
        GtkWidget* row =
            // account_store.cc Reconcile asks only about sessions whose
            // hash changed since the last start, and the guest slot.
            NewStatusRow(_("Checking saved accounts…"),
                         _("Asking Roblox about sign-ins that changed since "
                           "the last start"));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), NewSpinner());
        AddDynamic(row);
      }
      if (controller.signing_in()) {
        GtkWidget* row =
            NewStatusRow(_("Waiting for you to sign in…"),
                         _("Finish in the Roblox sign-in window, or close it "
                           "to cancel"));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), NewSpinner());
        GtkWidget* cancel = AddRowButton(row, _("_Cancel"), false);
        ConnectCallback(cancel, "clicked",
                        Later(controller_, [](AccountsController& controller) {
                          controller.CancelSignIn();
                        }));
        AddDynamic(row);
      }
      const std::vector<const runtime::SavedAccount*> accounts =
          model.VisibleAccounts();
      const bool legacy_pending = !model.snapshot().initialized &&
                                  model.snapshot().legacy_session_present;
      if (controller.checking() || controller.signing_in()) {
        // Their rows above say what happens next.
      } else if (legacy_pending) {
        // MigrateLegacySession was postponed: Roblox could not be asked.
        AddDynamic(NewStatusRow(_("Sign-in from an earlier Mocktail version"),
                                _("Roblox keeps using it. It joins this list "
                                  "once Roblox confirms whose it is.")));
      } else if (accounts.empty() && controller.load_error().empty()) {
        // research/ux.md 4.3: the empty state.
        GtkWidget* row =
            NewStatusRow(_("No saved accounts"),
                         _("Sign in once and Mocktail remembers you on this "
                           "computer"));
        GtkWidget* sign_in = AddRowButton(row, _("Sign _In…"), false);
        SetUnavailable(sign_in, controller.AddUnavailableReason());
        ConnectCallback(sign_in, "clicked",
                        Later(controller_, [](AccountsController& controller) {
                          controller.StartSignIn();
                        }));
        AddDynamic(row);
        empty_state = true;
      }
      for (const runtime::SavedAccount* account : accounts) {
        AddDynamic(BuildAccountRow(*account));
      }
      AddRow(accounts_group_, guest_row_);
    }
    if (guest_check_ != nullptr) {
      gtk_widget_set_visible(guest_check_,
                             model.IsSelected(runtime::ActiveAccountPointer{}));
    }

    const std::string add_reason = controller.AddUnavailableReason();
    SetUnavailable(add_button_, add_reason);
    // The empty state's own Sign In… does what Add Account does; with both,
    // and the Signing in group's Sign In… below, an empty list offered the
    // same action three times.
    gtk_widget_set_visible(add_button_,
                           !controller.environment_override() && !empty_state);
    SetUnavailable(website_button_, add_reason);
    // Play waits for the check and for an open sign-in window anyway.
    SetUnavailable(inside_button_, add_reason);
  }

  void AddDynamic(GtkWidget* row) {
    AddRow(accounts_group_, row);
    dynamic_rows_.push_back(row);
  }

  GtkWidget* BuildAccountRow(const runtime::SavedAccount& account) {
    const std::string name = ShownName(account);
    const std::int64_t user_id = account.user_id;
    const bool selected =
        controller_->model().IsSelected(AccountPointer(user_id));
    GtkWidget* row = NewStatusRow(name, AccountSubtitle(account));
    adw_action_row_add_prefix(
        ADW_ACTION_ROW(row), NewAvatar(*controller_, &account, kRowAvatarSize));
    if (StatusOf(account) == AccountStatus::kSignedOut) {
      // research/ux.md 4.6: a refused session is kept, with a Sign in button.
      GtkWidget* sign_in = AddRowButton(row, _("Sign _In…"), false);
      SetAccessibleName(sign_in,
                        Format(_("Sign in again as %s"), name.c_str()));
      SetUnavailable(sign_in, controller_->AddUnavailableReason());
      ConnectCallback(
          sign_in, "clicked",
          Later(controller_, [user_id](AccountsController& controller) {
            controller.StartSignIn(user_id);
          }));
    }
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), NewCheck(selected));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    const runtime::ActiveAccountPointer pointer = AccountPointer(user_id);
    ConnectCallback(
        row, "activated",
        Later(controller_, [pointer](AccountsController& controller) {
          controller.Choose(pointer);
        }));

    // research/ux.md 4.3: Use for Play, Sign out and remove…; Sign in
    // again for an account whose session expired (research/auth.md 5.5.3
    // step 5).
    GSimpleActionGroup* actions = g_simple_action_group_new();
    ConnectAction(actions, "use", !selected,
                  Later(controller_, [pointer](AccountsController& controller) {
                    controller.Choose(pointer);
                  }));
    ConnectAction(actions, "sign-in-again",
                  controller_->AddUnavailableReason().empty(),
                  Later(controller_, [user_id](AccountsController& controller) {
                    controller.StartSignIn(user_id);
                  }));
    ConnectAction(actions, "remove", true,
                  Later(controller_, [user_id](AccountsController& controller) {
                    controller.ConfirmRemoval(user_id);
                  }));
    gtk_widget_insert_action_group(row, "account", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    GMenu* menu = g_menu_new();
    GMenu* choose = g_menu_new();
    g_menu_append(choose, _("_Use for Play"), "account.use");
    // A signed-out row has its own button; an unchecked session may still
    // turn out to be refused.
    if (StatusOf(account) == AccountStatus::kUnverified) {
      g_menu_append(choose, _("Sign _In Again…"), "account.sign-in-again");
    }
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(choose));
    g_object_unref(choose);
    GMenu* remove = g_menu_new();
    // There is no server-side sign-out (account_store.h RemoveAccount).
    g_menu_append(remove, _("_Remove from This Computer…"), "account.remove");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(remove));
    g_object_unref(remove);

    GtkWidget* more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_add_css_class(more, "flat");
    gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(more, _("More"));
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(more), GTK_ACCESSIBLE_PROPERTY_LABEL,
        Format(_("Actions for %s"), name.c_str()).c_str(), -1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), more);
    return row;
  }

  LauncherContext* context_;
  std::shared_ptr<AccountsController> controller_;
  GtkWidget* page_ = nullptr;
  GtkWidget* accounts_group_ = nullptr;
  GtkWidget* add_button_ = nullptr;
  GtkWidget* website_button_ = nullptr;
  GtkWidget* inside_button_ = nullptr;
  GtkWidget* guest_row_ = nullptr;  // owned (re-added after the accounts)
  GtkWidget* guest_check_ = nullptr;
  std::vector<GtkWidget*> dynamic_rows_;
  AccountsController::ObserverId observer_ = 0;
  bool destroyed_ = false;
};

// ---- the chip
// ---------------------------------------------------------------

class AccountChipView {
 public:
  AccountChipView(LauncherContext* context,
                  std::shared_ptr<AccountsController> controller)
      : context_(context), controller_(std::move(controller)) {}

  GtkWidget* Build() {
    button_ = gtk_menu_button_new();
    gtk_widget_add_css_class(button_, "flat");
    gtk_widget_add_css_class(button_, "account-chip");

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    avatar_slot_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(content), avatar_slot_);
    names_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(names_, GTK_ALIGN_CENTER);
    name_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(name_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(name_), PANGO_ELLIPSIZE_END);
    // Long display names must not push Play out of a mid-size window.
    gtk_label_set_max_width_chars(GTK_LABEL(name_), 20);
    gtk_widget_add_css_class(name_, "heading");
    gtk_box_append(GTK_BOX(names_), name_);
    detail_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(detail_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(detail_), 20);
    gtk_widget_add_css_class(detail_, "caption");
    gtk_widget_add_css_class(detail_, "dim-label");
    gtk_box_append(GTK_BOX(names_), detail_);
    gtk_box_append(GTK_BOX(content), names_);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(button_), content);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button_), TRUE);

    popover_ = gtk_popover_new();
    popover_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(popover_box_, 6);
    gtk_widget_set_margin_bottom(popover_box_, 6);
    gtk_widget_set_margin_start(popover_box_, 6);
    gtk_widget_set_margin_end(popover_box_, 6);
    gtk_popover_set_child(GTK_POPOVER(popover_), popover_box_);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button_), popover_);
    // Keyboard users land on the account Play uses, as in a radio group.
    g_signal_connect(popover_, "map", G_CALLBACK(OnPopoverMapped), this);

    g_object_set_data_full(
        G_OBJECT(button_), kViewKey, this,
        [](gpointer data) { delete static_cast<AccountChipView*>(data); });
    g_signal_connect(button_, "destroy", G_CALLBACK(OnDestroy), this);
    observer_ = controller_->AddObserver([this] { Refresh(); });
    layout_listener_ = context_->OnLayoutChanged([this](bool narrow) {
      // research/ux.md 3.8: the chip shrinks to the avatar.
      gtk_widget_set_visible(names_, !narrow);
    });
    gtk_widget_set_visible(names_, !context_->narrow());
    Refresh();
    return button_;
  }

 private:
  static void OnDestroy(GtkWidget*, gpointer data) {
    auto* view = static_cast<AccountChipView*>(data);
    view->controller_->RemoveObserver(view->observer_);
    view->context_->RemoveListener(view->layout_listener_);
    view->destroyed_ = true;
  }

  static void OnPopoverMapped(GtkWidget*, gpointer data) {
    auto* view = static_cast<AccountChipView*>(data);
    if (view->focus_row_ != nullptr) gtk_widget_grab_focus(view->focus_row_);
  }

  void Refresh() {
    if (destroyed_) return;
    AccountsController& controller = *controller_;
    const AccountChipState state = controller.chip();
    std::string name;
    std::string detail;
    switch (state.kind) {
      case AccountChipKind::kAccount:
        name = ShownName(*state.account);
        detail = StatusOf(*state.account) == AccountStatus::kSignedOut
                     ? std::string(_("Signed out"))
                     : Handle(*state.account);
        break;
      case AccountChipKind::kGuest:
        name = _("Guest");
        detail = _("Not signed in");
        break;
      case AccountChipKind::kNotSignedIn:
        // research/ux.md 4.2: "Not signed in · Sign in".
        name = _("Not signed in");
        detail = _("Sign in");
        break;
      case AccountChipKind::kLegacySession:
        name = _("Saved sign-in");
        detail = _("Not checked yet");
        break;
      case AccountChipKind::kEnvironment:
        name = _("Set by environment");
        detail = controller.override_variables().empty()
                     ? std::string()
                     : controller.override_variables().front();
        break;
      case AccountChipKind::kUnavailable:
        name = _("Accounts unavailable");
        detail = _("See Accounts");
        break;
    }
    gtk_label_set_text(GTK_LABEL(name_), name.c_str());
    gtk_label_set_text(GTK_LABEL(detail_), detail.c_str());
    gtk_widget_set_visible(detail_, !detail.empty());

    GtkWidget* old = gtk_widget_get_first_child(avatar_slot_);
    if (old != nullptr) gtk_box_remove(GTK_BOX(avatar_slot_), old);
    gtk_box_append(GTK_BOX(avatar_slot_),
                   NewAvatar(controller, state.account, kSmallAvatarSize));

    const std::string label =
        Format(_("Account for this launch: %s"), name.c_str());
    gtk_widget_set_tooltip_text(button_, label.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(button_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(),
                                   -1);
    RebuildPopover();
  }

  void RebuildPopover() {
    focus_row_ = nullptr;
    while (GtkWidget* child = gtk_widget_get_first_child(popover_box_)) {
      gtk_box_remove(GTK_BOX(popover_box_), child);
    }
    AccountsController& controller = *controller_;
    if (controller.environment_override()) {
      AppendNote(Format(_("%s chooses the account for this launch; saved "
                          "accounts are not used."),
                        Join(controller.override_variables(), ", ").c_str()));
    } else if (!controller.load_error().empty()) {
      AppendNote(_("Saved accounts cannot be read"));
    } else {
      GtkWidget* list = gtk_list_box_new();
      gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
      gtk_widget_add_css_class(list, "boxed-list");
      for (const runtime::SavedAccount* account :
           controller.model().VisibleAccounts()) {
        const std::string subtitle =
            StatusOf(*account) == AccountStatus::kSignedOut
                ? Join({Handle(*account), _("signed out")}, " · ")
                : Handle(*account);
        gtk_list_box_append(
            GTK_LIST_BOX(list),
            NewChoiceRow(ShownName(*account), subtitle,
                         NewAvatar(controller, account, kSmallAvatarSize),
                         AccountPointer(account->user_id)));
      }
      gtk_list_box_append(
          GTK_LIST_BOX(list),
          NewChoiceRow(_("Play as guest"), _("Start signed out"),
                       NewAvatar(controller, nullptr, kSmallAvatarSize),
                       runtime::ActiveAccountPointer{}));
      if (focus_row_ == nullptr) {
        focus_row_ =
            GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
      }
      gtk_box_append(GTK_BOX(popover_box_), list);

      GtkWidget* add = NewMenuButton(_("_Add Account…"));
      SetUnavailable(add, controller.AddUnavailableReason());
      const Callback start =
          Later(controller_,
                [](AccountsController& accounts) { accounts.StartSignIn(); });
      ConnectCallback(add, "clicked", [this, start] {
        gtk_popover_popdown(GTK_POPOVER(popover_));
        start();
      });
      gtk_box_append(GTK_BOX(popover_box_), add);
    }
    GtkWidget* manage = NewMenuButton(_("_Manage Accounts…"));
    ConnectCallback(manage, "clicked", [this] {
      gtk_popover_popdown(GTK_POPOVER(popover_));
      context_->ShowSection(Section::kAccounts);
    });
    gtk_box_append(GTK_BOX(popover_box_), manage);
  }

  GtkWidget* NewChoiceRow(const std::string& title, const std::string& subtitle,
                          GtkWidget* avatar,
                          const runtime::ActiveAccountPointer& pointer) {
    GtkWidget* row = NewStatusRow(title, subtitle);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), avatar);
    const bool selected = controller_->model().IsSelected(pointer);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), NewCheck(selected));
    if (selected) focus_row_ = row;
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    const Callback later =
        Later(controller_, [pointer](AccountsController& controller) {
          controller.Choose(pointer);
        });
    const Callback choose = [this, later] {
      gtk_popover_popdown(GTK_POPOVER(popover_));
      later();
    };
    ConnectCallback(row, "activated", choose);
    // Also as an action of the row, which assistive technologies list
    // (a list row's activation is not one of them).
    GSimpleActionGroup* actions = g_simple_action_group_new();
    ConnectAction(actions, "choose", true, choose);
    gtk_widget_insert_action_group(row, "chip", G_ACTION_GROUP(actions));
    g_object_unref(actions);
    return row;
  }

  GtkWidget* NewMenuButton(const char* mnemonic_label) {
    GtkWidget* button = gtk_button_new_with_mnemonic(mnemonic_label);
    gtk_widget_add_css_class(button, "flat");
    GtkWidget* label = gtk_button_get_child(GTK_BUTTON(button));
    if (GTK_IS_LABEL(label)) gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    return button;
  }

  void AppendNote(const std::string& text) {
    GtkWidget* note = gtk_label_new(text.c_str());
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 36);
    gtk_label_set_xalign(GTK_LABEL(note), 0.0F);
    gtk_widget_set_margin_start(note, 6);
    gtk_widget_set_margin_end(note, 6);
    gtk_box_append(GTK_BOX(popover_box_), note);
  }

  LauncherContext* context_;
  std::shared_ptr<AccountsController> controller_;
  GtkWidget* button_ = nullptr;
  GtkWidget* avatar_slot_ = nullptr;
  GtkWidget* names_ = nullptr;
  GtkWidget* name_ = nullptr;
  GtkWidget* detail_ = nullptr;
  GtkWidget* popover_ = nullptr;
  GtkWidget* popover_box_ = nullptr;
  // The popover's row to focus when it opens; a child of popover_box_.
  GtkWidget* focus_row_ = nullptr;
  AccountsController::ObserverId observer_ = 0;
  LauncherContext::ListenerId layout_listener_ = 0;
  bool destroyed_ = false;
};

}  // namespace

// Accounts: the saved accounts and the one Play uses, adding accounts on the
// website or inside Roblox, and the sign-in method. research/ux.md 4.3
// ACCOUNTS, research/auth.md 5, SPEC 4-5.
GtkWidget* BuildAccountsPage(LauncherContext* context) {
  auto* view =
      new AccountsPageView(context, AccountsController::ForContext(context));
  return view->Build();
}

// The launch bar's account chip (research/ux.md 4.2): the account Play uses
// and a popover to switch, play as guest, add or manage accounts.
GtkWidget* BuildAccountChip(LauncherContext* context) {
  auto* view =
      new AccountChipView(context, AccountsController::ForContext(context));
  return view->Build();
}

}  // namespace mocktail::launcher_ui
