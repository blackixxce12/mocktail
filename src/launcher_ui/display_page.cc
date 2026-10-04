#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Display. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildDisplayPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kDisplay,
              _("The game window: its size and start mode, sharpness on scaled "
                "screens, the display server and the Roblox theme"));
  // TODO(display page) research/ux.md 4.3 DISPLAY, SPEC 5:
  //   Window: display.start_mode ("Start in"), window.width/height presets
  //     from the monitor (Save also updates window-state.json), [adv]
  //     window.title.
  //   Scaling: window.high_dpi with the computed resolution, read-only
  //     "Game resolution" summary.
  //   Display server: display.server (Wayland insensitive without
  //     WAYLAND_DISPLAY, X11 without DISPLAY; NVIDIA + Vulkan note).
  //   Roblox interface: appearance.theme.
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
