#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Integrations. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildIntegrationsPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kIntegrations,
              _("Your status in Discord and Fleasion asset replacement"));
  // TODO(integrations page) research/ux.md 4.3 INTEGRATIONS:
  //   Discord: integrations.discord_rpc.* expander, texts subpage.
  //   Fleasion: integrations.fleasion.* with the proxy conflict rules.
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
