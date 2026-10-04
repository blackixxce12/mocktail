#include "launcher_ui/i18n.h"

#include <clocale>
#include <cstdarg>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifndef MOCKTAIL_LAUNCHER_LOCALEDIR
#define MOCKTAIL_LAUNCHER_LOCALEDIR "/usr/share/locale"
#endif
#ifndef MOCKTAIL_LAUNCHER_BUILD_LOCALEDIR
#define MOCKTAIL_LAUNCHER_BUILD_LOCALEDIR ""
#endif
// Space-separated languages with a catalogue (po/LINGUAS).
#ifndef MOCKTAIL_LAUNCHER_LINGUAS
#define MOCKTAIL_LAUNCHER_LINGUAS "ru"
#endif

namespace mocktail::launcher_ui {
namespace {

bool HoldsCatalogue(const std::filesystem::path& directory) {
  std::error_code error;
  if (directory.empty() || !std::filesystem::is_directory(directory, error)) {
    return false;
  }
  const std::string languages = MOCKTAIL_LAUNCHER_LINGUAS;
  std::size_t start = 0;
  while (start < languages.size()) {
    std::size_t end = languages.find(' ', start);
    if (end == std::string::npos) end = languages.size();
    const std::string language = languages.substr(start, end - start);
    if (!language.empty() &&
        std::filesystem::is_regular_file(
            directory / language / "LC_MESSAGES" / (GETTEXT_PACKAGE ".mo"),
            error)) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

std::filesystem::path ExecutableDirectory(const char* argv0) {
  std::error_code error;
  std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    executable = argv0 != nullptr ? std::filesystem::path(argv0)
                                  : std::filesystem::path();
  }
  return executable.parent_path();
}

}  // namespace

std::string InitTranslations(const char* argv0) {
  setlocale(LC_ALL, "");
  const std::filesystem::path executable_directory = ExecutableDirectory(argv0);
  std::vector<std::filesystem::path> candidates;
  if (const char* override_directory = g_getenv("MOCKTAIL_LOCALE_DIR");
      override_directory != nullptr && override_directory[0] != '\0') {
    candidates.emplace_back(override_directory);
  }
  if (!executable_directory.empty()) {
    // The build tree: <build>/mocktail_launcher_ui and <build>/locale.
    candidates.push_back(executable_directory / "locale");
    // <prefix>/lib/mocktail/mocktail_launcher_ui -> <prefix>/share/locale
    candidates.push_back(executable_directory.parent_path().parent_path() /
                         "share" / "locale");
    // <prefix>/bin or a portable bundle's bin -> <prefix>/share/locale
    candidates.push_back(executable_directory.parent_path() / "share" /
                         "locale");
  }
  candidates.emplace_back(MOCKTAIL_LAUNCHER_LOCALEDIR);
  candidates.emplace_back(MOCKTAIL_LAUNCHER_BUILD_LOCALEDIR);
  std::string bound;
  for (const std::filesystem::path& candidate : candidates) {
    if (HoldsCatalogue(candidate)) {
      bound = candidate.string();
      break;
    }
  }
  bindtextdomain(GETTEXT_PACKAGE,
                 bound.empty() ? MOCKTAIL_LAUNCHER_LOCALEDIR : bound.c_str());
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);
  return bound;
}

std::string Format(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  gchar* text = g_strdup_vprintf(format, arguments);
  va_end(arguments);
  std::string result(text != nullptr ? text : "");
  g_free(text);
  return result;
}

}  // namespace mocktail::launcher_ui
