#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"

namespace mocktail::launcher_ui {

// Audio. See bindings.h for the row API and graphics_page.cc for a fully
// hinted example.
GtkWidget* BuildAudioPage(LauncherContext* context) {
  GtkWidget* page =
      NewPage(context, Section::kAudio,
              _("Where game sound plays and which microphone voice chat uses"));
  // TODO(audio page) research/ux.md 4.3 AUDIO: audio.output_device and
  //   audio.input_device from SDL3 enumeration on a worker; a missing saved
  //   device is shown as "<name> (not connected)".
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
