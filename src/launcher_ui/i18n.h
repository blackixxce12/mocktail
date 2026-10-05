#ifndef MOCKTAIL_LAUNCHER_UI_I18N_H_
#define MOCKTAIL_LAUNCHER_UI_I18N_H_

// Every user-visible string of the settings window goes through gettext:
// _("...") where it is shown, N_("...") in tables translated later with
// _(). The catalogue is the "mocktail" domain (po/), built by CMake.
#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "mocktail"
#endif
#include <glib/gi18n.h>

#include <string>

namespace mocktail::launcher_ui {

// setlocale(LC_ALL, ""), then binds the domain to the first directory that
// holds a compiled catalogue: MOCKTAIL_LOCALE_DIR, <dir of the executable>/
// locale (the build tree), the install layout relative to the executable
// for relocated and portable installs (../../share/locale from
// <prefix>/lib/mocktail, ../share/locale from a bin directory), the
// configured install directory, then the build tree's directory. Returns the
// directory bound, empty when no catalogue was found (the window then shows
// English).
std::string InitTranslations(const char* argv0);

// printf-style formatting for translated strings (g_strdup_vprintf).
std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

// `value` with `digits` decimals and the decimal separator of the user's
// locale ("14,9" in Russian). main.cc keeps LC_NUMERIC at "C" for the
// runtime's parsers, so "%.1f" always printed a point.
std::string DecimalText(double value, int digits);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_I18N_H_
