#include "runtime/desktop_notification.h"

#include <gio/gio.h>

#include <atomic>
#include <cstdio>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kApplicationName[] = "Mocktail";
constexpr char kApplicationId[] = "space.bigrat.mocktail";
constexpr gint32 kExpireTimeoutMs = 8000;
constexpr gint kCallTimeoutMs = 5000;

std::atomic<bool> g_failure_logged{false};

bool Fail(const char* step, GError* error) {
  if (!g_failure_logged.exchange(true)) {
    std::fprintf(stderr,
                 "  [notify] desktop notifications are unavailable (%s: %s)\n",
                 step, error != nullptr ? error->message : "unknown error");
  }
  g_clear_error(&error);
  return false;
}

bool ShowWithPrivateConnection(const char* summary, const char* body);

}  // namespace

bool ShowDesktopNotification(const char* summary, const char* body) {
  if (summary == nullptr || body == nullptr) {
    return false;
  }
  // GDBus queues the connection's "closed" notice on the thread-default main
  // context. The checkout worker has none, and nothing iterates the global
  // default context, so every connection would stay referenced forever. Own a
  // context for this call and drain it once the connection is released.
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  const bool shown = ShowWithPrivateConnection(summary, body);
  while (g_main_context_iteration(context, FALSE)) {
  }
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  return shown;
}

namespace {

bool ShowWithPrivateConnection(const char* summary, const char* body) {
  GError* error = nullptr;
  gchar* address =
      g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (address == nullptr) {
    return Fail("session bus", error);
  }
  // A private connection: the shared session bus singleton would exit the
  // whole process if the bus went away.
  GDBusConnection* connection = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  g_free(address);
  if (connection == nullptr) {
    return Fail("session bus", error);
  }
  GVariantBuilder hints;
  g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&hints, "{sv}", "desktop-entry",
                        g_variant_new_string(kApplicationId));
  GVariant* reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.Notifications",
      "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
      "Notify",
      g_variant_new("(susssasa{sv}i)", kApplicationName, 0u, kApplicationId,
                    summary, body, nullptr, &hints, kExpireTimeoutMs),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
      &error);
  (void)g_dbus_connection_close_sync(connection, nullptr, nullptr);
  g_object_unref(connection);
  if (reply == nullptr) {
    return Fail("notification server", error);
  }
  g_variant_unref(reply);
  return true;
}

}  // namespace

}  // namespace runtime
}  // namespace mocktail
