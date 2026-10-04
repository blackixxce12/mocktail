#include "launcher_ui/style.h"

#include <adwaita.h>

namespace mocktail::launcher_ui {
namespace {

constexpr char kStyle[] = R"css(
.launch-bar {
  padding: 6px 12px;
  border-spacing: 8px;
}

.launch-bar .launch-status {
  margin: 0 4px;
}

button.play-button {
  min-width: 96px;
  padding-left: 20px;
  padding-right: 20px;
}

.env-badge,
.recommended-badge {
  font-size: 0.75em;
  font-weight: 800;
  padding: 1px 7px;
  border-radius: 999px;
}

.env-badge {
  color: var(--warning-color);
  background-color: color-mix(in srgb, var(--warning-bg-color) 22%, transparent);
}

.recommended-badge {
  color: var(--success-color);
  background-color: color-mix(in srgb, var(--success-bg-color) 22%, transparent);
}

.launcher-warning label {
  color: var(--warning-color);
}

row.search-highlight {
  background-color: color-mix(in srgb, var(--accent-bg-color) 22%, transparent);
  transition: background-color 400ms ease-out;
}

popover.hint-popover > contents {
  padding: 8px;
}

.combo-option {
  padding: 4px 0;
}

.account-chip {
  padding: 2px 8px 2px 2px;
}
)css";

}  // namespace

void ApplyLauncherStyle() {
  AdwStyleManager* style = adw_style_manager_get_default();
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kStyle);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

}  // namespace mocktail::launcher_ui
