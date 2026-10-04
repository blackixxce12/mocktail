#include "runtime/desktop_notification.h"

#include <gio/gio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

// Every test points DBUS_SESSION_BUS_ADDRESS at a private bus or at nothing,
// so no notification ever reaches the desktop.

constexpr char kNotificationsXml[] = R"XML(<node>
  <interface name="org.freedesktop.Notifications">
    <method name="Notify">
      <arg type="s" direction="in"/>
      <arg type="u" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="as" direction="in"/>
      <arg type="a{sv}" direction="in"/>
      <arg type="i" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
  </interface>
</node>)XML";

void HandleNotify(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar*, GVariant* parameters,
                  GDBusMethodInvocation* invocation, gpointer user_data) {
  gchar* printed = g_variant_print(parameters, TRUE);
  *static_cast<std::string*>(user_data) = printed;
  g_free(printed);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 7u));
}

TEST(DesktopNotificationTest, FailsQuietlyWithoutASessionBus) {
  gchar* missing =
      g_build_filename(g_get_tmp_dir(), "mocktail-no-such-bus", nullptr);
  const std::string address = std::string("unix:path=") + missing;
  g_free(missing);
  ASSERT_EQ(setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1), 0);

  testing::internal::CaptureStderr();
  EXPECT_FALSE(ShowDesktopNotification("Summary", "Body"));
  EXPECT_FALSE(ShowDesktopNotification("Summary", "Body"));
  const std::string log = testing::internal::GetCapturedStderr();
  std::size_t lines = 0;
  for (std::size_t at = log.find("[notify]"); at != std::string::npos;
       at = log.find("[notify]", at + 1)) {
    ++lines;
  }
  EXPECT_LE(lines, 1U) << log;
  EXPECT_FALSE(ShowDesktopNotification(nullptr, "Body"));
}

TEST(DesktopNotificationTest, NotifiesAsMocktailThroughTheSessionBus) {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (daemon == nullptr) {
    GTEST_SKIP() << "dbus-daemon is not installed";
  }
  g_free(daemon);
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  // Points DBUS_SESSION_BUS_ADDRESS at the private bus.
  g_test_dbus_up(bus);
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);

  GError* error = nullptr;
  GDBusConnection* server = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  ASSERT_NE(server, nullptr) << error->message;

  // No notification server yet.
  EXPECT_FALSE(ShowDesktopNotification("Summary", "Body"));

  GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kNotificationsXml, &error);
  ASSERT_NE(node, nullptr) << error->message;
  std::string received;
  const GDBusInterfaceVTable vtable{HandleNotify, nullptr, nullptr, {}};
  const guint registration = g_dbus_connection_register_object(
      server, "/org/freedesktop/Notifications", node->interfaces[0], &vtable,
      &received, nullptr, &error);
  ASSERT_NE(registration, 0U) << error->message;
  GVariant* owned = g_dbus_connection_call_sync(
      server, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.freedesktop.Notifications", 4u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  ASSERT_NE(owned, nullptr) << error->message;
  g_variant_unref(owned);

  std::atomic<bool> done{false};
  bool shown = false;
  std::thread client([&] {
    shown = ShowDesktopNotification("Summary", "Body text");
    done = true;
  });
  const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!done && g_get_monotonic_time() < deadline) {
    if (!g_main_context_iteration(context, FALSE)) {
      g_usleep(5000);
    }
  }
  client.join();
  EXPECT_TRUE(shown);
  // The worker has no main context of its own. Nothing may be left queued on
  // the global default context, which Mocktail never iterates: a queued
  // "closed" notice would keep the connection alive forever.
  EXPECT_FALSE(g_main_context_pending(nullptr));
  EXPECT_EQ(received,
            "('Mocktail', uint32 0, 'space.bigrat.mocktail', 'Summary', "
            "'Body text', @as [], {'desktop-entry': "
            "<'space.bigrat.mocktail'>}, 8000)");

  g_dbus_connection_unregister_object(server, registration);
  g_dbus_node_info_unref(node);
  (void)g_dbus_connection_close_sync(server, nullptr, nullptr);
  g_object_unref(server);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  g_test_dbus_down(bus);
  g_object_unref(bus);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
