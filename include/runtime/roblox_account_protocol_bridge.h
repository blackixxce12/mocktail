#ifndef MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_PROTOCOL_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_PROTOCOL_BRIDGE_H_

#include <jni.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {

// Wire names used by libroblox's AccountService and by the APK's Java
// AccountProtocol handler. libroblox builds them from these literal strings
// (JNIAccountProtocol.getProtocolName, getDeviceIntegrityAvailableMethodName,
// getGetIntegrityTokenMethodName, getSupportKey, getTokenKey, getResultKey).
inline constexpr char kRobloxAccountProtocolName[] = "Account";
inline constexpr char kRobloxDeviceIntegrityAvailableMethod[] =
    "deviceIntegrityAvailable";
inline constexpr char kRobloxGetIntegrityTokenMethod[] = "getIntegrityToken";

enum class RobloxAccountProtocolMethod : std::size_t {
  kDeviceIntegrityAvailable = 0,
  kGetIntegrityToken = 1,
};

struct RobloxAccountProtocolAnswer {
  std::string json;
  const char* outcome = "";
};

// Mocktail has no Google Play Integrity provider, and it never makes up a
// token or a verdict. It reports that honestly:
// - deviceIntegrityAvailable answers {"support":false}. The APK (2.736)
//   always answers true here and lets getIntegrityToken report the problem.
// - getIntegrityToken answers an empty token with TOKEN_PROVIDER_UNINITIALIZED,
//   which is what the APK itself answers when its provider never initialized.
// The request (requestHash, timeoutMillis) is not read, logged or echoed.
//
// What LuaApp's DeviceIntegrityTokenChallenge does with the answer:
// - With FFlagEnableNewDeviceIntegrityFailureHandling on (the server value;
//   the bundle default is off), it logs ChallengeInvalidated and completes the
//   challenge with an empty redemptionToken at once. An Android device without
//   an integrity provider sends the same empty token.
// - With the flag off, it calls onChallengeFailed.
// Either way Roblox's server decides whether the login may continue. It may
// refuse it: an empty token is not a passed integrity check.
RobloxAccountProtocolAnswer AnswerRobloxAccountProtocolRequest(
    RobloxAccountProtocolMethod method);

using SetRobloxRequestHandlerRawFn = void (*)(JNIEnv*, jobject, jstring,
                                              jstring, jobject);
using ClearRobloxRequestHandlerFn = void (*)(JNIEnv*, jobject, jstring,
                                             jstring);

struct RobloxAccountProtocolSymbols {
  // MessageBus.setRequestHandlerRaw / clearRequestHandler. The APK registers
  // its AccountProtocol methods with the synchronous RequestHandlerRaw API.
  SetRobloxRequestHandlerRawFn set_request_handler_raw = nullptr;
  ClearRobloxRequestHandlerFn clear_request_handler = nullptr;

  bool complete() const {
    return set_request_handler_raw != nullptr &&
           clear_request_handler != nullptr;
  }
};

struct RobloxAccountProtocolObjects {
  jobject message_bus = nullptr;
  void* context = nullptr;
  jobject (*create_request_handler)(void* context,
                                    std::shared_ptr<void> callback_context,
                                    std::string (*run)(void*, JNIEnv*,
                                                       jstring)) = nullptr;
  void (*clear_request_handler)(void* context, jobject handler) = nullptr;

  bool complete() const {
    return message_bus != nullptr && context != nullptr &&
           create_request_handler != nullptr &&
           clear_request_handler != nullptr;
  }
};

class RobloxAccountProtocolBridge final {
 public:
  RobloxAccountProtocolBridge(JniEnvironmentProvider environment,
                              RobloxAccountProtocolSymbols symbols,
                              RobloxAccountProtocolObjects objects);
  ~RobloxAccountProtocolBridge();

  RobloxAccountProtocolBridge(const RobloxAccountProtocolBridge&) = delete;
  RobloxAccountProtocolBridge& operator=(const RobloxAccountProtocolBridge&) =
      delete;

  Status Initialize();
  Status Shutdown();

 private:
  struct State;
  Status ShutdownLocked();

  const JniEnvironmentProvider environment_;
  const RobloxAccountProtocolSymbols symbols_;
  const RobloxAccountProtocolObjects objects_;
  std::mutex lifecycle_mutex_;
  std::shared_ptr<State> state_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_PROTOCOL_BRIDGE_H_
