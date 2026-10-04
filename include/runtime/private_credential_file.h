#ifndef MOCKTAIL_RUNTIME_PRIVATE_CREDENTIAL_FILE_H_
#define MOCKTAIL_RUNTIME_PRIVATE_CREDENTIAL_FILE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "services/auth_service.h"

// Private credential files shared by the game runtime and the launcher. Kept
// free of jnivm so tools that manage saved sessions never load the VM.
namespace mocktail {
namespace runtime {

inline constexpr std::uintmax_t kMaximumPrivateCredentialFileBytes =
    1024 * 1024;

// Overwrites the bytes before releasing them. Use for anything that held a
// credential.
void SecurelyClearString(std::string* value);

enum class PrivateCredentialFileStatus {
  kFound,
  kMissing,
  kUnavailable,
};

struct PrivateCredentialFileReadResult {
  PrivateCredentialFileStatus status = PrivateCredentialFileStatus::kMissing;
  // Sensitive: clear with SecurelyClearString.
  std::string contents;
  std::string error;
};

// Reads a credential file without following a final symlink. It must be a
// regular file owned by the effective user with no group or other access, at
// most kMaximumPrivateCredentialFileBytes long, and unchanged while read. An
// empty file counts as missing unless missing_is_unavailable is set.
PrivateCredentialFileReadResult ReadPrivateCredentialFile(
    const std::filesystem::path& path, bool missing_is_unavailable);

// Replaces path atomically with a 0600 file in a 0700 directory owned by the
// effective user, under the writer lock other credential writers take.
bool WritePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view contents);

// Opens and exclusively locks ".<filename>.mocktail-writer.lock" in the
// directory, without waiting. Returns -1 when another writer holds it or the
// lock file is unsafe.
int OpenPrivateFileWriterLock(int directory_descriptor,
                              std::string_view filename);

// Redacts every .ROBLOSECURITY segment of a rejected credential file in place
// after confirming it still holds loaded_contents. Other cookies, the file
// size and its delimiters are kept.
bool ClearRejectedCookieFile(const std::filesystem::path& path,
                             std::string_view loaded_contents,
                             std::string* error);

// Persists a raw or prefixed Roblox credential to the given cookie file
// atomically.
bool PersistRobloxCookie(const std::filesystem::path& path,
                         std::string_view cookie_value);

enum class BrowserSignInStatus {
  kAccepted,
  kRejected,
  kUnverified,
  kStoreFailed,
};

struct RobloxSessionValidation {
  // kAccepted, kRejected or kUnverified; never kStoreFailed.
  BrowserSignInStatus status = BrowserSignInStatus::kUnverified;
  services::AuthIdentity identity;
  long http_status = 0;
};

// Asks Roblox which account a raw or prefixed session belongs to. Blocks for
// one authentication request. The cookie never appears in the result.
RobloxSessionValidation ValidateRobloxSession(
    services::AuthService& auth_service, std::string_view cookie_value);

// Persists a browser sign-in cookie only after Roblox resolves its account.
// WebKit can still hold a revoked session, so a rejected or unverified cookie
// is never written. Blocks for one authentication request.
BrowserSignInStatus PersistValidatedRobloxCookie(
    const std::filesystem::path& path, services::AuthService& auth_service,
    std::string_view cookie_value);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PRIVATE_CREDENTIAL_FILE_H_
