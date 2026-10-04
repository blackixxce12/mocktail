#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// About. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildAboutPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kAbout,
              _("Versions, this computer, and help when something goes wrong"));
  // TODO(about page) research/ux.md 4.3 ABOUT, SPEC 5: Mocktail version
  //   and commit (MOCKTAIL_PROJECT_VERSION and MOCKTAIL_BUILD_GIT_COMMIT are
  //   defined for this target), Roblox version (mocktail_updater status),
  //   check for updates, system info from context->machine(), open logs,
  //   copy diagnostic info (no secrets).
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
