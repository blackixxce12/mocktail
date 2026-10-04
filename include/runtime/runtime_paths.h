#ifndef MOCKTAIL_RUNTIME_RUNTIME_PATHS_H_
#define MOCKTAIL_RUNTIME_RUNTIME_PATHS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

struct ActivePayloadPaths {
  bool active = false;
  std::filesystem::path root;
  std::filesystem::path roblox_library;
  std::filesystem::path base_apk;
  std::filesystem::path arch_split_apk;
  std::filesystem::path assets_content;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path host_abi_profile;
  std::filesystem::path host_abi_approval_receipt;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

class RuntimePaths {
 public:
  static RuntimePaths FromEnvironment(
      const Environment& environment,
      std::filesystem::path working_directory = {});

  const std::filesystem::path& home() const { return home_; }
  const std::filesystem::path& working_directory() const {
    return working_directory_;
  }
  const std::filesystem::path& sober_data_root() const {
    return sober_data_root_;
  }
  const std::filesystem::path& sober_cache_root() const {
    return sober_cache_root_;
  }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  const std::filesystem::path& config_root() const { return config_root_; }
  const std::filesystem::path& data_root() const { return data_root_; }
  const std::filesystem::path& state_root() const { return state_root_; }
  const std::filesystem::path& auth_root() const { return auth_root_; }
  const std::filesystem::path& payloads_root() const { return payloads_root_; }
  const std::filesystem::path& downloads_root() const {
    return downloads_root_;
  }
  const std::filesystem::path& logs_root() const { return logs_root_; }
  const std::filesystem::path& android_runtime_root() const {
    return android_runtime_root_;
  }
  const std::filesystem::path& android_data_root() const {
    return android_data_root_;
  }
  const std::filesystem::path& android_cache_root() const {
    return android_cache_root_;
  }
  const std::filesystem::path& vulkan_shader_cache_file() const {
    return vulkan_shader_cache_file_;
  }
  const std::filesystem::path& config_file() const { return config_file_; }
  const std::filesystem::path& active_payload_manifest() const {
    return active_payload_manifest_;
  }
  const std::filesystem::path& cookie_file() const { return cookie_file_; }
  bool use_real_sober_paths() const { return use_real_sober_paths_; }

  std::filesystem::path DefaultAssetPath() const;
  ActivePayloadPaths ResolveActivePayload() const;
  std::filesystem::path DefaultSoberAwarePath(
      const std::filesystem::path& sober_path,
      const std::filesystem::path& fallback_path) const;

  static bool Exists(const std::filesystem::path& path);
  static bool EnsureDirectory(const std::filesystem::path& path,
                              std::error_code* error = nullptr);

 private:
  std::filesystem::path home_ = "/root";
  std::filesystem::path working_directory_ = ".";
  std::filesystem::path sober_data_root_;
  std::filesystem::path sober_cache_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path config_root_;
  std::filesystem::path data_root_;
  std::filesystem::path state_root_;
  std::filesystem::path auth_root_;
  std::filesystem::path payloads_root_;
  std::filesystem::path downloads_root_;
  std::filesystem::path logs_root_;
  std::filesystem::path android_runtime_root_;
  std::filesystem::path android_data_root_;
  std::filesystem::path android_cache_root_;
  std::filesystem::path vulkan_shader_cache_file_;
  std::filesystem::path config_file_;
  std::filesystem::path active_payload_manifest_;
  std::filesystem::path cookie_file_;
  bool use_real_sober_paths_ = false;
};

// Publishes the XDG-backed host paths consumed by the transitional Bionic
// filesystem shim. Existing non-empty overrides remain authoritative.
bool ExportRuntimePathEnvironment(const RuntimePaths& paths,
                                  std::string* error = nullptr);

// Publishes the managed payload under the guest's rbx_bin paths.
bool PrepareManagedPayloadWorkingDirectory(const RuntimePaths& paths,
                                           const ActivePayloadPaths& active,
                                           std::string* error = nullptr);

// Keeps an explicit research libroblox.so override paired with the assets
// extracted from the same payload. Relative library paths are resolved against
// the runtime working directory so downstream policy files always receive an
// absolute path.
std::filesystem::path ResolveAdjacentRobloxAssetPath(
    const std::filesystem::path& roblox_library,
    const std::filesystem::path& working_directory);

// Saved accounts live under <auth root>/accounts, one 0700 directory per
// Roblox user id plus "guest". The "active" file there is the contract between
// the launcher and the runtime: "guest" or a user id, with one optional
// trailing newline, naming the directory the runtime uses as its auth root.
inline constexpr std::string_view kAccountStoreDirectoryName = "accounts";
inline constexpr std::string_view kActiveAccountFileName = "active";
inline constexpr std::string_view kGuestAccountSlotName = "guest";
inline constexpr std::size_t kMaximumActiveAccountFileBytes = 32;

// True when MOCKTAIL_AUTH_ROOT, MOCKTAIL_COOKIE_FILE or
// MOCKTAIL_ROBLOX_COOKIES picks the session instead of the account store.
bool AccountStoreOverriddenByEnvironment(const Environment& environment);

// Accepts a canonical positive decimal Roblox user id: no sign, no leading
// zero, at most INT64_MAX.
bool ParseAccountUserId(std::string_view text, std::int64_t* user_id);

struct ActiveAccountPointer {
  bool guest = true;
  std::int64_t user_id = 0;
};

inline bool operator==(const ActiveAccountPointer& left,
                       const ActiveAccountPointer& right) {
  return left.guest == right.guest &&
         (left.guest || left.user_id == right.user_id);
}
inline bool operator!=(const ActiveAccountPointer& left,
                       const ActiveAccountPointer& right) {
  return !(left == right);
}

std::optional<ActiveAccountPointer> ParseActiveAccountPointer(
    std::string_view contents);
std::string FormatActiveAccountPointer(const ActiveAccountPointer& pointer);

enum class ActiveAccountKind {
  // An environment override chooses the session; the auth root is unchanged.
  kEnvironmentOverride,
  // No account store (no accounts/active): the single legacy auth root.
  kLegacy,
  kGuest,
  kAccount,
  // The pointer or the account directory it names is unusable. The runtime
  // starts signed out in the guest slot and never falls back to another
  // account's session.
  kInvalidSelection,
};

struct ActiveAccountResolution {
  ActiveAccountKind kind = ActiveAccountKind::kLegacy;
  std::filesystem::path auth_root;
  std::int64_t user_id = 0;
  // Set when the store itself (accounts/ or accounts/guest) is not a private
  // directory of this user. Nothing was selected then.
  std::string error;

  explicit operator bool() const { return error.empty(); }
  bool uses_account_store() const {
    return kind == ActiveAccountKind::kGuest ||
           kind == ActiveAccountKind::kAccount ||
           kind == ActiveAccountKind::kInvalidSelection;
  }
};

// Resolves the auth root of the selected saved account. base_paths must be
// built without an account-store MOCKTAIL_AUTH_ROOT. Reads only; the caller
// exports MOCKTAIL_AUTH_ROOT and rebuilds its paths when uses_account_store().
ActiveAccountResolution ResolveActiveAccountAuthRoot(
    const RuntimePaths& base_paths, const Environment& environment);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_PATHS_H_
