#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Accounts. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildAccountsPage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kAccounts,
                            _("The Roblox accounts saved on this computer and "
                              "how you sign in"));
  // TODO(accounts page) research/ux.md 4.3 ACCOUNTS, research/auth.md 5,
  // SPEC 4-5:
  //   Saved accounts: AccountStore (MakeAccountStoreOptions(context->paths(),
  //     ..., assume_exclusive=true); never in context->selftest()), one row
  //     per account (avatar, display name, @username), add through
  //     BrowserSignInSession polled from a GLib timeout, remove with a
  //     confirmation.
  //   Sign-in: account.sign_in.
  //   Play: context->AddPlayHook(...) writes the selection
  //     (AccountStore::SelectForLaunch) after Save, before "play".
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

// The launch bar's account chip. TODO(accounts page): avatar from the
// store's cached headshot at the surface scale, display name over
// @username, a popover with the saved accounts, "Play as guest", "Add
// account…" and "Manage accounts…"; names hidden while the window is
// narrow (context->OnLayoutChanged).
GtkWidget* BuildAccountChip(LauncherContext* context) {
  GtkWidget* button = gtk_menu_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "account-chip");
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Account for this launch"), -1);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* avatar = adw_avatar_new(32, nullptr, FALSE);
  gtk_box_append(GTK_BOX(content), avatar);
  GtkWidget* names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
  GtkWidget* name = gtk_label_new(_("Not signed in"));
  gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(name, "heading");
  gtk_box_append(GTK_BOX(names), name);
  GtkWidget* detail = gtk_label_new(_("Sign in"));
  gtk_label_set_xalign(GTK_LABEL(detail), 0.0F);
  gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(detail, "caption");
  gtk_widget_add_css_class(detail, "dim-label");
  gtk_box_append(GTK_BOX(names), detail);
  gtk_box_append(GTK_BOX(content), names);
  gtk_menu_button_set_child(GTK_MENU_BUTTON(button), content);
  gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), TRUE);

  GtkWidget* popover = gtk_popover_new();
  GtkWidget* manage = gtk_button_new_with_mnemonic(_("_Manage Accounts…"));
  gtk_widget_add_css_class(manage, "flat");
  g_signal_connect(
      manage, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
        GtkWidget* owner =
            gtk_widget_get_ancestor(GTK_WIDGET(clicked), GTK_TYPE_POPOVER);
        if (owner != nullptr) gtk_popover_popdown(GTK_POPOVER(owner));
        static_cast<LauncherContext*>(data)->ShowSection(Section::kAccounts);
      }),
      context);
  gtk_popover_set_child(GTK_POPOVER(popover), manage);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);

  context->OnLayoutChanged(
      [names](bool narrow) { gtk_widget_set_visible(names, !narrow); });
  return button;
}

}  // namespace mocktail::launcher_ui
