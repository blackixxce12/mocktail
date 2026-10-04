#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Advanced. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildAdvancedPage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kAdvanced,
                            _("The device Roblox sees, Fast Flags, this window "
                              "and Mocktail's files"));
  // TODO(advanced page) research/ux.md 4.3 ADVANCED, SPEC 5:
  //   Device: device presets (a detailed mapping is shown read-only).
  //   Fast Flags: editor subpage (FastFlagsDocument as a DirtySource).
  //   Launcher: launcher.show_on_start.
  //   Files: open config.yaml, open the data folder.
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
