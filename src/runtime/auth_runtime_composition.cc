#include "runtime/auth_runtime_composition.h"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "jnivm/jnivm.h"
#include "runtime/environment.h"
#include "runtime/private_credential_file.h"
#include "runtime/runtime_paths.h"
#include "services/auth_service.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr uintmax_t kMaximumCookieFileBytes =
    kMaximumPrivateCredentialFileBytes;

enum class CookieLoadStatus {
  kFound,
  kMissing,
  kUnavailable,
};

enum class CookieSource {
  kNone,
  kEnvironment,
  kExplicitFile,
  kManagedFile,
};

struct CookieLoadResult {
  CookieLoadResult() = default;
  CookieLoadResult(CookieLoadStatus initial_status, std::string initial_value,
                   std::string initial_error)
      : status(initial_status),
        value(std::move(initial_value)),
        error(std::move(initial_error)) {}

  CookieLoadStatus status = CookieLoadStatus::kMissing;
  std::string value;
  std::string error;
  CookieSource source = CookieSource::kNone;
  std::filesystem::path source_path;
};

struct CookiePersistenceContext {
  std::filesystem::path path;
  std::weak_ptr<jnivm::VM> vm;
  std::shared_ptr<services::HttpClient> live_auth_http_client;
  // Guards accepted_credential and identity promotion. Never held across a
  // request to Roblox.
  std::mutex promotion_mutex;
  // Engine cookie syncs replay the last session Roblox accepted.
  SecureRobloxCredential accepted_credential;
};

void ClearSensitiveString(std::string* value) { SecurelyClearString(value); }

void RetireRejectedSavedCredential(const std::filesystem::path& path,
                                   std::string_view credential);

bool PersistRobloxCredential(void* opaque, const char* data, size_t size) {
  auto* context = static_cast<CookiePersistenceContext*>(opaque);
  if (context == nullptr || context->path.empty() || data == nullptr ||
      size == 0 || size > kMaximumCookieFileBytes) {
    return false;
  }
  constexpr std::string_view kPrefix = ".ROBLOSECURITY=";
  const std::string_view credential(data, size);
  if (credential.size() <= kPrefix.size() ||
      credential.compare(0, kPrefix.size(), kPrefix) != 0 ||
      credential.find('\r') != std::string_view::npos ||
      credential.find('\n') != std::string_view::npos ||
      credential.find('\0') != std::string_view::npos) {
    return false;
  }

  const std::shared_ptr<jnivm::VM> vm = context->vm.lock();
  // A guest VM gets its session from a browser sign-in or a native login, and
  // WebKit or the engine can replay one Roblox already revoked: check a new
  // credential before it replaces the saved one. A VM whose account is
  // already resolved saves the sessions the engine rotates without waiting.
  bool check = false;
  {
    std::lock_guard<std::mutex> lock(context->promotion_mutex);
    check = context->live_auth_http_client != nullptr &&
            (vm == nullptr ||
             vm->GetRobloxAuthIdentitySnapshot().user_id <= 0) &&
            credential != context->accepted_credential.view();
  }
  services::AuthSession session;
  if (check) {
    services::AuthService auth_service(*context->live_auth_http_client);
    session = auth_service.ResolveSession(credential, false);
    if (session.status == services::AuthSessionStatus::kInvalid) {
      std::fprintf(stderr,
                   "  [auth] Roblox rejected a new sign-in session (HTTP %ld); "
                   "it was not saved\n",
                   session.http_status);
      if (session.http_status == 401 || session.http_status == 403) {
        RetireRejectedSavedCredential(context->path, credential);
      }
      return false;
    }
    if (session.status == services::AuthSessionStatus::kUnavailable) {
      std::fprintf(stderr,
                   "  [auth] new sign-in session could not be verified (%s); "
                   "saving it unverified\n",
                   session.error.c_str());
    }
  }

  std::string stored_credential(data, size);
  stored_credential.push_back('\n');
  const bool stored =
      WritePrivateFileAtomically(context->path, stored_credential);
  ClearSensitiveString(&stored_credential);
  if (!stored) {
    // Roblox did not refuse this session, so it is still used for this run. A
    // refusal would make the browser sign-in clear it from WebKit.
    std::fprintf(stderr,
                 "  [auth] could not save the sign-in session; using it for "
                 "this run only\n");
  }
  if (session.status == services::AuthSessionStatus::kAuthenticated) {
    std::lock_guard<std::mutex> lock(context->promotion_mutex);
    context->accepted_credential =
        SecureRobloxCredential(std::string(credential));
    if (vm != nullptr && vm->GetRobloxAuthIdentitySnapshot().user_id <= 0) {
      jnivm::RobloxAuthIdentity identity;
      identity.user_id = session.identity.user_id;
      identity.username = session.identity.username;
      identity.display_name = session.identity.display_name;
      vm->SetRobloxAuthIdentity(identity);
      std::fprintf(stderr,
                   "  [auth] native sign-in identity promoted into the "
                   "running VM\n");
    }
  }
  return true;
}

void InstallCredentialPersistence(const std::shared_ptr<jnivm::VM>& vm,
                                  const RuntimePaths& paths,
                                  std::shared_ptr<services::HttpClient>
                                      live_auth_http_client,
                                  std::string_view accepted_credential = {}) {
  if (vm == nullptr) {
    return;
  }
  auto context = std::make_shared<CookiePersistenceContext>();
  context->path = paths.cookie_file();
  context->vm = vm;
  context->live_auth_http_client = std::move(live_auth_http_client);
  context->accepted_credential =
      SecureRobloxCredential(std::string(accepted_credential));
  vm->SetRobloxCredentialSink(
      std::move(context),
      jnivm::RobloxCredentialSinkCallbacks{&PersistRobloxCredential});
}

bool Enabled(const Environment& environment, const char* name,
             bool default_value) {
  const std::optional<std::string> value = environment.Get(name);
  if (!value.has_value() || value->empty()) {
    return default_value;
  }
  return *value != "0";
}

CookieLoadResult ReadCookieFile(const std::filesystem::path& path,
                                bool missing_is_unavailable) {
  PrivateCredentialFileReadResult file =
      ReadPrivateCredentialFile(path, missing_is_unavailable);
  CookieLoadResult result;
  switch (file.status) {
    case PrivateCredentialFileStatus::kFound:
      result.status = CookieLoadStatus::kFound;
      break;
    case PrivateCredentialFileStatus::kMissing:
      result.status = CookieLoadStatus::kMissing;
      break;
    case PrivateCredentialFileStatus::kUnavailable:
      result.status = CookieLoadStatus::kUnavailable;
      break;
  }
  result.value = std::move(file.contents);
  result.error = std::move(file.error);
  return result;
}

CookieLoadResult ReadCookieSource(const std::filesystem::path& path,
                                  bool missing_is_unavailable,
                                  CookieSource source) {
  CookieLoadResult result = ReadCookieFile(path, missing_is_unavailable);
  if (result.status == CookieLoadStatus::kFound) {
    result.source = source;
    result.source_path = path;
  }
  return result;
}

bool IsAutomaticallyRecoverableSource(CookieSource source) {
  return source == CookieSource::kManagedFile;
}

// Matches ComposeAuthRuntime's recovery so a revoked session that was saved
// earlier is not offered again on the next launch.
void RetireRejectedSavedCredential(const std::filesystem::path& path,
                                   std::string_view credential) {
  CookieLoadResult saved = ReadCookieFile(path, false);
  std::string saved_value =
      services::AuthService::ExtractRoblosecurityValue(saved.value);
  std::string rejected_value =
      services::AuthService::ExtractRoblosecurityValue(credential);
  if (saved.status == CookieLoadStatus::kFound && !saved_value.empty() &&
      saved_value == rejected_value) {
    std::string error;
    if (ClearRejectedCookieFile(path, saved.value, &error)) {
      std::fprintf(stderr, "  [auth] rejected saved Roblox session retired\n");
    } else {
      std::fprintf(stderr,
                   "  [auth] rejected saved Roblox session was not retired: "
                   "%s\n",
                   error.c_str());
    }
  }
  ClearSensitiveString(&saved.value);
  ClearSensitiveString(&saved_value);
  ClearSensitiveString(&rejected_value);
}

CookieLoadResult LoadSavedCookie(const Environment& environment,
                                 const RuntimePaths& paths) {
  std::optional<std::string> environment_cookie =
      environment.Get("MOCKTAIL_ROBLOX_COOKIES");
  if (environment_cookie.has_value() && !environment_cookie->empty()) {
    CookieLoadResult result;
    result.status = CookieLoadStatus::kFound;
    result.value = std::move(*environment_cookie);
    result.source = CookieSource::kEnvironment;
    return result;
  }

  const std::optional<std::string> explicit_file =
      environment.Get("MOCKTAIL_COOKIE_FILE");
  if (explicit_file.has_value() && !explicit_file->empty()) {
    return ReadCookieSource(*explicit_file, true, CookieSource::kExplicitFile);
  }

  CookieLoadResult result =
      ReadCookieSource(paths.cookie_file(), false, CookieSource::kManagedFile);
  if (result.status == CookieLoadStatus::kFound &&
      !services::AuthService::HasRoblosecurityCookie(result.value)) {
    ClearSensitiveString(&result.value);
    result = {};
  } else if (result.status != CookieLoadStatus::kMissing) {
    return result;
  }
  return result;
}

}  // namespace

SecureRobloxCredential::SecureRobloxCredential(std::string canonical_header) {
  if (!canonical_header.empty()) {
    bytes_.assign(canonical_header.begin(), canonical_header.end());
    bytes_.push_back('\0');
  }
  ClearSensitiveString(&canonical_header);
}

SecureRobloxCredential::~SecureRobloxCredential() { Clear(); }

SecureRobloxCredential::SecureRobloxCredential(
    SecureRobloxCredential&& other) noexcept
    : bytes_(std::move(other.bytes_)) {}

SecureRobloxCredential& SecureRobloxCredential::operator=(
    SecureRobloxCredential&& other) noexcept {
  if (this != &other) {
    Clear();
    bytes_ = std::move(other.bytes_);
  }
  return *this;
}

void SecureRobloxCredential::Clear() {
  volatile char* byte = bytes_.empty() ? nullptr : bytes_.data();
  for (size_t index = 0; index < bytes_.size(); ++index) {
    byte[index] = '\0';
  }
  bytes_.clear();
}

ScopedRobloxCredentialBinding::ScopedRobloxCredentialBinding(
    jnivm::VM* jni_vm, const SecureRobloxCredential& credential)
    : jni_vm_(jni_vm) {
  if (jni_vm_ != nullptr) {
    jni_vm_->SetRobloxCredentialProvider(&credential, &ProvideCredential);
  }
}

ScopedRobloxCredentialBinding::~ScopedRobloxCredentialBinding() {
  if (jni_vm_ != nullptr) {
    jni_vm_->ClearRobloxCredentialProvider();
  }
}

jnivm::RobloxCredentialView
ScopedRobloxCredentialBinding::ProvideCredential(const void* context) {
  const auto* credential =
      static_cast<const SecureRobloxCredential*>(context);
  return credential != nullptr
             ? jnivm::RobloxCredentialView{credential->c_str(),
                                            credential->size()}
             : jnivm::RobloxCredentialView{};
}

AuthRuntimeComposition ComposeAuthRuntime(const Environment& environment,
                                          const RuntimePaths& paths,
                                          services::AuthService& auth_service) {
  return ComposeAuthRuntime(environment, paths, auth_service, nullptr);
}

AuthRuntimeComposition ComposeAuthRuntime(
    const Environment& environment, const RuntimePaths& paths,
    services::AuthService& auth_service,
    std::shared_ptr<services::HttpClient> live_auth_http_client) {
  AuthRuntimeComposition composition;
  CookieLoadResult cookie = LoadSavedCookie(environment, paths);
  if (cookie.status == CookieLoadStatus::kUnavailable) {
    composition.error = std::move(cookie.error);
    return composition;
  }

  SecureRobloxCredential credential;
  if (cookie.status == CookieLoadStatus::kFound) {
    std::string cookie_value =
        services::AuthService::ExtractRoblosecurityValue(cookie.value);
    if (!cookie_value.empty()) {
      std::string canonical_header = ".ROBLOSECURITY=";
      canonical_header += cookie_value;
      credential = SecureRobloxCredential(std::move(canonical_header));
    }
    ClearSensitiveString(&cookie_value);
  }
  const bool allow_guest =
      Enabled(environment, "MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", false);
  const services::AuthSession session = auth_service.ResolveSession(
      credential.empty() ? std::string_view(cookie.value) : credential.view(),
      allow_guest);

  if (session.status == services::AuthSessionStatus::kInvalid &&
      (session.http_status == 401 || session.http_status == 403) &&
      IsAutomaticallyRecoverableSource(cookie.source)) {
    std::string reset_error;
    if (cookie.source == CookieSource::kManagedFile &&
        !ClearRejectedCookieFile(cookie.source_path, cookie.value,
                                 &reset_error)) {
      ClearSensitiveString(&cookie.value);
      credential.Clear();
      composition.status = AuthRuntimeStatus::kUnavailable;
      composition.http_status = session.http_status;
      composition.error = std::move(reset_error);
      return composition;
    }
    ClearSensitiveString(&cookie.value);
    credential.Clear();
    composition.rejected_credential_retired = true;
    if (!allow_guest) {
      composition.status = AuthRuntimeStatus::kInvalidCredentials;
      composition.http_status = session.http_status;
      composition.error = session.error;
      return composition;
    }
    composition.status = AuthRuntimeStatus::kGuest;
    composition.http_status = session.http_status;
    composition.jni_vm = std::make_shared<jnivm::VM>();
    InstallCredentialPersistence(composition.jni_vm, paths,
                                 live_auth_http_client);
    return composition;
  }
  ClearSensitiveString(&cookie.value);

  composition.http_status = session.http_status;
  composition.error = session.error;
  switch (session.status) {
    case services::AuthSessionStatus::kAuthenticated: {
      auto jni_vm = std::make_shared<jnivm::VM>();
      jnivm::RobloxAuthIdentity identity;
      identity.user_id = session.identity.user_id;
      identity.username = session.identity.username;
      identity.display_name = session.identity.display_name;
      jni_vm->SetRobloxAuthIdentity(identity);
      InstallCredentialPersistence(jni_vm, paths, live_auth_http_client,
                                   credential.view());
      if (!credential.empty()) {
        (void)jni_vm->DispatchRobloxCredential(credential.c_str(),
                                               credential.size());
      }
      composition.status = AuthRuntimeStatus::kAuthenticated;
      composition.jni_vm = std::move(jni_vm);
      composition.account_identity = std::move(identity);
      composition.credential = std::move(credential);
      break;
    }
    case services::AuthSessionStatus::kGuest:
      composition.status = AuthRuntimeStatus::kGuest;
      composition.jni_vm = std::make_shared<jnivm::VM>();
      InstallCredentialPersistence(composition.jni_vm, paths,
                                   live_auth_http_client);
      break;
    case services::AuthSessionStatus::kInvalid:
      composition.status = AuthRuntimeStatus::kInvalidCredentials;
      break;
    case services::AuthSessionStatus::kUnavailable:
      composition.status = AuthRuntimeStatus::kUnavailable;
      break;
  }
  return composition;
}

}  // namespace runtime
}  // namespace mocktail
