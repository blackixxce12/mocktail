#ifndef MOCKTAIL_RUNTIME_DESKTOP_NOTIFICATION_H_
#define MOCKTAIL_RUNTIME_DESKTOP_NOTIFICATION_H_

namespace mocktail {
namespace runtime {

// Shows a short notification from Mocktail through the desktop's
// org.freedesktop.Notifications service on the session bus. It waits for one
// bus round trip, so callers run it off the game loop. Without a session bus
// or a notification server it returns false; only the first failure is
// logged.
bool ShowDesktopNotification(const char* summary, const char* body);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_DESKTOP_NOTIFICATION_H_
