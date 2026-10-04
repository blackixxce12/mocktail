#include "runtime/roblox_account_protocol_bridge.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

// A request shaped like LuaApp's: the hash is a placeholder, not a real one.
constexpr char kTokenRequest[] =
    R"({"requestHash":"placeholder-request-hash","timeoutMillis":450})";

std::string Copy(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string copy = chars != nullptr ? chars : "";
  if (chars != nullptr) {
    env->ReleaseStringUTFChars(value, chars);
  }
  return copy;
}

struct Probe {
  jnivm::VM* vm = nullptr;
  std::map<std::string, jobject> handlers;
  std::vector<std::string> registered_protocols;
  std::vector<std::string> cleared;
  // Shutdown steps in order: "unregister:<method>" and "release-handler".
  std::vector<std::string> events;
  int handlers_created = 0;
  int handlers_cleared = 0;
  int fail_handler_at = 0;
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

jobject CreateHandler(void* context, std::shared_ptr<void> target,
                      std::string (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  if (++probe->handlers_created == probe->fail_handler_at) {
    return nullptr;
  }
  return probe->vm->CreateMessageBusRequestHandler(
      std::move(target), jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearHandlerObject(void* context, jobject handler) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_cleared;
  probe->events.emplace_back("release-handler");
  probe->vm->ClearMessageBusRequestHandler(handler);
}

void SetRequestHandlerRaw(JNIEnv* env, jobject, jstring protocol,
                          jstring method, jobject handler) {
  g_probe->registered_protocols.push_back(Copy(env, protocol));
  g_probe->handlers[Copy(env, method)] = handler;
}

void ClearRequestHandler(JNIEnv* env, jobject, jstring protocol,
                         jstring method) {
  EXPECT_EQ(Copy(env, protocol), "Account");
  g_probe->cleared.push_back(Copy(env, method));
  g_probe->events.push_back("unregister:" + g_probe->cleared.back());
}

class RobloxAccountProtocolBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    probe.vm = &vm;
    g_probe = &probe;
    env = vm.GetJNIEnv();
    jclass cls =
        env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
    bus = env->AllocObject(cls);
    env->DeleteLocalRef(cls);
  }

  void TearDown() override {
    bridge.reset();
    env->DeleteLocalRef(bus);
    g_probe = nullptr;
  }

  std::unique_ptr<RobloxAccountProtocolBridge> Make(
      RobloxAccountProtocolSymbols symbols = {SetRequestHandlerRaw,
                                              ClearRequestHandler}) {
    return std::make_unique<RobloxAccountProtocolBridge>(
        JniEnvironmentProvider{vm.GetJavaVM(), &vm, Prepare}, symbols,
        RobloxAccountProtocolObjects{bus, &probe, CreateHandler,
                                     ClearHandlerObject});
  }

  // Calls RequestHandlerRaw.run(String) the way libroblox does through JNI.
  std::string Run(const char* method, const char* payload) {
    jobject handler = probe.handlers.at(method);
    jclass cls = env->GetObjectClass(handler);
    jmethodID run =
        env->GetMethodID(cls, "run", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring message = payload != nullptr ? env->NewStringUTF(payload) : nullptr;
    auto result =
        static_cast<jstring>(env->CallObjectMethod(handler, run, message));
    std::string copy = Copy(env, result);
    if (result != nullptr) {
      env->DeleteLocalRef(result);
    }
    if (message != nullptr) {
      env->DeleteLocalRef(message);
    }
    env->DeleteLocalRef(cls);
    return copy;
  }

  jnivm::VM vm;
  Probe probe;
  JNIEnv* env = nullptr;
  jobject bus = nullptr;
  std::unique_ptr<RobloxAccountProtocolBridge> bridge;
};

TEST(RobloxAccountProtocolAnswerTest, DeviceIntegrityIsReportedUnsupported) {
  const RobloxAccountProtocolAnswer answer = AnswerRobloxAccountProtocolRequest(
      RobloxAccountProtocolMethod::kDeviceIntegrityAvailable);
  EXPECT_EQ(Json::parse(answer.json), (Json{{"support", false}}));
  EXPECT_STREQ(answer.outcome, "unsupported");
}

TEST(RobloxAccountProtocolAnswerTest,
     TokenRequestGetsEmptyTokenWithTheApksNoProviderResult) {
  const RobloxAccountProtocolAnswer answer = AnswerRobloxAccountProtocolRequest(
      RobloxAccountProtocolMethod::kGetIntegrityToken);
  const Json body = Json::parse(answer.json);
  EXPECT_EQ(body, (Json{{"token", ""},
                        {"result", "TOKEN_PROVIDER_UNINITIALIZED"}}));
  // LuaApp treats nil and "" as "no token"; never anything else.
  EXPECT_TRUE(body.at("token").get<std::string>().empty());
  EXPECT_STREQ(answer.outcome, "no_provider");
}

TEST_F(RobloxAccountProtocolBridgeTest,
       RegistersBothSyncHandlersOnTheAccountProtocol) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  ASSERT_EQ(probe.handlers.size(), 2u);
  EXPECT_EQ(probe.handlers.count("deviceIntegrityAvailable"), 1u);
  EXPECT_EQ(probe.handlers.count("getIntegrityToken"), 1u);
  EXPECT_EQ(probe.registered_protocols,
            (std::vector<std::string>{"Account", "Account"}));
}

TEST_F(RobloxAccountProtocolBridgeTest,
       AvailabilityAnswersUnsupportedWhateverThePayload) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  for (const char* payload : {"{}", "", "not json", kTokenRequest}) {
    EXPECT_EQ(Json::parse(Run("deviceIntegrityAvailable", payload)),
              (Json{{"support", false}}))
        << payload;
  }
  EXPECT_EQ(Json::parse(Run("deviceIntegrityAvailable", nullptr)),
            (Json{{"support", false}}));
}

TEST_F(RobloxAccountProtocolBridgeTest,
       TokenRequestNeverEchoesTheRequestHashOrInventsAToken) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  const std::string response = Run("getIntegrityToken", kTokenRequest);
  EXPECT_EQ(response.find("placeholder-request-hash"), std::string::npos);
  const Json body = Json::parse(response);
  EXPECT_EQ(body.at("token"), "");
  EXPECT_EQ(body.at("result"), "TOKEN_PROVIDER_UNINITIALIZED");
  EXPECT_EQ(Json::parse(Run("getIntegrityToken", nullptr)), body);
}

TEST_F(RobloxAccountProtocolBridgeTest,
       ShutdownUnregistersNativeHandlersBeforeReleasingObjects) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  jobject availability = probe.handlers.at("deviceIntegrityAvailable");
  ASSERT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.cleared, (std::vector<std::string>{"deviceIntegrityAvailable",
                                                     "getIntegrityToken"}));
  EXPECT_EQ(probe.handlers_cleared, 2);
  // Each method leaves the MessageBus before its handler object is released.
  EXPECT_EQ(probe.events,
            (std::vector<std::string>{"unregister:deviceIntegrityAvailable",
                                      "release-handler",
                                      "unregister:getIntegrityToken",
                                      "release-handler"}));
  // A stale native call after Shutdown finds no binding and gets null back.
  EXPECT_EQ(vm.DispatchMessageBusRequestHandler(availability, env, nullptr),
            nullptr);
  // Shutdown is idempotent, and the bridge can be brought up again.
  EXPECT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.cleared.size(), 2u);
  ASSERT_TRUE(bridge->Initialize().ok());
  EXPECT_EQ(Json::parse(Run("deviceIntegrityAvailable", "{}")),
            (Json{{"support", false}}));
}

TEST_F(RobloxAccountProtocolBridgeTest, LogsMethodAndOutcomeButNoRequestData) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  testing::internal::CaptureStderr();
  (void)Run("getIntegrityToken", kTokenRequest);
  (void)Run("deviceIntegrityAvailable", kTokenRequest);
  const std::string log = testing::internal::GetCapturedStderr();
  EXPECT_NE(log.find("[messagebus] Account method=getIntegrityToken "
                     "result=no_provider"),
            std::string::npos)
      << log;
  EXPECT_NE(log.find("[messagebus] Account method=deviceIntegrityAvailable "
                     "result=unsupported"),
            std::string::npos)
      << log;
  EXPECT_EQ(log.find("placeholder-request-hash"), std::string::npos);
  EXPECT_EQ(log.find("requestHash"), std::string::npos);
  EXPECT_EQ(log.find("timeoutMillis"), std::string::npos);
}

TEST_F(RobloxAccountProtocolBridgeTest, SecondInitializeIsRejected) {
  bridge = Make();
  ASSERT_TRUE(bridge->Initialize().ok());
  const Status again = bridge->Initialize();
  EXPECT_EQ(again.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(probe.handlers_created, 2);
}

TEST_F(RobloxAccountProtocolBridgeTest, IncompleteSymbolsRegisterNothing) {
  bridge = Make(RobloxAccountProtocolSymbols{SetRequestHandlerRaw, nullptr});
  EXPECT_EQ(bridge->Initialize().code(), StatusCode::kUnavailable);
  EXPECT_TRUE(probe.handlers.empty());
  EXPECT_EQ(probe.handlers_created, 0);
}

TEST_F(RobloxAccountProtocolBridgeTest,
       HandlerCreationFailureRollsBackTheFirstRegistration) {
  probe.fail_handler_at = 2;
  bridge = Make();
  EXPECT_EQ(bridge->Initialize().code(), StatusCode::kUnavailable);
  // The first method was registered, so it must be unregistered again.
  EXPECT_EQ(probe.cleared,
            (std::vector<std::string>{"deviceIntegrityAvailable"}));
  EXPECT_EQ(probe.handlers_cleared, 1);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
