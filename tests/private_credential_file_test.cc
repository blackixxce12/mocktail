#include "runtime/private_credential_file.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "services/auth_service.h"
#include "services/http_client.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr char kCredential[] = "_|test-private-credential-file";

class FakeHttpClient final : public services::HttpClient {
 public:
  services::HttpResponse Get(const services::HttpRequest& request) override {
    ++request_count;
    last_request = request;
    return response;
  }

  services::HttpResponse response;
  services::HttpRequest last_request;
  int request_count = 0;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_private_credential_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

mode_t ModeOf(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? (status.st_mode & 07777) : 0;
}

TEST(PrivateCredentialFileTest, ValidationReturnsTheAccountRobloxResolved) {
  FakeHttpClient http;
  http.response = services::HttpResponse(
      true, 200, R"({"id":42,"name":"builder","displayName":"Builder"})", {});
  services::AuthService auth(http);

  const RobloxSessionValidation validation =
      ValidateRobloxSession(auth, kCredential);

  EXPECT_EQ(validation.status, BrowserSignInStatus::kAccepted);
  EXPECT_EQ(validation.identity.user_id, 42);
  EXPECT_EQ(validation.identity.username, "builder");
  EXPECT_EQ(validation.identity.display_name, "Builder");
  EXPECT_EQ(validation.http_status, 200);
  ASSERT_EQ(http.request_count, 1);
  EXPECT_EQ(http.last_request.url,
            "https://users.roblox.com/v1/users/authenticated");
  EXPECT_FALSE(http.last_request.follow_redirects);
}

TEST(PrivateCredentialFileTest, ValidationAcceptsPrefixedSessions) {
  FakeHttpClient http;
  http.response = services::HttpResponse(
      true, 200, R"({"id":7,"name":"seven","displayName":""})", {});
  services::AuthService auth(http);

  const RobloxSessionValidation validation =
      ValidateRobloxSession(auth, std::string(".ROBLOSECURITY=") + kCredential);

  EXPECT_EQ(validation.status, BrowserSignInStatus::kAccepted);
  EXPECT_EQ(validation.identity.user_id, 7);
  bool sent = false;
  for (const std::string& header : http.last_request.headers) {
    sent =
        sent || header == std::string("Cookie: .ROBLOSECURITY=") + kCredential;
  }
  EXPECT_TRUE(sent);
}

TEST(PrivateCredentialFileTest, ValidationSeparatesRejectedFromUnverified) {
  FakeHttpClient http;
  services::AuthService auth(http);

  http.response = services::HttpResponse(true, 401, "{}", {});
  RobloxSessionValidation validation = ValidateRobloxSession(auth, kCredential);
  EXPECT_EQ(validation.status, BrowserSignInStatus::kRejected);
  EXPECT_EQ(validation.http_status, 401);
  EXPECT_EQ(validation.identity.user_id, -1);

  http.response = services::HttpResponse(false, 0, {}, kCredential);
  validation = ValidateRobloxSession(auth, kCredential);
  EXPECT_EQ(validation.status, BrowserSignInStatus::kUnverified);

  http.response = services::HttpResponse(true, 200, "not json", {});
  validation = ValidateRobloxSession(auth, kCredential);
  EXPECT_EQ(validation.status, BrowserSignInStatus::kUnverified);

  validation = ValidateRobloxSession(auth, "");
  EXPECT_EQ(validation.status, BrowserSignInStatus::kRejected);
}

TEST(PrivateCredentialFileTest, PersistsOnlyAcceptedSessionsInPrivateFiles) {
  TemporaryDirectory directory;
  const std::filesystem::path cookie =
      directory.path() / "auth/accounts/42/roblox.cookie";
  FakeHttpClient http;
  services::AuthService auth(http);

  http.response = services::HttpResponse(true, 403, "{}", {});
  EXPECT_EQ(PersistValidatedRobloxCookie(cookie, auth, kCredential),
            BrowserSignInStatus::kRejected);
  EXPECT_FALSE(std::filesystem::exists(cookie));

  http.response = services::HttpResponse(
      true, 200, R"({"id":42,"name":"builder","displayName":"Builder"})", {});
  EXPECT_EQ(PersistValidatedRobloxCookie(cookie, auth, kCredential),
            BrowserSignInStatus::kAccepted);
  EXPECT_EQ(ReadFile(cookie),
            std::string(".ROBLOSECURITY=") + kCredential + "\n");
  EXPECT_EQ(ModeOf(cookie), 0600u);
  EXPECT_EQ(ModeOf(cookie.parent_path()), 0700u);
}

TEST(PrivateCredentialFileTest, ReadRefusesSharedAndSymlinkedFiles) {
  TemporaryDirectory directory;
  const std::filesystem::path cookie = directory.path() / "roblox.cookie";
  ASSERT_TRUE(WritePrivateFileAtomically(cookie, "value\n"));

  PrivateCredentialFileReadResult read =
      ReadPrivateCredentialFile(cookie, false);
  EXPECT_EQ(read.status, PrivateCredentialFileStatus::kFound);
  EXPECT_EQ(read.contents, "value\n");

  ASSERT_EQ(chmod(cookie.c_str(), 0640), 0);
  read = ReadPrivateCredentialFile(cookie, false);
  EXPECT_EQ(read.status, PrivateCredentialFileStatus::kUnavailable);
  EXPECT_TRUE(read.contents.empty());

  const std::filesystem::path link = directory.path() / "link.cookie";
  ASSERT_EQ(chmod(cookie.c_str(), 0600), 0);
  std::filesystem::create_symlink(cookie, link);
  read = ReadPrivateCredentialFile(link, false);
  EXPECT_EQ(read.status, PrivateCredentialFileStatus::kUnavailable);

  read = ReadPrivateCredentialFile(directory.path() / "missing", false);
  EXPECT_EQ(read.status, PrivateCredentialFileStatus::kMissing);
  read = ReadPrivateCredentialFile(directory.path() / "missing", true);
  EXPECT_EQ(read.status, PrivateCredentialFileStatus::kUnavailable);
}

TEST(PrivateCredentialFileTest, WriterLockIsExclusiveAcrossDescriptors) {
  TemporaryDirectory directory;
  const int directory_descriptor =
      open(directory.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(directory_descriptor, 0);
  const int first =
      OpenPrivateFileWriterLock(directory_descriptor, "roblox.cookie");
  ASSERT_GE(first, 0);
  EXPECT_LT(OpenPrivateFileWriterLock(directory_descriptor, "roblox.cookie"),
            0);
  EXPECT_FALSE(WritePrivateFileAtomically(directory.path() / "roblox.cookie",
                                          "value\n"));
  close(first);
  EXPECT_TRUE(WritePrivateFileAtomically(directory.path() / "roblox.cookie",
                                         "value\n"));
  close(directory_descriptor);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
