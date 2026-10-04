#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

namespace jnivm {
namespace {

struct StoreCall {
  jlong player_id = 0;
  std::string product_id;
  bool expects_in_game_result = false;
};

struct StoreProbe {
  std::vector<StoreCall> calls;
};

void RecordStorePurchase(void* context, JNIEnv* env, jlong player_id,
                         jstring product_id, bool expects_in_game_result) {
  auto* probe = static_cast<StoreProbe*>(context);
  StoreCall call;
  call.player_id = player_id;
  call.expects_in_game_result = expects_in_game_result;
  if (product_id != nullptr) {
    const char* chars = env->GetStringUTFChars(product_id, nullptr);
    call.product_id = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(product_id, chars);
  }
  probe->calls.push_back(std::move(call));
}

void CallStaticVoidMethodVForTest(JNIEnv* env, jclass receiver,
                                  jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  env->functions->CallStaticVoidMethodV(env, receiver, method_id, args);
  va_end(args);
}

jboolean CallStaticBooleanMethodVForTest(JNIEnv* env, jclass receiver,
                                         jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  const jboolean result =
      env->functions->CallStaticBooleanMethodV(env, receiver, method_id, args);
  va_end(args);
  return result;
}

// Synthetic product ids only; nothing here reaches a store or the network.
TEST(JniVmNativeStoreTest, RoutesPlayBillingEntryPointsToTheStoreConsumer) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  ASSERT_NE(env, nullptr);
  auto probe = std::make_shared<StoreProbe>();
  vm.SetRobloxNativeStoreCallbacks(
      probe, RobloxNativeStoreCallbacks{&RecordStorePurchase});

  jclass gl = env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  jclass iap = env->FindClass("com/roblox/client/purchase/IAPPurchaseManager");
  ASSERT_NE(gl, nullptr);
  ASSERT_NE(iap, nullptr);

  jmethodID prompt = env->GetStaticMethodID(gl, "promptNativePurchase",
                                            "(JLjava/lang/String;)V");
  env->CallStaticVoidMethod(gl, prompt, static_cast<jlong>(42),
                            env->NewStringUTF("test.robux.small"));

  jmethodID with_session = env->GetStaticMethodID(
      gl, "promptNativePurchaseWithPaymentSessionId",
      "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  CallStaticVoidMethodVForTest(env, gl, with_session, static_cast<jlong>(43),
                               env->NewStringUTF("test.robux.medium"),
                               env->NewStringUTF("session"),
                               env->NewStringUTF("payload"));

  jmethodID with_payload = env->GetStaticMethodID(
      gl, "promptNativePurchaseWithPayload",
      "(JLjava/lang/String;Ljava/lang/String;)V");
  jvalue payload_args[3] = {};
  payload_args[0].j = 44;
  payload_args[1].l = env->NewStringUTF("test.robux.large");
  payload_args[2].l = env->NewStringUTF("{}");
  env->CallStaticVoidMethodA(gl, with_payload, payload_args);

  jmethodID invoke_store = env->GetStaticMethodID(
      iap, "invokeStore", "(Ljava/lang/String;Ljava/lang/String;)Z");
  EXPECT_EQ(CallStaticBooleanMethodVForTest(env, iap, invoke_store,
                                            env->NewStringUTF("test.sku"),
                                            env->NewStringUTF("extra")),
            JNI_FALSE);

  jmethodID invoke_store_v2 = env->GetStaticMethodID(
      iap, "invokeStoreV2", "(Ljava/lang/String;JLjava/lang/String;)V");
  CallStaticVoidMethodVForTest(env, iap, invoke_store_v2,
                               env->NewStringUTF("test.sku.v2"),
                               static_cast<jlong>(45),
                               env->NewStringUTF("extra"));

  ASSERT_EQ(probe->calls.size(), 5u);
  EXPECT_EQ(probe->calls[0].player_id, 42);
  EXPECT_EQ(probe->calls[0].product_id, "test.robux.small");
  EXPECT_TRUE(probe->calls[0].expects_in_game_result);
  EXPECT_EQ(probe->calls[1].player_id, 43);
  EXPECT_EQ(probe->calls[1].product_id, "test.robux.medium");
  EXPECT_TRUE(probe->calls[1].expects_in_game_result);
  EXPECT_EQ(probe->calls[2].player_id, 44);
  EXPECT_EQ(probe->calls[2].product_id, "test.robux.large");
  EXPECT_TRUE(probe->calls[2].expects_in_game_result);
  EXPECT_EQ(probe->calls[3].product_id, "test.sku");
  EXPECT_FALSE(probe->calls[3].expects_in_game_result);
  EXPECT_EQ(probe->calls[4].player_id, 45);
  EXPECT_EQ(probe->calls[4].product_id, "test.sku.v2");
  EXPECT_FALSE(probe->calls[4].expects_in_game_result);
}

TEST(JniVmNativeStoreTest, IgnoresLookalikesAndStopsAfterClear) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  auto probe = std::make_shared<StoreProbe>();
  vm.SetRobloxNativeStoreCallbacks(
      probe, RobloxNativeStoreCallbacks{&RecordStorePurchase});

  jclass gl = env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  jclass other = env->FindClass("example/NativeGLJavaInterface");
  jmethodID prompt = env->GetStaticMethodID(gl, "promptNativePurchase",
                                            "(JLjava/lang/String;)V");
  jmethodID wrong_signature =
      env->GetStaticMethodID(gl, "promptNativePurchase", "(I)V");
  env->CallStaticVoidMethod(other, prompt, static_cast<jlong>(1),
                            env->NewStringUTF("x"));
  env->CallStaticVoidMethod(gl, wrong_signature, 7);
  EXPECT_TRUE(probe->calls.empty());

  vm.ClearRobloxNativeStoreCallbacks();
  env->CallStaticVoidMethod(gl, prompt, static_cast<jlong>(1),
                            env->NewStringUTF("x"));
  EXPECT_TRUE(probe->calls.empty());
}

}  // namespace
}  // namespace jnivm
