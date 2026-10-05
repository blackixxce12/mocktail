// mocktail_launcher_ui: the settings window mocktail shows before Roblox
// starts. See include/runtime/launcher_ui_launch.h for how mocktail runs it
// and reads its answer, and launcher_context.h for the window's structure.

#include <adwaita.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/selftest.h"
#include "launcher_ui/style.h"
#include "launcher_ui/window.h"
#include "runtime/launcher_ui_launch.h"

namespace {

using mocktail::launcher_ui::LauncherContext;
using mocktail::launcher_ui::LauncherOptions;
using mocktail::launcher_ui::LauncherWindow;
using mocktail::launcher_ui::Selftest;
using mocktail::launcher_ui::WindowOptions;

struct Arguments {
  std::filesystem::path selftest_directory;
  std::filesystem::path config_file;
  WindowOptions window;
  std::string section;
};

constexpr char kUsage[] =
    "Usage: mocktail_launcher_ui [--size WxH] [--section ID] "
    "[--config FILE]\n"
    "       mocktail_launcher_ui --selftest OUT_DIR [--size WxH]\n"
    "Mocktail runs this window itself before Roblox starts; run\n"
    "`mocktail --launcher` to open it.\n";

bool ParseSize(std::string_view text, WindowOptions* options) {
  const std::size_t x = text.find('x');
  if (x == std::string_view::npos) return false;
  int width = 0;
  int height = 0;
  const std::string_view w = text.substr(0, x);
  const std::string_view h = text.substr(x + 1);
  const auto parsed_width =
      std::from_chars(w.data(), w.data() + w.size(), width);
  const auto parsed_height =
      std::from_chars(h.data(), h.data() + h.size(), height);
  if (parsed_width.ec != std::errc() ||
      parsed_width.ptr != w.data() + w.size() ||
      parsed_height.ec != std::errc() ||
      parsed_height.ptr != h.data() + h.size() || width < 200 || height < 200 ||
      width > 16384 || height > 16384) {
    return false;
  }
  options->width = width;
  options->height = height;
  return true;
}

bool ParseArguments(int argc, char** argv, Arguments* arguments) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    const bool has_value = index + 1 < argc;
    if (argument == "--selftest" && has_value) {
      arguments->selftest_directory = std::filesystem::absolute(argv[++index]);
    } else if (argument == "--size" && has_value) {
      if (!ParseSize(argv[++index], &arguments->window)) return false;
    } else if (argument == "--config" && has_value) {
      arguments->config_file = std::filesystem::absolute(argv[++index]);
    } else if (argument == "--section" && has_value) {
      arguments->section = argv[++index];
    } else {
      return false;
    }
  }
  return true;
}

// The pipe mocktail reads the answer from. Kept away from every process
// this window starts (the WebKit sign-in helper, a file manager), so
// mocktail never waits for one of them.
int TakeResultDescriptor() {
  const char* value = std::getenv(
      std::string(mocktail::runtime::kLauncherUiResultFdVariable).c_str());
  int descriptor = -1;
  if (value != nullptr) {
    const std::string_view text(value);
    std::from_chars(text.data(), text.data() + text.size(), descriptor);
  }
  unsetenv(std::string(mocktail::runtime::kLauncherUiResultFdVariable).c_str());
  if (descriptor < 3 || fcntl(descriptor, F_GETFD) < 0) {
    return -1;
  }
  fcntl(descriptor, F_SETFD, FD_CLOEXEC);
  return descriptor;
}

void Report(int descriptor, mocktail::runtime::LauncherUiResult result) {
  std::string line(mocktail::runtime::LauncherUiResultLine(result));
  if (descriptor < 0) {
    std::printf("mocktail-launcher-ui result=%s\n", line.c_str());
    std::fflush(stdout);
    return;
  }
  line += '\n';
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t count =
        write(descriptor, line.data() + written, line.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  close(descriptor);
}

struct Application {
  Arguments arguments;
  LauncherContext* context = nullptr;
  std::unique_ptr<LauncherWindow> window;
  std::unique_ptr<Selftest> selftest;
};

void Activate(GApplication* application, gpointer data) {
  auto* app = static_cast<Application*>(data);
  if (app->window != nullptr) {
    app->window->Present();
    return;
  }
  mocktail::launcher_ui::ApplyLauncherStyle();
  app->window = std::make_unique<LauncherWindow>(
      ADW_APPLICATION(application), app->context, app->arguments.window);
  if (const mocktail::launcher_ui::SectionInfo* section =
          mocktail::launcher_ui::FindSection(app->arguments.section.c_str())) {
    app->window->ShowSection(section->section);
  }
  // Slow work only after the window exists: the renderer alone may take
  // seconds on a cold start.
  app->context->StartMachineDetection();
  app->context->StartFileMonitor();
  if (!app->arguments.selftest_directory.empty()) {
    app->selftest = std::make_unique<Selftest>(
        ADW_APPLICATION(application), app->context, app->window.get(),
        app->arguments.selftest_directory);
    Selftest* selftest = app->selftest.get();
    app->window->OnReady([selftest] { selftest->Start(); });
  }
  app->window->Present();
}

}  // namespace

int main(int argc, char** argv) {
  const int result_descriptor = TakeResultDescriptor();
  Application app;
  if (!ParseArguments(argc, argv, &app.arguments)) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const bool selftest = !app.arguments.selftest_directory.empty();
  if (selftest) {
    std::string error;
    if (!mocktail::launcher_ui::PrepareSelftestEnvironment(
            app.arguments.selftest_directory, &error)) {
      std::fprintf(stderr, "mocktail-launcher-ui selftest: %s\n",
                   error.c_str());
      return 1;
    }
  }
  mocktail::launcher_ui::InitTranslations(argv[0]);
  // Settle the theme before libadwaita starts (KDE sets this and libadwaita
  // warns about it, as in the failure dialog).
  if (!gtk_init_check()) {
    std::fputs("mocktail-launcher-ui: cannot open a display\n", stderr);
    // No window: let Roblox start as if Play was pressed.
    if (!selftest) {
      Report(result_descriptor, mocktail::runtime::LauncherUiResult::kPlay);
    }
    return 1;
  }
  // gtk_init set every category from the environment. Numbers are parsed
  // and printed by runtime code (config values, JSON) that expects '.' as
  // the decimal point, as in the game process; only messages and text
  // follow the user's locale.
  setlocale(LC_NUMERIC, "C");
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
               FALSE, nullptr);
  // The hints' paragraphs are selectable for copying; focusing one (Tab,
  // or a popover opening) must not select all of it.
  g_object_set(gtk_settings_get_default(), "gtk-label-select-on-focus", FALSE,
               nullptr);
  g_set_application_name("Mocktail");
  gtk_window_set_default_icon_name("space.bigrat.mocktail");

  LauncherOptions options;
  if (!app.arguments.config_file.empty()) {
    options.config_file = app.arguments.config_file;
  } else if (const char* config = std::getenv(
                 std::string(mocktail::runtime::kLauncherUiConfigFileVariable)
                     .c_str());
             config != nullptr && config[0] != '\0') {
    options.config_file = config;
  }
  const char* created = std::getenv(
      std::string(mocktail::runtime::kLauncherUiConfigCreatedVariable).c_str());
  options.config_created = created != nullptr && std::strcmp(created, "1") == 0;
  options.selftest = selftest;
  auto context = std::make_unique<LauncherContext>(options);
  context->Load();
  app.context = context.get();

  AdwApplication* application =
      adw_application_new("space.bigrat.mocktail", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate", G_CALLBACK(Activate), &app);
  char* run_argv[] = {argv[0], nullptr};
  const int status = g_application_run(G_APPLICATION(application), 1, run_argv);
  const mocktail::runtime::LauncherUiResult outcome = context->outcome();
  const int selftest_status =
      app.selftest != nullptr ? app.selftest->exit_code() : 1;
  app.selftest.reset();
  app.window.reset();
  g_object_unref(application);
  context.reset();

  if (selftest) {
    return status != 0 ? status : selftest_status;
  }
  Report(result_descriptor, outcome);
  return status;
}
