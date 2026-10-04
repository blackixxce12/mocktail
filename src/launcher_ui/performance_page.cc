#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Performance. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildPerformancePage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kPerformance,
                            _("How Roblox uses your processor and memory, and "
                              "whether Feral GameMode is requested"));
  // TODO(performance page) research/ux.md 4.3 PERFORMANCE:
  //   Processor: performance.multithreaded_rendering, [adv]
  //     performance.physics_worker_mode.
  //   System: performance.gamemode, [adv] performance.memory_limit_mb.
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
