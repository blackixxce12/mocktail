#include "runtime/roblox_account_protocol_bridge.h"

#include <array>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

// The APK's yl/l enum value for "no integrity token provider".
constexpr char kTokenProviderUninitialized[] = "TOKEN_PROVIDER_UNINITIALIZED";

constexpr std::array<const char*, 2> kMethods = {
    kRobloxDeviceIntegrityAvailableMethod,
    kRobloxGetIntegrityTokenMethod,
};

Status Unavailable(const char* message) {
  return Status::Error(StatusCode::kUnavailable, message);
}

Status CheckJni(JNIEnv* env) {
  if (!env->ExceptionCheck()) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return Unavailable("Account protocol JNI operation failed");
}

}  // namespace

RobloxAccountProtocolAnswer AnswerRobloxAccountProtocolRequest(
    RobloxAccountProtocolMethod method) {
  switch (method) {
    case RobloxAccountProtocolMethod::kDeviceIntegrityAvailable:
      return {nlohmann::json{{"support", false}}.dump(), "unsupported"};
    case RobloxAccountProtocolMethod::kGetIntegrityToken:
      return {nlohmann::json{{"token", ""},
                             {"result", kTokenProviderUninitialized}}
                  .dump(),
              "no_provider"};
  }
  return {"{}", "unknown_method"};
}

struct RobloxAccountProtocolBridge::State {
  // The handler context holds only the method. The answer is fixed, so a call
  // that races with Shutdown never touches bridge state.
  struct Endpoint {
    RobloxAccountProtocolMethod method;
  };

  struct Registration {
    jstring method = nullptr;
    jobject handler = nullptr;
    bool registered = false;
  };

  jobject bus = nullptr;
  jstring protocol = nullptr;
  std::array<Registration, kMethods.size()> registrations{};

  static std::string Run(void* context, JNIEnv*, jstring) {
    const auto* endpoint = static_cast<const Endpoint*>(context);
    if (endpoint == nullptr) {
      return "{}";
    }
    RobloxAccountProtocolAnswer answer =
        AnswerRobloxAccountProtocolRequest(endpoint->method);
    // The request carries Roblox's requestHash; it is never logged.
    std::fprintf(stderr,
                 "  [messagebus] %s method=%s result=%s transport=sync\n",
                 kRobloxAccountProtocolName,
                 kMethods[static_cast<std::size_t>(endpoint->method)],
                 answer.outcome);
    return std::move(answer.json);
  }
};

RobloxAccountProtocolBridge::RobloxAccountProtocolBridge(
    JniEnvironmentProvider environment, RobloxAccountProtocolSymbols symbols,
    RobloxAccountProtocolObjects objects)
    : environment_(environment), symbols_(symbols), objects_(objects) {}

RobloxAccountProtocolBridge::~RobloxAccountProtocolBridge() {
  (void)Shutdown();
}

Status RobloxAccountProtocolBridge::Initialize() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Account protocol bridge is already initialized");
  }
  if (!environment_.valid() || !symbols_.complete() || !objects_.complete()) {
    return Unavailable("Account protocol bridge prerequisites are incomplete");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  state_ = std::make_shared<State>();
  auto fail = [this](Status failure) {
    (void)ShutdownLocked();
    return failure;
  };

  state_->bus = env->NewGlobalRef(objects_.message_bus);
  jstring protocol = env->NewStringUTF(kRobloxAccountProtocolName);
  if (protocol != nullptr) {
    state_->protocol = static_cast<jstring>(env->NewGlobalRef(protocol));
    env->DeleteLocalRef(protocol);
  }
  if (state_->bus == nullptr || state_->protocol == nullptr ||
      !CheckJni(env).ok()) {
    return fail(Unavailable("could not retain Account protocol roots"));
  }

  for (std::size_t index = 0; index < kMethods.size(); ++index) {
    State::Registration& entry = state_->registrations[index];
    jstring method = env->NewStringUTF(kMethods[index]);
    if (method != nullptr) {
      entry.method = static_cast<jstring>(env->NewGlobalRef(method));
      env->DeleteLocalRef(method);
    }
    auto endpoint = std::make_shared<State::Endpoint>(
        State::Endpoint{static_cast<RobloxAccountProtocolMethod>(index)});
    jobject handler = objects_.create_request_handler(
        objects_.context, std::move(endpoint), &State::Run);
    if (handler != nullptr) {
      entry.handler = env->NewGlobalRef(handler);
      if (entry.handler == nullptr) {
        objects_.clear_request_handler(objects_.context, handler);
      }
      env->DeleteLocalRef(handler);
    }
    if (entry.method == nullptr || entry.handler == nullptr ||
        !CheckJni(env).ok()) {
      return fail(Unavailable("could not create Account protocol handler"));
    }
    symbols_.set_request_handler_raw(env, state_->bus, state_->protocol,
                                     entry.method, entry.handler);
    status = CheckJni(env);
    if (!status.ok()) {
      return fail(status);
    }
    entry.registered = true;
  }
  std::fprintf(stderr,
               "  [messagebus] %s ready sync=%zu (no device integrity "
               "provider; answering unavailable)\n",
               kRobloxAccountProtocolName, kMethods.size());
  return Status::Ok();
}

Status RobloxAccountProtocolBridge::Shutdown() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  return ShutdownLocked();
}

Status RobloxAccountProtocolBridge::ShutdownLocked() {
  if (!state_) {
    return Status::Ok();
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;  // Keep the roots for a later retry.
  }
  const Status pending = CheckJni(env);
  if (!pending.ok()) {
    status = pending;
  }
  for (State::Registration& entry : state_->registrations) {
    if (entry.registered) {
      symbols_.clear_request_handler(env, state_->bus, state_->protocol,
                                     entry.method);
      const Status cleared = CheckJni(env);
      if (!cleared.ok()) {
        status = cleared;
      }
      entry.registered = false;
    }
    if (entry.handler != nullptr) {
      objects_.clear_request_handler(objects_.context, entry.handler);
      env->DeleteGlobalRef(entry.handler);
    }
    if (entry.method != nullptr) {
      env->DeleteGlobalRef(entry.method);
    }
    entry = {};
  }
  if (state_->protocol != nullptr) {
    env->DeleteGlobalRef(state_->protocol);
  }
  if (state_->bus != nullptr) {
    env->DeleteGlobalRef(state_->bus);
  }
  state_.reset();
  return status;
}

}  // namespace runtime
}  // namespace mocktail
