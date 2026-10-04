#include "runtime/host_launch_environment.h"

#include <gio/gio.h>

#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kMocktailPrefix = "MOCKTAIL_";

constexpr std::string_view kMocktailVariables[] = {
    // window.cc and main.cc: the game window's video driver and GPU tuning.
    "SDL_VIDEODRIVER",
    "SDL_VIDEO_DRIVER",
    "SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR",
    "NODEVICE_SELECT",
    "DISABLE_LAYER_MESA_ANTI_LAG",
    "ANGLE_DEFAULT_PLATFORM",
    "__GL_VRR_ALLOWED",
    "__GL_SHADER_DISK_CACHE",
    "__GL_SHADER_DISK_CACHE_SIZE",
    "__GL_YIELD",
    "__GL_THREADED_OPTIMIZATIONS",
    // graphics_launch_policy.cc: the direct Vulkan driver selection.
    "VK_LOADER_DRIVERS_DISABLE",
    "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES",
    "MESA_VK_WSI_PRESENT_MODE",
    "MESA_VK_ENABLE_SUBMIT_THREAD",
    "ANV_SYS_MEM_LIMIT",
    // Loader state for Mocktail's own and bundled libraries.
    "LD_PRELOAD",
    "LD_LIBRARY_PATH",
    "ROBLOX_LIB_PATH",
    "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS",
};

// mocktail-launcher.sh exports these for the bundled WebKitGTK only when
// MOCKTAIL_PORTABLE_MODE=standalone; elsewhere they belong to the user.
constexpr std::string_view kStandaloneBundleVariables[] = {
    "WEBKIT_EXEC_PATH",
    "WEBKIT_INJECTED_BUNDLE_PATH",
    "GST_PLUGIN_SYSTEM_PATH_1_0",
    "GST_PLUGIN_SCANNER",
    "GST_PLUGIN_PATH_1_0",
    "GIO_EXTRA_MODULES",
    "GIO_USE_TLS",
    "GDK_PIXBUF_MODULE_FILE",
    "GLYCIN_DATA_DIR",
    "GSETTINGS_SCHEMA_DIR",
    "FONTCONFIG_FILE",
    "JAVA_HOME",
    "SSL_CERT_FILE",
};

}  // namespace

bool IsMocktailOnlyEnvironmentVariable(std::string_view name,
                                       bool standalone_bundle) {
  if (name.substr(0, kMocktailPrefix.size()) == kMocktailPrefix) {
    return true;
  }
  for (const std::string_view variable : kMocktailVariables) {
    if (name == variable) {
      return true;
    }
  }
  if (standalone_bundle) {
    for (const std::string_view variable : kStandaloneBundleVariables) {
      if (name == variable) {
        return true;
      }
    }
  }
  return false;
}

void RemoveMocktailEnvironment(GAppLaunchContext* context) {
  if (context == nullptr) {
    return;
  }
  gchar** environment = g_get_environ();
  const char* portable_mode =
      g_environ_getenv(environment, "MOCKTAIL_PORTABLE_MODE");
  const bool standalone_bundle =
      portable_mode != nullptr &&
      std::string_view(portable_mode) == "standalone";
  for (gchar** entry = environment; *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    const std::string name(variable.substr(0, variable.find('=')));
    if (IsMocktailOnlyEnvironmentVariable(name, standalone_bundle)) {
      g_app_launch_context_unsetenv(context, name.c_str());
    }
  }
  g_strfreev(environment);
}

}  // namespace runtime
}  // namespace mocktail
