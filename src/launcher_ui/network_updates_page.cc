#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Network & Updates. See bindings.h for the row API and graphics_page.cc for a
// fully hinted example.
GtkWidget* BuildNetworkUpdatesPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kNetworkUpdates,
              _("How Roblox is kept up to date and which proxy Mocktail uses"));
  // TODO(network & updates page) research/ux.md 4.3 NETWORK & UPDATES:
  //   Updates: updates.automatic, installed Roblox version, [adv]
  //     updates.source.
  //   Proxy: network.use_system_proxy / proxy_host + proxy_port, [adv]
  //     network.ca_bundle.
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
