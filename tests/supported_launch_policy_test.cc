#include "runtime/graphics_launch_policy.h"
#include "runtime/supported_launch_policy.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

int RunPolicyProbe(bool interactive, bool explicit_override) {
  for (const char* name :
       {"MOCKTAIL_SKIP_LIBROBLOX_CTORS", "MOCKTAIL_INIT_CLIENT_SETTINGS",
        "MOCKTAIL_GRAPHICS_BACKEND", "MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS",
        "MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP"}) {
    if (unsetenv(name) != 0) {
      return 10;
    }
  }
  if (explicit_override) {
    if (setenv("MOCKTAIL_GRAPHICS_BACKEND", "custom-backend", 1) != 0 ||
        setenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "0", 1) != 0) {
      return 11;
    }
  }
  std::string error;
  if (!ApplySupportedLaunchPolicy(interactive, &error)) {
    return 12;
  }
  if (!interactive) {
    return getenv("MOCKTAIL_INIT_CLIENT_SETTINGS") == nullptr &&
                   getenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP") == nullptr
               ? 0
               : 13;
  }
  const char* skip_constructors = getenv("MOCKTAIL_SKIP_LIBROBLOX_CTORS");
  const char* initialize_settings = getenv("MOCKTAIL_INIT_CLIENT_SETTINGS");
  const char* auto_exit = getenv("MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS");
  const char* allow_guest = getenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP");
  if (skip_constructors == nullptr || initialize_settings == nullptr ||
      auto_exit == nullptr || allow_guest == nullptr ||
      std::string(skip_constructors) != "0" ||
      std::string(initialize_settings) != "1" ||
      std::string(auto_exit) != "0" ||
      std::string(allow_guest) != (explicit_override ? "0" : "1")) {
    return 14;
  }
  const char* graphics = getenv("MOCKTAIL_GRAPHICS_BACKEND");
  if (explicit_override) {
    return graphics != nullptr && std::string(graphics) == "custom-backend"
               ? 0
               : 15;
  }
  return graphics == nullptr ? 0 : 16;
}

void ExpectPolicyProbe(bool interactive, bool explicit_override) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunPolicyProbe(interactive, explicit_override));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SupportedLaunchPolicyTest, PublishesInteractiveDefaults) {
  ExpectPolicyProbe(true, false);
}

TEST(SupportedLaunchPolicyTest, PreservesExplicitOverrides) {
  ExpectPolicyProbe(true, true);
}

TEST(SupportedLaunchPolicyTest, LeavesResearchModesUnchanged) {
  ExpectPolicyProbe(false, false);
}

void ExpectPackagedManifestProbe(bool candidate_override) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    constexpr const char* packaged = "/relocated AppDir/share/mocktail/default.json";
    constexpr const char* candidate = "/private-canary/candidate.json";
    if (setenv("MOCKTAIL_PACKAGED_COMPATIBILITY_MANIFEST", packaged, 1) != 0 ||
        unsetenv("MOCKTAIL_COMPATIBILITY_MANIFEST") != 0) {
      std::_Exit(10);
    }
    if (candidate_override &&
        setenv("MOCKTAIL_COMPATIBILITY_MANIFEST", candidate, 1) != 0) {
      std::_Exit(11);
    }
    std::string error;
    if (!ApplySupportedLaunchPolicy(false, &error)) std::_Exit(12);
    const char* manifest = getenv("MOCKTAIL_COMPATIBILITY_MANIFEST");
    std::_Exit(manifest != nullptr &&
                       std::string(manifest) == (candidate_override ? candidate : packaged)
                   ? 0 : 13);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SupportedLaunchPolicyTest, UsesRelocatedPackagedManifestByDefault) {
  ExpectPackagedManifestProbe(false);
}

TEST(SupportedLaunchPolicyTest, PreservesCandidateManifestOverPackagedDefault) {
  ExpectPackagedManifestProbe(true);
}

int RunGraphicsPolicyProbe(const char* backend) {
  for (const char* name : {
           "MOCKTAIL_GRAPHICS_BACKEND",
           "MOCKTAIL_NVIDIA_SHADER_MT",
           "MOCKTAIL_PRELOAD_VULKAN_SHIM",
           "MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK",
           "MOCKTAIL_SOFTWARE_WINDOW_FALLBACK",
           "MOCKTAIL_REQUIRE_REAL_GRAPHICS",
           "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
           "ANV_SYS_MEM_LIMIT",
           "MESA_VK_ENABLE_SUBMIT_THREAD",
       }) {
    if (unsetenv(name) != 0) return 20;
  }
  if (backend != nullptr &&
      setenv("MOCKTAIL_GRAPHICS_BACKEND", backend, 1) != 0) {
    return 21;
  }
  const ProcessEnvironment environment;
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);
  std::string error;
  if (!ApplyGraphicsLaunchPolicy(config, &error)) return 22;

  const bool open_gl = backend != nullptr && std::string(backend) == "opengl";
  const char* resolved = getenv("MOCKTAIL_GRAPHICS_BACKEND");
  const char* preload = getenv("MOCKTAIL_PRELOAD_VULKAN_SHIM");
  const char* nvidia_shader_mt = getenv("MOCKTAIL_NVIDIA_SHADER_MT");
  if (resolved == nullptr || preload == nullptr ||
      std::string(resolved) != (open_gl ? "opengl" : "direct-vulkan") ||
      std::string(preload) != (open_gl ? "0" : "1") ||
      nvidia_shader_mt == nullptr || std::string(nvidia_shader_mt) != "1") {
    return 23;
  }
  if (open_gl) {
    const char* disable_angle =
        getenv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK");
    const char* software = getenv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK");
    return disable_angle != nullptr && software != nullptr &&
                   std::string(disable_angle) == "1" &&
                   std::string(software) == "0" &&
                   getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON") == nullptr &&
                   getenv("ANV_SYS_MEM_LIMIT") == nullptr &&
                   getenv("MESA_VK_ENABLE_SUBMIT_THREAD") == nullptr
               ? 0
               : 24;
  }
  const char* overrides = getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON");
  const char* anv_memory_limit = getenv("ANV_SYS_MEM_LIMIT");
  const char* submit_thread = getenv("MESA_VK_ENABLE_SUBMIT_THREAD");
  std::string expected_anv_limit = "50";
  {
    std::ifstream input("/proc/meminfo");
    std::string key;
    unsigned long kb = 0;
    std::string unit;
    while (input >> key >> kb >> unit) {
      if (key == "MemTotal:") {
        expected_anv_limit = kb > 4UL * 1024UL * 1024UL ? "75" : "50";
        break;
      }
    }
  }
  // Mocktail no longer forces any client setting for Vulkan: Roblox's
  // shader pack loader runs on several threads on every vendor.
  return overrides == nullptr && anv_memory_limit != nullptr &&
                 submit_thread != nullptr &&
                 std::string(anv_memory_limit) == expected_anv_limit &&
                 std::string(submit_thread) == "1"
             ? 0
             : 25;
}

void ExpectGraphicsPolicyProbe(const char* backend) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunGraphicsPolicyProbe(backend));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(GraphicsLaunchPolicyTest, DefaultsToDirectVulkanAfterConfigResolution) {
  ExpectGraphicsPolicyProbe(nullptr);
}

TEST(GraphicsLaunchPolicyTest, MakesOpenGlStrictAndVulkanIndependent) {
  ExpectGraphicsPolicyProbe("opengl");
}

// Mesa's WSI applies MESA_VK_WSI_PRESENT_MODE over the present mode the
// adapter chose, so it must follow present_mode_policy.cc: Vertical sync On
// wins over the unlimited frame rate, Off and unlimited are unthrottled.
int RunWsiPresentModeProbe(const char* vsync, const char* frame_rate,
                           const char* expected) {
  for (const char* name : {"MOCKTAIL_GRAPHICS_BACKEND", "MOCKTAIL_VSYNC",
                           "MOCKTAIL_FRAME_RATE_LIMIT",
                           "MESA_VK_WSI_PRESENT_MODE"}) {
    if (unsetenv(name) != 0) return 30;
  }
  if (setenv("MOCKTAIL_VSYNC", vsync, 1) != 0 ||
      setenv("MOCKTAIL_FRAME_RATE_LIMIT", frame_rate, 1) != 0) {
    return 31;
  }
  const ProcessEnvironment environment;
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);
  std::string error;
  if (!ApplyGraphicsLaunchPolicy(config, &error)) return 32;
  const char* mode = getenv("MESA_VK_WSI_PRESENT_MODE");
  return mode != nullptr && std::string(mode) == expected ? 0 : 33;
}

void ExpectWsiPresentMode(const char* vsync, const char* frame_rate,
                          const char* expected) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunWsiPresentModeProbe(vsync, frame_rate, expected));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "vsync=" << vsync << " frame_rate=" << frame_rate;
}

TEST(GraphicsLaunchPolicyTest, MesaPresentModeFollowsTheVsyncPolicy) {
  ExpectWsiPresentMode("auto", "-1", "mailbox");
  ExpectWsiPresentMode("auto", "unlimited", "immediate");
  ExpectWsiPresentMode("off", "-1", "immediate");
  ExpectWsiPresentMode("on", "unlimited", "mailbox");
  ExpectWsiPresentMode("on", "144", "mailbox");
}

constexpr char kNvidiaDeny[] =
    R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

// Applies the policy with MOCKTAIL_NVIDIA_SHADER_MT and the user's
// MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON set as given (nullptr leaves a
// variable unset), then checks the published overrides against `expected`
// (nullptr: still unset).
int RunNvidiaShaderPolicyProbe(const char* backend, const char* switch_value,
                               const char* user_overrides,
                               const char* expected) {
  for (const char* name : {
           "MOCKTAIL_GRAPHICS_BACKEND",
           "MOCKTAIL_NVIDIA_SHADER_MT",
           "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
       }) {
    if (unsetenv(name) != 0) return 20;
  }
  if (setenv("MOCKTAIL_GRAPHICS_BACKEND", backend, 1) != 0 ||
      (switch_value != nullptr &&
       setenv("MOCKTAIL_NVIDIA_SHADER_MT", switch_value, 1) != 0) ||
      (user_overrides != nullptr &&
       setenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON", user_overrides, 1) !=
           0)) {
    return 21;
  }
  const RuntimeConfig config =
      RuntimeConfig::FromEnvironment(ProcessEnvironment());
  std::string error;
  if (!ApplyGraphicsLaunchPolicy(config, &error)) return 22;
  const char* published = getenv("MOCKTAIL_NVIDIA_SHADER_MT");
  const char* resolved = config.engine().nvidia_shader_mt ? "1" : "0";
  if (published == nullptr || std::string(published) != resolved) {
    return 23;
  }
  const char* overrides = getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON");
  if (expected == nullptr) {
    return overrides == nullptr ? 0 : 24;
  }
  return overrides != nullptr && std::string(overrides) == expected ? 0 : 25;
}

void ExpectNvidiaShaderPolicy(const char* backend, const char* switch_value,
                              const char* user_overrides,
                              const char* expected) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunNvidiaShaderPolicyProbe(backend, switch_value, user_overrides,
                                          expected));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << backend << " MOCKTAIL_NVIDIA_SHADER_MT="
      << (switch_value != nullptr ? switch_value : "(unset)") << " overrides="
      << (user_overrides != nullptr ? user_overrides : "(unset)");
}

TEST(GraphicsLaunchPolicyTest, LeavesShaderLoadingThreadsToRobloxByDefault) {
  ExpectNvidiaShaderPolicy("direct-vulkan", nullptr, nullptr, nullptr);
  ExpectNvidiaShaderPolicy("direct-vulkan", "1", nullptr, nullptr);
  ExpectNvidiaShaderPolicy("direct-vulkan", "on", R"({"FFlagExample":"True"})",
                           R"({"FFlagExample":"True"})");
}

TEST(GraphicsLaunchPolicyTest, SwitchedOffDeniesShaderLoadingThreadsOnNvidia) {
  ExpectNvidiaShaderPolicy("direct-vulkan", "0", nullptr, kNvidiaDeny);
  ExpectNvidiaShaderPolicy("direct-vulkan", "off", "", kNvidiaDeny);
  // The user's own client settings stay, and the deny joins them.
  ExpectNvidiaShaderPolicy("direct-vulkan", "false",
                           R"({"FFlagExample":"True"})",
                           R"({"FFlagExample":"True",)"
                           R"("FStringGraphicsVulkanShaderMTDenyPattern":)"
                           R"("4318:.*"})");
  // A pattern the user set for the flag is theirs to keep.
  ExpectNvidiaShaderPolicy(
      "direct-vulkan", "off",
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:5.*"})",
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:5.*"})");
  // The flag only steers Roblox's Vulkan device.
  ExpectNvidiaShaderPolicy("opengl", "off", nullptr, nullptr);
}

TEST(GraphicsLaunchPolicyTest, MergesTheNvidiaShaderDenyIntoClientSettings) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(
      MergeNvidiaShaderLoadingClientSettingsOverrides(true, "", &merged));
  EXPECT_EQ(merged, "{}");
  ASSERT_TRUE(MergeNvidiaShaderLoadingClientSettingsOverrides(
      true, R"({"FFlagExample":"True"})", &merged));
  EXPECT_EQ(merged, R"({"FFlagExample":"True"})");
  ASSERT_TRUE(
      MergeNvidiaShaderLoadingClientSettingsOverrides(false, "{}", &merged));
  EXPECT_EQ(merged, kNvidiaDeny);
  ASSERT_TRUE(MergeNvidiaShaderLoadingClientSettingsOverrides(
      false, R"({"FStringGraphicsVulkanShaderMTDenyPattern":""})", &merged));
  EXPECT_EQ(merged, R"({"FStringGraphicsVulkanShaderMTDenyPattern":""})");

  for (const char* invalid : {"[]", "\"4318:.*\"", "{", "not json"}) {
    EXPECT_FALSE(MergeNvidiaShaderLoadingClientSettingsOverrides(
        false, invalid, &merged, &error))
        << invalid;
    EXPECT_FALSE(error.empty()) << invalid;
  }
  EXPECT_FALSE(
      MergeNvidiaShaderLoadingClientSettingsOverrides(false, "{}", nullptr));
}

TEST(GraphicsLaunchPolicyTest, RefusesAnInvalidNvidiaShaderSwitch) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    if (setenv("MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan", 1) != 0 ||
        setenv("MOCKTAIL_NVIDIA_SHADER_MT", "auto", 1) != 0 ||
        unsetenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON") != 0) {
      std::_Exit(20);
    }
    const RuntimeConfig invalid =
        RuntimeConfig::FromEnvironment(ProcessEnvironment());
    std::string error;
    std::_Exit(!invalid.engine().nvidia_shader_mt_valid &&
                       !ApplyGraphicsLaunchPolicy(invalid, {}, &error) &&
                       error.find("NVIDIA shader loading") !=
                           std::string::npos &&
                       getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON") ==
                           nullptr
                   ? 0
                   : 21);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
