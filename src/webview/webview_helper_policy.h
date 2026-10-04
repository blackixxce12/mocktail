#ifndef MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
#define MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_

#include <jsc/jsc.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace mocktail {
namespace webview {

inline constexpr std::size_t kMaximumHybridCommandBytes = 64 * 1024;
inline constexpr char kExecuteRobloxHandler[] = "executeRoblox";
inline constexpr char kRobloxWkHybridHandler[] = "RobloxWKHybrid";
inline constexpr char kCompatibilityHandler[] = "mocktailRobloxBridge";
// Desktop web checkout. It is the only URL ever handed to the host browser for
// a purchase; the page's own purchase URL carries a payment session and is
// never forwarded.
inline constexpr char kWebRobuxPurchaseUrl[] =
    "https://www.roblox.com/upgrades/robux";
// WebKit lists host-only cookies only for their own host, so clearing the
// Roblox session must enumerate both the www and the apex origin.
inline constexpr const char* kRobloxCookieOrigins[] = {
    "https://www.roblox.com/", "https://roblox.com/"};

enum class CaptchaEventType {
  kShown,
  kSuccess,
};

struct CaptchaEvent {
  CaptchaEventType type = CaptchaEventType::kShown;
  std::string callback_id;
};

struct UriPolicyResult {
  bool allowed = false;
  bool privileged_bridge_allowed = false;
  std::string scheme = "invalid";
  std::string host = "none";
};

// An explicit WebKit sandbox setting takes precedence over host defaults.
bool ShouldDisableWebKitSandbox(std::string_view kernel_version,
                                const char* sandbox_override);
// Match WebKit's WEBKIT_DISABLE_COMPOSITING_MODE override semantics.
bool ShouldDisableWebViewHardwareAcceleration(
    bool wayland_display, const char* compositing_override);
const char* AndroidBridgeSource();
std::string BuildRobloxAndroidUserAgent();
bool IsBrowserLoginUrl(std::string_view url);
bool IsRobloxSecurityCookie(const char* name, const char* domain);
bool IsEssentialWebResource(const char* uri);
// The Robux page served to the Android app ("GooglePlayStore" user agent)
// starts a purchase by navigating to /mobile-app-upgrades/buy?id=<sku>&...;
// the APK's RobloxWebFragment.shouldOverrideUrlLoading intercepts that URL and
// opens Google Play Billing. Mocktail has no Play Billing, so the helper must
// intercept it too instead of letting it reach the server.
bool IsAndroidStorePurchaseNavigation(const char* uri);
enum class StorePurchaseNavigation {
  kNone,     // Not a store purchase: decide the navigation as usual.
  kHandOff,  // A Roblox page started a purchase: open the web checkout.
  kIgnore,   // Any other page: drop the navigation and launch nothing.
};
// |current_uri| is the web view's main-frame URI (for a new-window action, the
// opener's). It cannot tell a Roblox page from a third-party iframe inside it,
// so this only keeps non-Roblox top-level pages from opening browser tabs.
StorePurchaseNavigation ClassifyStorePurchaseNavigation(const char* current_uri,
                                                        const char* target_uri);
std::string BoundedLogToken(const char* value, std::string_view fallback);
UriPolicyResult EvaluateNavigationUri(const char* uri);
const char* CaptchaEventName(CaptchaEventType type);
bool ExtractExecuteRobloxCommand(JSCValue* value, std::string* command);
bool ExtractRobloxWkHybridCommand(JSCValue* value, std::string* command);
bool ParseCaptchaEvent(std::string_view command, CaptchaEvent* event);
std::string BuildCallbackScript(std::string_view callback_id);

}  // namespace webview
}  // namespace mocktail

#endif  // MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
