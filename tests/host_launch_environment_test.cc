#include "runtime/host_launch_environment.h"

#include <gio/gio.h>
#include <gtest/gtest.h>

#include <cstdlib>

namespace mocktail {
namespace runtime {
namespace {

TEST(HostLaunchEnvironmentTest, NamesOnlyMocktailVariables) {
  for (const char* name : {
           "MOCKTAIL_GRAPHICS_BACKEND",
           "MOCKTAIL_PORTABLE_MODE",
           "SDL_VIDEODRIVER",
           "SDL_VIDEO_DRIVER",
           "NODEVICE_SELECT",
           "DISABLE_LAYER_MESA_ANTI_LAG",
           "LD_PRELOAD",
           "LD_LIBRARY_PATH",
           "ROBLOX_LIB_PATH",
           "__GL_THREADED_OPTIMIZATIONS",
           "VK_DRIVER_FILES",
       }) {
    EXPECT_TRUE(IsMocktailOnlyEnvironmentVariable(name, false)) << name;
  }
  for (const char* name : {
           "PATH",
           "HOME",
           "DISPLAY",
           "WAYLAND_DISPLAY",
           "XDG_RUNTIME_DIR",
           "XDG_DATA_DIRS",
           "DBUS_SESSION_BUS_ADDRESS",
           "LANG",
           "MOCKTAIL",
           "mocktail_theme",
           "SDL_VIDEODRIVER_X",
           "GIO_EXTRA_MODULES",
           "WEBKIT_EXEC_PATH",
       }) {
    EXPECT_FALSE(IsMocktailOnlyEnvironmentVariable(name, false)) << name;
  }
  EXPECT_TRUE(IsMocktailOnlyEnvironmentVariable("GIO_EXTRA_MODULES", true));
  EXPECT_TRUE(IsMocktailOnlyEnvironmentVariable("WEBKIT_EXEC_PATH", true));
  EXPECT_FALSE(IsMocktailOnlyEnvironmentVariable("PATH", true));
}

// Inspects the launch environment only; nothing is launched.
TEST(HostLaunchEnvironmentTest, LaunchContextDropsMocktailVariables) {
  ASSERT_EQ(setenv("MOCKTAIL_TEST_SETTING", "1", 1), 0);
  ASSERT_EQ(setenv("SDL_VIDEODRIVER", "x11", 1), 0);
  ASSERT_EQ(setenv("NODEVICE_SELECT", "1", 1), 0);
  ASSERT_EQ(setenv("GIO_EXTRA_MODULES", "/bundle/gio", 1), 0);
  ASSERT_EQ(setenv("HOST_LAUNCH_ENVIRONMENT_TEST_KEPT", "kept", 1), 0);
  ASSERT_EQ(unsetenv("MOCKTAIL_PORTABLE_MODE"), 0);

  GAppLaunchContext* context = g_app_launch_context_new();
  RemoveMocktailEnvironment(context);
  gchar** environment = g_app_launch_context_get_environment(context);
  EXPECT_EQ(g_environ_getenv(environment, "MOCKTAIL_TEST_SETTING"), nullptr);
  EXPECT_EQ(g_environ_getenv(environment, "SDL_VIDEODRIVER"), nullptr);
  EXPECT_EQ(g_environ_getenv(environment, "NODEVICE_SELECT"), nullptr);
  EXPECT_STREQ(g_environ_getenv(environment, "GIO_EXTRA_MODULES"),
               "/bundle/gio");
  EXPECT_STREQ(g_environ_getenv(environment,
                                "HOST_LAUNCH_ENVIRONMENT_TEST_KEPT"),
               "kept");
  g_strfreev(environment);
  g_object_unref(context);

  ASSERT_EQ(setenv("MOCKTAIL_PORTABLE_MODE", "standalone", 1), 0);
  context = g_app_launch_context_new();
  RemoveMocktailEnvironment(context);
  environment = g_app_launch_context_get_environment(context);
  EXPECT_EQ(g_environ_getenv(environment, "MOCKTAIL_PORTABLE_MODE"), nullptr);
  EXPECT_EQ(g_environ_getenv(environment, "GIO_EXTRA_MODULES"), nullptr);
  EXPECT_STREQ(g_environ_getenv(environment,
                                "HOST_LAUNCH_ENVIRONMENT_TEST_KEPT"),
               "kept");
  g_strfreev(environment);
  g_object_unref(context);

  // Mocktail's own process environment is left as it was.
  EXPECT_STREQ(std::getenv("SDL_VIDEODRIVER"), "x11");
  EXPECT_STREQ(std::getenv("MOCKTAIL_TEST_SETTING"), "1");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
