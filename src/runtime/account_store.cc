#include "runtime/account_store.h"

#define JSON_NOEXCEPTION 1
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/environment.h"
#include "runtime/private_credential_file.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaximumMetadataBytes = 64 * 1024;
constexpr std::size_t kMaximumAppStorageBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumProfileResponseBytes = 64 * 1024;
constexpr std::size_t kMaximumAvatarBytes = 1024 * 1024;
constexpr std::size_t kMaximumAvatarUrlBytes = 512;
constexpr std::size_t kThumbnailBatchSize = 100;
constexpr std::int64_t kProfileRefreshIntervalSeconds = 24 * 60 * 60;
constexpr std::chrono::seconds kLockTimeout(10);
constexpr char kCookieFileName[] = "roblox.cookie";
constexpr char kAccountFileName[] = "account.json";
constexpr char kStoreFileName[] = "store.json";
constexpr char kStoreLockName[] = ".store.lock";
constexpr char kRoblosecurityPrefix[] = ".ROBLOSECURITY=";
constexpr char kNotExclusive[] =
    "saved accounts cannot change while Roblox is running";
constexpr std::array<const char*, 4> kCookieJarFiles = {
    "cookies.sqlite", "cookies.sqlite-wal", "cookies.sqlite-shm",
    "cookies.sqlite-journal"};
constexpr std::array<const char*, 2> kHydrationKeys = {
    "PlayerHydrationBlob", "PlayerHydrationSignature"};
constexpr std::array<unsigned char, 8> kPngSignature = {0x89, 'P',  'N',  'G',
                                                        '\r', '\n', 0x1a, '\n'};

class ScopedFd final {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int descriptor) : descriptor_(descriptor) {}
  ~ScopedFd() { Reset(); }

  ScopedFd(ScopedFd&& other) noexcept : descriptor_(other.Release()) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      Reset();
      descriptor_ = other.Release();
    }
    return *this;
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return descriptor_; }
  bool valid() const { return descriptor_ >= 0; }
  int Release() {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
  }
  void Reset() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
    descriptor_ = -1;
  }

 private:
  int descriptor_ = -1;
};

// Holds a session and clears it when it goes out of scope.
class SensitiveString final {
 public:
  SensitiveString() = default;
  ~SensitiveString() { SecurelyClearString(&value_); }
  SensitiveString(const SensitiveString&) = delete;
  SensitiveString& operator=(const SensitiveString&) = delete;

  std::string* get() { return &value_; }
  const std::string& value() const { return value_; }

 private:
  std::string value_;
};

bool IsPrivateDirectory(const struct stat& status) {
  return S_ISDIR(status.st_mode) && status.st_uid == geteuid() &&
         (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool WriteAll(int descriptor, std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count =
        write(descriptor, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReadAll(int descriptor, std::size_t maximum, std::string* contents) {
  contents->clear();
  std::array<char, 4096> buffer = {};
  while (true) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return false;
    }
    if (count == 0) {
      return true;
    }
    if (contents->size() + static_cast<std::size_t>(count) > maximum) {
      return false;
    }
    contents->append(buffer.data(), static_cast<std::size_t>(count));
  }
}

// Opens name under parent without following a symlink. With create, a
// missing directory is made 0700. Returns an invalid descriptor with an
// empty error when the directory is missing and create is false.
ScopedFd OpenPrivateDirectoryAt(int parent, const std::string& name,
                                bool create, std::string* error) {
  bool created = false;
  if (create) {
    if (mkdirat(parent, name.c_str(), S_IRWXU) == 0) {
      created = true;
    } else if (errno != EEXIST) {
      *error = "cannot create the saved account folder " + name;
      return {};
    }
  }
  ScopedFd directory(openat(parent, name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!directory.valid()) {
    if (errno != ENOENT) {
      *error = "saved account folder " + name + " is not a directory";
    }
    return {};
  }
  struct stat status = {};
  if (fstat(directory.get(), &status) != 0 ||
      (created &&
       (fchmod(directory.get(), S_IRWXU) != 0 ||
        fstat(directory.get(), &status) != 0 || fsync(parent) != 0)) ||
      !IsPrivateDirectory(status)) {
    *error = "saved account folder " + name + " is not private to this user";
    return {};
  }
  return directory;
}

// Opens the base auth root, creating it 0700 when asked. It must be a real
// directory of this user; the runtime's writers already require that.
ScopedFd OpenAuthRoot(const std::filesystem::path& auth_root, bool create,
                      std::string* error) {
  struct stat status = {};
  if (create && lstat(auth_root.c_str(), &status) != 0 && errno == ENOENT) {
    std::error_code filesystem_error;
    if (!RuntimePaths::EnsureDirectory(auth_root, &filesystem_error) ||
        chmod(auth_root.c_str(), S_IRWXU) != 0) {
      *error = "cannot create the Roblox sign-in folder";
      return {};
    }
  }
  ScopedFd directory(
      open(auth_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!directory.valid()) {
    if (errno != ENOENT) {
      *error = "the Roblox sign-in folder is not a directory";
    }
    return {};
  }
  if (fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid()) {
    *error = "the Roblox sign-in folder does not belong to this user";
    return {};
  }
  return directory;
}

enum class SmallFileStatus { kMissing, kRead, kUnsafe };

// Reads a private regular file of this user without following a symlink.
SmallFileStatus ReadPrivateFileAt(int directory, const std::string& name,
                                  std::size_t maximum, std::string* contents) {
  ScopedFd file(openat(directory, name.c_str(),
                       O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (!file.valid()) {
    return errno == ENOENT ? SmallFileStatus::kMissing
                           : SmallFileStatus::kUnsafe;
  }
  struct stat status = {};
  if (fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
      static_cast<std::size_t>(status.st_size) > maximum ||
      !ReadAll(file.get(), maximum, contents)) {
    return SmallFileStatus::kUnsafe;
  }
  return SmallFileStatus::kRead;
}

// Replaces name atomically with a 0600 file. The destination, if present,
// must be a regular file of this user with a single link.
bool WritePrivateFileAt(int directory, const std::string& name,
                        std::string_view contents) {
  struct stat existing = {};
  if (fstatat(directory, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode) || existing.st_uid != geteuid() ||
        existing.st_nlink != 1) {
      return false;
    }
  } else if (errno != ENOENT) {
    return false;
  }
  static std::atomic<std::uint64_t> serial{0};
  const std::string temporary = "." + name + ".tmp-" +
                                std::to_string(getpid()) + "-" +
                                std::to_string(serial.fetch_add(1));
  ScopedFd file(openat(directory, temporary.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                       S_IRUSR | S_IWUSR));
  if (!file.valid()) {
    return false;
  }
  const bool written = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
                       WriteAll(file.get(), contents) && fsync(file.get()) == 0;
  file.Reset();
  if (!written ||
      renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) {
    (void)unlinkat(directory, temporary.c_str(), 0);
    return false;
  }
  return fsync(directory) == 0;
}

// Unlinks a file or symlink; refuses directories. A missing name is fine.
bool UnlinkFileAt(int directory, const std::string& name) {
  struct stat status = {};
  if (fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT;
  }
  if (S_ISDIR(status.st_mode)) {
    return false;
  }
  return unlinkat(directory, name.c_str(), 0) == 0 || errno == ENOENT;
}

bool ListDirectory(int directory, std::vector<std::string>* names) {
  // A fresh open file description, so listing never moves another offset.
  const int listing =
      openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (listing < 0) {
    return false;
  }
  DIR* stream = fdopendir(listing);
  if (stream == nullptr) {
    close(listing);
    return false;
  }
  names->clear();
  errno = 0;
  while (const dirent* entry = readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") {
      names->emplace_back(name);
    }
    errno = 0;
  }
  const bool complete = errno == 0;
  closedir(stream);
  return complete;
}

// flock()s a lock file, retrying for a while instead of blocking forever.
bool LockFileWithTimeout(int descriptor) {
  const auto deadline = std::chrono::steady_clock::now() + kLockTimeout;
  while (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EINTR) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

ScopedFd OpenLockFileAt(int directory, const std::string& name) {
  ScopedFd lock(openat(directory, name.c_str(),
                       O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                       S_IRUSR | S_IWUSR));
  if (!lock.valid()) {
    return {};
  }
  struct stat status = {};
  if (fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || status.st_nlink != 1 ||
      fchmod(lock.get(), S_IRUSR | S_IWUSR) != 0 ||
      !LockFileWithTimeout(lock.get())) {
    return {};
  }
  return lock;
}

std::string Sha256Hex(std::string_view value) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest = {};
  unsigned int length = 0;
  if (EVP_Digest(value.data(), value.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1) {
    return {};
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(length * 2);
  for (unsigned int index = 0; index < length; ++index) {
    hex.push_back(kHex[digest[index] >> 4]);
    hex.push_back(kHex[digest[index] & 0x0f]);
  }
  return hex;
}

bool IsSha256Hex(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<std::int64_t> JsonInt64(const Json& value, std::int64_t minimum) {
  std::int64_t result = 0;
  if (value.is_number_unsigned()) {
    const std::uint64_t unsigned_value = value.get<std::uint64_t>();
    if (unsigned_value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    result = static_cast<std::int64_t>(unsigned_value);
  } else if (value.is_number_integer()) {
    result = value.get<std::int64_t>();
  } else {
    return std::nullopt;
  }
  if (result < minimum) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::string> JsonName(const Json& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const std::string& text = value.get_ref<const std::string&>();
  if (text.size() > kMaximumNameBytes ||
      std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    return std::nullopt;
  }
  return text;
}

std::string DumpJson(const Json& document) {
  return document.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::string PointerText(const ActiveAccountPointer& pointer) {
  std::string text = FormatActiveAccountPointer(pointer);
  text.pop_back();
  return text;
}

const char* StateName(SavedAccountState state) {
  switch (state) {
    case SavedAccountState::kSignedIn:
      return "signed_in";
    case SavedAccountState::kSignedOut:
      return "signed_out";
    case SavedAccountState::kUnverified:
      break;
  }
  return "unverified";
}

std::optional<SavedAccountState> ParseState(const Json& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const std::string& text = value.get_ref<const std::string&>();
  if (text == "signed_in") {
    return SavedAccountState::kSignedIn;
  }
  if (text == "signed_out") {
    return SavedAccountState::kSignedOut;
  }
  if (text == "unverified") {
    return SavedAccountState::kUnverified;
  }
  return std::nullopt;
}

// store.json: launcher-only metadata. Damaged content falls back to defaults
// (directory order), which the next change rewrites.
struct StoreDocument {
  std::vector<std::int64_t> order;
  std::optional<ActiveAccountPointer> last_launched;
};

StoreDocument ParseStoreDocument(std::string_view bytes) {
  StoreDocument store;
  const Json document = Json::parse(bytes, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return store;
  }
  const auto schema = document.find("schema_version");
  if (schema == document.end() || JsonInt64(*schema, 1) != kSchemaVersion) {
    return store;
  }
  const auto order = document.find("order");
  if (order != document.end() && order->is_array()) {
    for (const Json& entry : *order) {
      const std::optional<std::int64_t> user_id = JsonInt64(entry, 1);
      if (user_id.has_value() &&
          std::find(store.order.begin(), store.order.end(), *user_id) ==
              store.order.end()) {
        store.order.push_back(*user_id);
      }
    }
  }
  const auto last_launched = document.find("last_launched");
  if (last_launched != document.end() && last_launched->is_string()) {
    store.last_launched =
        ParseActiveAccountPointer(last_launched->get_ref<const std::string&>());
  }
  return store;
}

std::string SerializeStoreDocument(const StoreDocument& store) {
  Json document = Json::object();
  document["schema_version"] = kSchemaVersion;
  document["order"] = store.order;
  document["last_launched"] = store.last_launched.has_value()
                                  ? Json(PointerText(*store.last_launched))
                                  : Json(nullptr);
  return DumpJson(document);
}

struct AccountRecord {
  SavedAccount account;
  std::string credential_sha256;
};

// account.json is untrusted: every field is type-checked and bounded, and
// the user id must match its directory.
bool ParseAccountRecord(std::string_view bytes, std::int64_t user_id,
                        AccountRecord* record) {
  const Json document = Json::parse(bytes, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return false;
  }
  const auto field = [&document](const char* name) -> const Json* {
    const auto found = document.find(name);
    return found == document.end() ? nullptr : &*found;
  };
  const Json* schema = field("schema_version");
  const Json* id = field("user_id");
  if (schema == nullptr || JsonInt64(*schema, 1) != kSchemaVersion ||
      id == nullptr || JsonInt64(*id, 1) != user_id) {
    return false;
  }
  AccountRecord parsed;
  parsed.account.user_id = user_id;
  if (const Json* value = field("username")) {
    const std::optional<std::string> name = JsonName(*value);
    if (!name.has_value()) {
      return false;
    }
    parsed.account.username = *name;
  }
  if (const Json* value = field("display_name")) {
    const std::optional<std::string> name = JsonName(*value);
    if (!name.has_value()) {
      return false;
    }
    parsed.account.display_name = *name;
  }
  if (const Json* value = field("state")) {
    const std::optional<SavedAccountState> state = ParseState(*value);
    if (!state.has_value()) {
      return false;
    }
    parsed.account.state = *state;
  }
  const std::array<std::pair<const char*, std::int64_t*>, 4> timestamps = {{
      {"added_at", &parsed.account.added_at},
      {"last_used_at", &parsed.account.last_used_at},
      {"last_verified_at", &parsed.account.last_verified_at},
      {"profile_refreshed_at", &parsed.account.profile_refreshed_at},
  }};
  for (const auto& [name, target] : timestamps) {
    if (const Json* value = field(name)) {
      const std::optional<std::int64_t> timestamp = JsonInt64(*value, 0);
      if (!timestamp.has_value()) {
        return false;
      }
      *target = *timestamp;
    }
  }
  if (const Json* value = field("credential_sha256")) {
    if (!value->is_string() ||
        !IsSha256Hex(value->get_ref<const std::string&>())) {
      return false;
    }
    parsed.credential_sha256 = value->get<std::string>();
  }
  *record = std::move(parsed);
  return true;
}

std::string SerializeAccountRecord(const AccountRecord& record) {
  Json document = Json::object();
  document["schema_version"] = kSchemaVersion;
  document["user_id"] = record.account.user_id;
  document["username"] = record.account.username;
  document["display_name"] = record.account.display_name;
  document["state"] = StateName(record.account.state);
  document["added_at"] = record.account.added_at;
  document["last_used_at"] = record.account.last_used_at;
  document["last_verified_at"] = record.account.last_verified_at;
  document["profile_refreshed_at"] = record.account.profile_refreshed_at;
  if (!record.credential_sha256.empty()) {
    // A digest of a high-entropy bearer token, used only to notice that the
    // runtime saved a different session. It cannot be used to sign in.
    document["credential_sha256"] = record.credential_sha256;
  }
  return DumpJson(document);
}

// Reads <account directory>/account.json. A missing or damaged record yields
// an unverified record with no names.
AccountRecord ReadAccountRecordAt(int account_directory, std::int64_t user_id) {
  AccountRecord record;
  record.account.user_id = user_id;
  std::string bytes;
  if (ReadPrivateFileAt(account_directory, kAccountFileName,
                        kMaximumMetadataBytes,
                        &bytes) == SmallFileStatus::kRead) {
    (void)ParseAccountRecord(bytes, user_id, &record);
  }
  return record;
}

bool WriteAccountRecordAt(int account_directory, const AccountRecord& record) {
  return WritePrivateFileAt(account_directory, kAccountFileName,
                            SerializeAccountRecord(record));
}

// The session a slot's roblox.cookie holds, read with the runtime's rules.
struct SlotSession {
  bool readable = false;
  SensitiveString contents;
  SensitiveString value;
  std::string sha256;

  bool present() const { return !value.value().empty(); }
};

void ReadSlotSession(const std::filesystem::path& cookie_file,
                     SlotSession* session) {
  PrivateCredentialFileReadResult read =
      ReadPrivateCredentialFile(cookie_file, false);
  session->readable = read.status != PrivateCredentialFileStatus::kUnavailable;
  *session->contents.get() = std::move(read.contents);
  SecurelyClearString(&read.contents);
  *session->value.get() = services::AuthService::ExtractRoblosecurityValue(
      session->contents.value());
  session->sha256 =
      session->present() ? Sha256Hex(session->value.value()) : std::string();
}

std::string AccountName(std::int64_t user_id) {
  return std::to_string(user_id);
}

// The cookie-octets Roblox sessions use; anything else never reaches a file.
bool IsSafeSessionValue(std::string_view value) {
  return !value.empty() &&
         std::none_of(value.begin(), value.end(), [](unsigned char character) {
           return character <= 0x20 || character >= 0x7f || character == ';';
         });
}

bool IsAllowedAccountEntry(std::string_view name) {
  return name == kCookieFileName || name == kAccountFileName ||
         name == ".roblox.cookie.mocktail-writer.lock" ||
         StartsWith(name, ".roblox.cookie.tmp-") ||
         StartsWith(name, ".account.json.tmp-");
}

// The store under its lock, with store.json and the pointer as read there.
struct LockedStore {
  ScopedFd auth;
  ScopedFd accounts;
  ScopedFd lock;
  std::filesystem::path accounts_path;
  StoreDocument store;
  std::optional<ActiveAccountPointer> active;
  bool initialized = false;

  bool Open(const std::filesystem::path& auth_root, bool create,
            std::string* error) {
    auth = OpenAuthRoot(auth_root, create, error);
    if (!auth.valid()) {
      if (error->empty()) {
        *error = "there are no saved accounts";
      }
      return false;
    }
    accounts_path = auth_root / std::string(kAccountStoreDirectoryName);
    accounts = OpenPrivateDirectoryAt(
        auth.get(), std::string(kAccountStoreDirectoryName), create, error);
    if (!accounts.valid()) {
      if (error->empty()) {
        *error = "there are no saved accounts";
      }
      return false;
    }
    lock = OpenLockFileAt(accounts.get(), kStoreLockName);
    if (!lock.valid()) {
      *error = "the saved accounts are busy";
      return false;
    }
    std::string bytes;
    if (ReadPrivateFileAt(accounts.get(), kStoreFileName, kMaximumMetadataBytes,
                          &bytes) == SmallFileStatus::kRead) {
      store = ParseStoreDocument(bytes);
    }
    std::string pointer;
    const SmallFileStatus pointer_status =
        ReadPrivateFileAt(accounts.get(), std::string(kActiveAccountFileName),
                          kMaximumActiveAccountFileBytes, &pointer);
    initialized = pointer_status != SmallFileStatus::kMissing;
    if (pointer_status == SmallFileStatus::kRead) {
      active = ParseActiveAccountPointer(pointer);
    }
    return true;
  }

  bool HasAccountDirectory(std::int64_t user_id) const {
    struct stat status = {};
    return fstatat(accounts.get(), AccountName(user_id).c_str(), &status,
                   AT_SYMLINK_NOFOLLOW) == 0 &&
           IsPrivateDirectory(status);
  }

  bool SetActive(const ActiveAccountPointer& pointer, std::string* error) {
    if (!WritePrivateFileAt(accounts.get(), std::string(kActiveAccountFileName),
                            FormatActiveAccountPointer(pointer))) {
      *error = "cannot select the saved account";
      return false;
    }
    active = pointer;
    initialized = true;
    return true;
  }

  bool Commit(std::string* error) {
    if (!WritePrivateFileAt(accounts.get(), kStoreFileName,
                            SerializeStoreDocument(store))) {
      *error = "cannot save the account list";
      return false;
    }
    return true;
  }

  void AddToOrder(std::int64_t user_id) {
    if (std::find(store.order.begin(), store.order.end(), user_id) ==
        store.order.end()) {
      store.order.push_back(user_id);
    }
  }

  // Records that the session an account's slot holds is still its own.
  bool RecordVerified(const services::AuthIdentity& identity,
                      const std::string& sha256, std::int64_t now) {
    const ScopedFd account(
        openat(accounts.get(), AccountName(identity.user_id).c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!account.valid()) {
      return false;
    }
    AccountRecord record = ReadAccountRecordAt(account.get(), identity.user_id);
    record.account.username = identity.username;
    record.account.display_name = identity.display_name;
    record.account.state = SavedAccountState::kSignedIn;
    record.account.last_verified_at = now;
    record.credential_sha256 = sha256;
    return WriteAccountRecordAt(account.get(), record);
  }

  // Saves a session Roblox accepted for identity under its account.
  bool FileSession(const services::AuthIdentity& identity,
                   std::string_view value, std::int64_t now,
                   std::string* error) {
    const std::string name = AccountName(identity.user_id);
    const ScopedFd account =
        OpenPrivateDirectoryAt(accounts.get(), name, true, error);
    if (!account.valid()) {
      return false;
    }
    std::string canonical(kRoblosecurityPrefix);
    canonical.append(value).push_back('\n');
    const bool stored = WritePrivateFileAtomically(
        accounts_path / name / kCookieFileName, canonical);
    SecurelyClearString(&canonical);
    if (!stored) {
      *error = "cannot save the Roblox session for this account";
      return false;
    }
    AccountRecord record = ReadAccountRecordAt(account.get(), identity.user_id);
    if (record.account.added_at == 0) {
      record.account.added_at = now;
    }
    if (!identity.username.empty()) {
      record.account.username = identity.username;
      record.account.display_name = identity.display_name;
    }
    record.account.state = SavedAccountState::kSignedIn;
    record.account.last_verified_at = now;
    record.credential_sha256 = Sha256Hex(value);
    if (!WriteAccountRecordAt(account.get(), record)) {
      *error = "cannot save the account details";
      return false;
    }
    AddToOrder(identity.user_id);
    return true;
  }
};

// Deletes a slot's session if it still is the one that was checked.
bool DeleteSlotSession(const std::filesystem::path& slot_path,
                       std::string_view expected_sha256) {
  ScopedFd slot(
      open(slot_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!slot.valid()) {
    return false;
  }
  ScopedFd writer(OpenPrivateFileWriterLock(slot.get(), kCookieFileName));
  if (!writer.valid()) {
    return false;
  }
  SlotSession current;
  ReadSlotSession(slot_path / kCookieFileName, &current);
  if (current.sha256 != expected_sha256) {
    return false;
  }
  return unlinkat(slot.get(), kCookieFileName, 0) == 0 &&
         fsync(slot.get()) == 0;
}

bool RemoveHydrationKeys(const std::filesystem::path& app_storage_file,
                         std::string* error) {
  if (app_storage_file.empty()) {
    return true;
  }
  const std::filesystem::path parent = app_storage_file.parent_path();
  const std::string name = app_storage_file.filename().string();
  ScopedFd directory(
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!directory.valid()) {
    if (errno == ENOENT) {
      return true;
    }
    *error = "cannot open Roblox's app storage";
    return false;
  }
  struct stat directory_status = {};
  if (fstat(directory.get(), &directory_status) != 0 ||
      directory_status.st_uid != geteuid() ||
      (directory_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    *error = "Roblox's app storage folder is not private";
    return false;
  }
  struct stat storage_status = {};
  if (fstatat(directory.get(), name.c_str(), &storage_status,
              AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    *error = "cannot inspect Roblox's app storage";
    return false;
  }
  // The same lock the runtime's appStorage writers take.
  const ScopedFd lock = OpenLockFileAt(directory.get(), name + ".lock");
  if (!lock.valid()) {
    *error = "Roblox's app storage is busy";
    return false;
  }
  ScopedFd file(openat(directory.get(), name.c_str(),
                       O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (!file.valid()) {
    if (errno == ENOENT) {
      return true;
    }
    *error = "cannot open Roblox's app storage";
    return false;
  }
  std::string bytes;
  if (fstat(file.get(), &storage_status) != 0 ||
      !S_ISREG(storage_status.st_mode) || storage_status.st_uid != geteuid() ||
      storage_status.st_nlink != 1 ||
      !ReadAll(file.get(), kMaximumAppStorageBytes, &bytes)) {
    *error = "cannot read Roblox's app storage";
    return false;
  }
  file.Reset();
  Json storage = Json::parse(bytes, nullptr, false);
  std::fill(bytes.begin(), bytes.end(), '\0');
  if (storage.is_discarded() || !storage.is_object()) {
    *error = "Roblox's app storage is not a JSON object";
    return false;
  }
  std::size_t removed = 0;
  for (const char* key : kHydrationKeys) {
    removed += storage.erase(key);
  }
  if (removed == 0) {
    return true;
  }
  if (!WritePrivateFileAt(
          directory.get(), name,
          storage.dump(-1, ' ', false, Json::error_handler_t::replace))) {
    *error = "cannot update Roblox's app storage";
    return false;
  }
  return true;
}

bool DeleteCookieJar(const std::filesystem::path& webview_directory,
                     std::string* error) {
  if (webview_directory.empty()) {
    return true;
  }
  ScopedFd directory(open(webview_directory.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!directory.valid()) {
    if (errno == ENOENT) {
      return true;
    }
    *error = "cannot open the sign-in window's storage";
    return false;
  }
  struct stat status = {};
  if (fstat(directory.get(), &status) != 0 || status.st_uid != geteuid()) {
    *error = "the sign-in window's storage does not belong to this user";
    return false;
  }
  for (const char* name : kCookieJarFiles) {
    if (!UnlinkFileAt(directory.get(), name)) {
      *error = "cannot delete the sign-in window's cookies";
      return false;
    }
  }
  if (fsync(directory.get()) != 0) {
    *error = "cannot delete the sign-in window's cookies";
    return false;
  }
  return true;
}

bool IsSafeAvatarUrl(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  constexpr std::string_view kHostSuffix = ".rbxcdn.com";
  if (url.size() > kMaximumAvatarUrlBytes || !StartsWith(url, kScheme) ||
      std::any_of(url.begin(), url.end(), [](unsigned char character) {
        return character <= 0x20 || character >= 0x7f || character == '\\';
      })) {
    return false;
  }
  const std::string_view rest = url.substr(kScheme.size());
  const std::string_view host = rest.substr(0, rest.find_first_of("/?#"));
  if (host.find_first_of("@:") != std::string_view::npos ||
      host.size() <= kHostSuffix.size()) {
    return false;
  }
  return host.compare(host.size() - kHostSuffix.size(), kHostSuffix.size(),
                      kHostSuffix) == 0;
}

services::HttpRequest PublicRequest(std::string url, std::size_t maximum) {
  services::HttpRequest request;
  request.url = std::move(url);
  request.timeout_ms = 15000;
  request.maximum_body_bytes = maximum;
  request.follow_redirects = false;
  request.headers.push_back("Accept: application/json");
  return request;
}

// Public names for one account; no session is sent.
bool FetchPublicNames(services::HttpClient& http_client, std::int64_t user_id,
                      std::string* username, std::string* display_name) {
  const services::HttpResponse response = http_client.Get(
      PublicRequest("https://users.roblox.com/v1/users/" + AccountName(user_id),
                    kMaximumProfileResponseBytes));
  if (!response.transport_ok || response.status_code != 200) {
    return false;
  }
  const Json document = Json::parse(response.body, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return false;
  }
  const auto id = document.find("id");
  const auto name = document.find("name");
  const auto display = document.find("displayName");
  if (id == document.end() || JsonInt64(*id, 1) != user_id ||
      name == document.end() || display == document.end()) {
    return false;
  }
  const std::optional<std::string> parsed_name = JsonName(*name);
  const std::optional<std::string> parsed_display = JsonName(*display);
  if (!parsed_name.has_value() || parsed_name->empty() ||
      !parsed_display.has_value()) {
    return false;
  }
  *username = *parsed_name;
  *display_name = *parsed_display;
  return true;
}

// Headshot URLs for up to kThumbnailBatchSize accounts that Roblox has
// finished rendering.
std::vector<std::pair<std::int64_t, std::string>> FetchAvatarUrls(
    services::HttpClient& http_client, const std::vector<std::int64_t>& ids) {
  std::vector<std::pair<std::int64_t, std::string>> urls;
  std::string joined;
  for (const std::int64_t id : ids) {
    if (!joined.empty()) {
      joined.push_back(',');
    }
    joined.append(AccountName(id));
  }
  const services::HttpResponse response = http_client.Get(PublicRequest(
      "https://thumbnails.roblox.com/v1/users/avatar-headshot?userIds=" +
          joined + "&size=150x150&format=Png&isCircular=false",
      kMaximumProfileResponseBytes));
  if (!response.transport_ok || response.status_code != 200) {
    return urls;
  }
  const Json document = Json::parse(response.body, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return urls;
  }
  const auto data = document.find("data");
  if (data == document.end() || !data->is_array()) {
    return urls;
  }
  for (const Json& entry : *data) {
    if (!entry.is_object()) {
      continue;
    }
    const auto target = entry.find("targetId");
    const auto state = entry.find("state");
    const auto image = entry.find("imageUrl");
    if (target == entry.end() || state == entry.end() || image == entry.end() ||
        !state->is_string() ||
        state->get_ref<const std::string&>() != "Completed" ||
        !image->is_string()) {
      continue;
    }
    const std::optional<std::int64_t> id = JsonInt64(*target, 1);
    const std::string& url = image->get_ref<const std::string&>();
    if (id.has_value() && std::find(ids.begin(), ids.end(), *id) != ids.end() &&
        IsSafeAvatarUrl(url)) {
      urls.emplace_back(*id, url);
    }
  }
  return urls;
}

ScopedFd OpenAvatarDirectory(const std::filesystem::path& avatar_directory,
                             bool create, std::string* error) {
  const std::filesystem::path parent = avatar_directory.parent_path();
  std::error_code filesystem_error;
  if (create && !RuntimePaths::EnsureDirectory(parent, &filesystem_error)) {
    *error = "cannot create the avatar cache";
    return {};
  }
  ScopedFd parent_directory(
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!parent_directory.valid()) {
    if (errno != ENOENT) {
      *error = "cannot open the avatar cache";
    }
    return {};
  }
  return OpenPrivateDirectoryAt(parent_directory.get(),
                                avatar_directory.filename().string(), create,
                                error);
}

std::string AvatarName(std::int64_t user_id) {
  return AccountName(user_id) + ".png";
}

}  // namespace

AccountStoreOptions MakeAccountStoreOptions(const RuntimePaths& base_paths,
                                            const Environment& environment,
                                            bool assume_exclusive) {
  AccountStoreOptions options;
  options.auth_root = base_paths.auth_root();
  const std::filesystem::path runtime_root =
      environment.HasNonEmpty("MOCKTAIL_RUNTIME_ROOT")
          ? std::filesystem::path(
                environment.GetOr("MOCKTAIL_RUNTIME_ROOT", ""))
          : base_paths.android_runtime_root();
  options.app_storage_file =
      runtime_root / "data/files/appData/LocalStorage/appStorage.json";
  // The WebKit helper keeps its jar under g_get_user_data_dir(), which
  // ignores MOCKTAIL_DATA_ROOT.
  const std::filesystem::path xdg_data_home =
      environment.GetOr("XDG_DATA_HOME", "");
  options.webview_data_directory =
      (xdg_data_home.is_absolute() ? xdg_data_home
                                   : base_paths.home() / ".local/share") /
      "mocktail/webview";
  options.avatar_directory = base_paths.cache_root() / "avatars";
  options.assume_exclusive = assume_exclusive;
  return options;
}

AccountStore::AccountStore(AccountStoreOptions options)
    : options_(std::move(options)) {}

std::filesystem::path AccountStore::accounts_directory() const {
  return options_.auth_root / std::string(kAccountStoreDirectoryName);
}

std::int64_t AccountStore::Now() const {
  if (options_.now) {
    return options_.now();
  }
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool AccountStore::Load(AccountStoreSnapshot* snapshot,
                        std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  *snapshot = {};
  SlotSession legacy;
  ReadSlotSession(options_.auth_root / kCookieFileName, &legacy);
  snapshot->legacy_session_present = legacy.present();

  const ScopedFd auth = OpenAuthRoot(options_.auth_root, false, error);
  if (!auth.valid()) {
    return error->empty();
  }
  const ScopedFd accounts = OpenPrivateDirectoryAt(
      auth.get(), std::string(kAccountStoreDirectoryName), false, error);
  if (!accounts.valid()) {
    return error->empty();
  }
  std::string pointer;
  const SmallFileStatus pointer_status =
      ReadPrivateFileAt(accounts.get(), std::string(kActiveAccountFileName),
                        kMaximumActiveAccountFileBytes, &pointer);
  snapshot->initialized = pointer_status != SmallFileStatus::kMissing;
  if (pointer_status == SmallFileStatus::kRead) {
    snapshot->active = ParseActiveAccountPointer(pointer);
  }
  StoreDocument store;
  std::string bytes;
  if (ReadPrivateFileAt(accounts.get(), kStoreFileName, kMaximumMetadataBytes,
                        &bytes) == SmallFileStatus::kRead) {
    store = ParseStoreDocument(bytes);
  }
  snapshot->last_launched = store.last_launched;

  std::vector<std::string> names;
  if (!ListDirectory(accounts.get(), &names)) {
    *error = "cannot list the saved accounts";
    return false;
  }
  std::vector<std::int64_t> found;
  for (const std::string& name : names) {
    std::int64_t user_id = 0;
    struct stat status = {};
    if (ParseAccountUserId(name, &user_id) &&
        fstatat(accounts.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) ==
            0 &&
        IsPrivateDirectory(status)) {
      found.push_back(user_id);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<std::int64_t> ordered;
  for (const std::int64_t user_id : store.order) {
    if (std::binary_search(found.begin(), found.end(), user_id)) {
      ordered.push_back(user_id);
    }
  }
  for (const std::int64_t user_id : found) {
    if (std::find(ordered.begin(), ordered.end(), user_id) == ordered.end()) {
      ordered.push_back(user_id);
    }
  }

  for (const std::int64_t user_id : ordered) {
    const std::string name = AccountName(user_id);
    const ScopedFd account(
        openat(accounts.get(), name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!account.valid()) {
      continue;
    }
    AccountRecord record = ReadAccountRecordAt(account.get(), user_id);
    SlotSession session;
    ReadSlotSession(accounts_directory() / name / kCookieFileName, &session);
    record.account.has_session = session.present();
    if (!record.account.has_session) {
      record.account.state = SavedAccountState::kSignedOut;
    }
    const std::filesystem::path avatar =
        options_.avatar_directory / AvatarName(user_id);
    struct stat avatar_status = {};
    if (!options_.avatar_directory.empty() &&
        lstat(avatar.c_str(), &avatar_status) == 0 &&
        S_ISREG(avatar_status.st_mode)) {
      record.account.avatar_file = avatar;
    }
    snapshot->accounts.push_back(std::move(record.account));
  }

  SlotSession guest;
  ReadSlotSession(accounts_directory() / std::string(kGuestAccountSlotName) /
                      kCookieFileName,
                  &guest);
  snapshot->guest_session_pending = guest.present();
  return true;
}

AccountMigrationResult AccountStore::MigrateLegacySession(
    services::AuthService& auth_service) const {
  AccountMigrationResult result;
  if (!options_.assume_exclusive) {
    result.error = kNotExclusive;
    return result;
  }
  const std::filesystem::path legacy_file =
      options_.auth_root / kCookieFileName;
  SlotSession legacy;
  ReadSlotSession(legacy_file, &legacy);
  if (!legacy.readable) {
    result.error = "the saved Roblox session cannot be read safely";
    return result;
  }

  RobloxSessionValidation validation;
  if (legacy.present()) {
    validation = ValidateRobloxSession(auth_service, legacy.value.value());
    if (validation.status == BrowserSignInStatus::kUnverified) {
      result.outcome = AccountMigrationOutcome::kPostponed;
      return result;
    }
  }

  LockedStore locked;
  if (!locked.Open(options_.auth_root, true, &result.error)) {
    return result;
  }
  // The legacy file may have changed while Roblox was asked.
  SlotSession current;
  ReadSlotSession(legacy_file, &current);
  if (current.sha256 != legacy.sha256) {
    result.outcome = AccountMigrationOutcome::kPostponed;
    return result;
  }

  if (!legacy.present()) {
    result.outcome = AccountMigrationOutcome::kNothingToMigrate;
  } else if (validation.status == BrowserSignInStatus::kRejected) {
    result.outcome = AccountMigrationOutcome::kRejected;
  } else {
    result.outcome = AccountMigrationOutcome::kMigrated;
    result.user_id = validation.identity.user_id;
    if (!locked.FileSession(validation.identity, legacy.value.value(), Now(),
                            &result.error)) {
      return result;
    }
  }

  ActiveAccountPointer migrated;
  if (result.outcome == AccountMigrationOutcome::kMigrated) {
    migrated.guest = false;
    migrated.user_id = result.user_id;
  }
  if (!locked.initialized) {
    // The legacy runs used the shared app data with this session.
    if (result.outcome == AccountMigrationOutcome::kMigrated) {
      locked.store.last_launched = migrated;
    }
    // The pointer goes last: until it exists the runtime keeps the legacy
    // root.
    if (!locked.Commit(&result.error) ||
        !locked.SetActive(migrated, &result.error)) {
      return result;
    }
  } else if (legacy.present()) {
    // A run outside the store saved this session after the store was made:
    // an older Mocktail, or one whose session came from MOCKTAIL_COOKIE_FILE
    // or MOCKTAIL_ROBLOX_COOKIES. It used the shared app data since the slot
    // store.json names, so the next start clears it, and the user's
    // selection stays unless it is unusable anyway.
    locked.store.last_launched.reset();
    if (!locked.Commit(&result.error)) {
      return result;
    }
    const bool selection_usable =
        locked.active.has_value() &&
        (locked.active->guest ||
         locked.HasAccountDirectory(locked.active->user_id));
    if (result.outcome == AccountMigrationOutcome::kMigrated &&
        !selection_usable && !locked.SetActive(migrated, &result.error)) {
      return result;
    }
  }

  struct stat status = {};
  if (fstatat(locked.auth.get(), kCookieFileName, &status,
              AT_SYMLINK_NOFOLLOW) != 0 &&
      errno == ENOENT) {
    return result;
  }
  const ScopedFd writer(
      OpenPrivateFileWriterLock(locked.auth.get(), kCookieFileName));
  if (!writer.valid() || !UnlinkFileAt(locked.auth.get(), kCookieFileName) ||
      !UnlinkFileAt(locked.auth.get(), ".roblox.cookie.mocktail-writer.lock") ||
      fsync(locked.auth.get()) != 0) {
    result.error = "the old saved session could not be removed";
  }
  return result;
}

AccountReconcileResult AccountStore::Reconcile(
    services::AuthService& auth_service) const {
  AccountReconcileResult result;
  if (!options_.assume_exclusive) {
    result.error = kNotExclusive;
    return result;
  }
  AccountStoreSnapshot snapshot;
  if (!Load(&snapshot, &result.error)) {
    return result;
  }
  if (!snapshot.initialized) {
    return result;
  }

  // Collect the sessions to check without holding the store lock: requests
  // can take seconds and the launcher keeps using the store meanwhile.
  struct Candidate {
    std::optional<std::int64_t> slot_user_id;  // nullopt: the guest slot
    std::filesystem::path slot_path;
    std::string sha256;
    RobloxSessionValidation validation;
  };
  std::vector<Candidate> candidates;
  const auto add_candidate = [&](std::optional<std::int64_t> slot_user_id,
                                 const std::filesystem::path& slot_path,
                                 const std::string& recorded_sha256) {
    SlotSession session;
    ReadSlotSession(slot_path / kCookieFileName, &session);
    if (!session.present() || session.sha256 == recorded_sha256) {
      return;
    }
    Candidate candidate;
    candidate.slot_user_id = slot_user_id;
    candidate.slot_path = slot_path;
    candidate.sha256 = session.sha256;
    candidate.validation =
        ValidateRobloxSession(auth_service, session.value.value());
    candidates.push_back(std::move(candidate));
  };
  add_candidate(std::nullopt,
                accounts_directory() / std::string(kGuestAccountSlotName), {});
  {
    const ScopedFd accounts(
        open(accounts_directory().c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    for (const SavedAccount& account : snapshot.accounts) {
      if (!account.has_session || !accounts.valid()) {
        continue;
      }
      const std::string name = AccountName(account.user_id);
      const ScopedFd directory(
          openat(accounts.get(), name.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (!directory.valid()) {
        continue;
      }
      const AccountRecord record =
          ReadAccountRecordAt(directory.get(), account.user_id);
      add_candidate(account.user_id, accounts_directory() / name,
                    record.credential_sha256);
    }
  }

  LockedStore locked;
  if (!locked.Open(options_.auth_root, false, &result.error)) {
    return result;
  }
  const std::int64_t now = Now();
  const auto mark_signed_out = [&](std::int64_t user_id) {
    const ScopedFd directory(
        openat(locked.accounts.get(), AccountName(user_id).c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.valid()) {
      return;
    }
    AccountRecord record = ReadAccountRecordAt(directory.get(), user_id);
    if (record.account.state == SavedAccountState::kSignedOut &&
        record.credential_sha256.empty()) {
      return;
    }
    record.account.state = SavedAccountState::kSignedOut;
    record.credential_sha256.clear();
    if (WriteAccountRecordAt(directory.get(), record)) {
      result.signed_out.push_back(user_id);
    }
  };
  const auto move_selection = [&](const ActiveAccountPointer& from,
                                  std::int64_t to) {
    ActiveAccountPointer pointer;
    pointer.guest = false;
    pointer.user_id = to;
    if (!locked.active.has_value() || *locked.active == from) {
      std::string error;
      if (locked.SetActive(pointer, &error)) {
        result.active_changed = true;
      }
    }
    if (locked.store.last_launched.has_value() &&
        *locked.store.last_launched == from) {
      locked.store.last_launched = pointer;
    }
  };

  for (Candidate& candidate : candidates) {
    SlotSession current;
    ReadSlotSession(candidate.slot_path / kCookieFileName, &current);
    if (current.sha256 != candidate.sha256) {
      // Changed while Roblox was asked: check it next time.
      result.pending = true;
      continue;
    }
    ActiveAccountPointer slot;
    if (candidate.slot_user_id.has_value()) {
      slot.guest = false;
      slot.user_id = *candidate.slot_user_id;
    }
    const RobloxSessionValidation& validation = candidate.validation;
    if (validation.status == BrowserSignInStatus::kUnverified) {
      result.pending = true;
      if (candidate.slot_user_id.has_value()) {
        const ScopedFd directory(
            open(candidate.slot_path.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory.valid()) {
          AccountRecord record =
              ReadAccountRecordAt(directory.get(), slot.user_id);
          record.account.state = SavedAccountState::kUnverified;
          (void)WriteAccountRecordAt(directory.get(), record);
        }
      }
      continue;
    }
    if (validation.status == BrowserSignInStatus::kRejected) {
      if (!candidate.slot_user_id.has_value()) {
        if (!DeleteSlotSession(candidate.slot_path, candidate.sha256)) {
          result.pending = true;
        }
        continue;
      }
      // Retire it the way the runtime does, keeping other cookies.
      std::string error;
      if (ClearRejectedCookieFile(candidate.slot_path / kCookieFileName,
                                  current.contents.value(), &error)) {
        mark_signed_out(slot.user_id);
      } else {
        result.pending = true;
      }
      continue;
    }

    const std::int64_t resolved = validation.identity.user_id;
    if (candidate.slot_user_id == resolved) {
      if (locked.RecordVerified(validation.identity, candidate.sha256, now)) {
        result.verified.push_back(resolved);
      } else {
        result.pending = true;
      }
      continue;
    }
    // A sign-in inside Roblox (or its account switcher) saved another
    // account's session in this slot: file it under that account.
    if (!locked.FileSession(validation.identity, current.value.value(), now,
                            &result.error)) {
      return result;
    }
    result.filed.push_back(resolved);
    if (!DeleteSlotSession(candidate.slot_path, candidate.sha256)) {
      result.pending = true;
    }
    if (candidate.slot_user_id.has_value()) {
      mark_signed_out(slot.user_id);
    }
    move_selection(slot, resolved);
  }

  for (const SavedAccount& account : snapshot.accounts) {
    locked.AddToOrder(account.user_id);
  }
  locked.store.order.erase(
      std::remove_if(locked.store.order.begin(), locked.store.order.end(),
                     [&locked](std::int64_t user_id) {
                       return !locked.HasAccountDirectory(user_id);
                     }),
      locked.store.order.end());
  // Sessions the runtime retired (or that vanished) sign their account out.
  for (const std::int64_t user_id : locked.store.order) {
    SlotSession session;
    ReadSlotSession(
        locked.accounts_path / AccountName(user_id) / kCookieFileName,
        &session);
    if (!session.present() &&
        std::find(result.signed_out.begin(), result.signed_out.end(),
                  user_id) == result.signed_out.end()) {
      mark_signed_out(user_id);
    }
  }
  (void)locked.Commit(&result.error);
  return result;
}

bool AccountStore::AddValidatedSession(const services::AuthIdentity& identity,
                                       std::string_view cookie_value,
                                       bool make_active,
                                       std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  if (!options_.assume_exclusive) {
    *error = kNotExclusive;
    return false;
  }
  if (identity.user_id <= 0 || identity.username.empty()) {
    *error = "Roblox did not identify the account";
    return false;
  }
  SensitiveString value;
  *value.get() = services::AuthService::ExtractRoblosecurityValue(cookie_value);
  if (!IsSafeSessionValue(value.value())) {
    *error = "the sign-in did not produce a Roblox session";
    return false;
  }
  LockedStore locked;
  if (!locked.Open(options_.auth_root, true, error) ||
      !locked.FileSession(identity, value.value(), Now(), error)) {
    return false;
  }
  if (make_active) {
    ActiveAccountPointer pointer;
    pointer.guest = false;
    pointer.user_id = identity.user_id;
    // The sign-in window's jar now holds this account's session, and
    // PrepareBrowserSignIn cleared everything else.
    locked.store.last_launched = pointer;
    return locked.Commit(error) && locked.SetActive(pointer, error);
  }
  return locked.Commit(error);
}

bool AccountStore::SelectForLaunch(const ActiveAccountPointer& selection,
                                   std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  if (!options_.assume_exclusive) {
    *error = kNotExclusive;
    return false;
  }
  LockedStore locked;
  if (!locked.Open(options_.auth_root, true, error)) {
    return false;
  }
  if (!selection.guest && (selection.user_id <= 0 ||
                           !locked.HasAccountDirectory(selection.user_id))) {
    *error = "this account is not saved";
    return false;
  }
  // Another account's web session or hydration data must not reach this
  // start. A guest start always gets a clean jar.
  if (selection.guest || !locked.store.last_launched.has_value() ||
      *locked.store.last_launched != selection) {
    if (!ClearSessionArtifacts(error)) {
      return false;
    }
  }
  locked.store.last_launched = selection;
  if (!selection.guest) {
    const ScopedFd directory(
        openat(locked.accounts.get(), AccountName(selection.user_id).c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.valid()) {
      AccountRecord record =
          ReadAccountRecordAt(directory.get(), selection.user_id);
      record.account.last_used_at = Now();
      (void)WriteAccountRecordAt(directory.get(), record);
    }
  }
  return locked.Commit(error) && locked.SetActive(selection, error);
}

bool AccountStore::PrepareBrowserSignIn(std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  if (!options_.assume_exclusive) {
    *error = kNotExclusive;
    return false;
  }
  LockedStore locked;
  if (!locked.Open(options_.auth_root, true, error) ||
      !ClearSessionArtifacts(error)) {
    return false;
  }
  // The jar is about to hold the new account's session: whatever starts next
  // must clear it unless it is that account.
  locked.store.last_launched.reset();
  return locked.Commit(error);
}

bool AccountStore::RemoveAccount(std::int64_t user_id,
                                 std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  if (!options_.assume_exclusive) {
    *error = kNotExclusive;
    return false;
  }
  if (user_id <= 0) {
    *error = "this account is not saved";
    return false;
  }
  LockedStore locked;
  if (!locked.Open(options_.auth_root, false, error)) {
    return false;
  }
  const std::string name = AccountName(user_id);
  ScopedFd account =
      OpenPrivateDirectoryAt(locked.accounts.get(), name, false, error);
  if (!account.valid() && !error->empty()) {
    return false;
  }
  if (account.valid()) {
    const ScopedFd writer(
        OpenPrivateFileWriterLock(account.get(), kCookieFileName));
    if (!writer.valid()) {
      *error = "the account's session is busy";
      return false;
    }
    std::vector<std::string> entries;
    if (!ListDirectory(account.get(), &entries)) {
      *error = "cannot list the account folder";
      return false;
    }
    for (const std::string& entry : entries) {
      struct stat status = {};
      if (!IsAllowedAccountEntry(entry) ||
          fstatat(account.get(), entry.c_str(), &status, AT_SYMLINK_NOFOLLOW) !=
              0 ||
          S_ISDIR(status.st_mode)) {
        *error = "the account folder holds an unexpected file (" + entry +
                 "); nothing was removed";
        return false;
      }
    }
    // The jar may hold this account's web session.
    if (!ClearSessionArtifacts(error)) {
      return false;
    }
    // Best effort: copy-on-write filesystems may keep the old blocks.
    ScopedFd cookie(openat(account.get(), kCookieFileName,
                           O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat cookie_status = {};
    if (cookie.valid() && fstat(cookie.get(), &cookie_status) == 0 &&
        S_ISREG(cookie_status.st_mode) && cookie_status.st_nlink == 1 &&
        cookie_status.st_size > 0) {
      const std::string zeros(static_cast<std::size_t>(cookie_status.st_size),
                              '\0');
      if (WriteAll(cookie.get(), zeros)) {
        (void)fsync(cookie.get());
      }
    }
    cookie.Reset();
    // The writer lock goes last; it stays held until the folder is gone.
    std::sort(entries.begin(), entries.end(),
              [](const std::string& left, const std::string& right) {
                const bool left_lock =
                    left == ".roblox.cookie.mocktail-writer.lock";
                const bool right_lock =
                    right == ".roblox.cookie.mocktail-writer.lock";
                return left_lock != right_lock ? right_lock : left < right;
              });
    for (const std::string& entry : entries) {
      if (unlinkat(account.get(), entry.c_str(), 0) != 0 && errno != ENOENT) {
        *error = "cannot remove the account folder";
        return false;
      }
    }
    account.Reset();
    if (unlinkat(locked.accounts.get(), name.c_str(), AT_REMOVEDIR) != 0 ||
        fsync(locked.accounts.get()) != 0) {
      *error = "cannot remove the account folder";
      return false;
    }
  }

  std::string avatar_error;
  const ScopedFd avatars =
      OpenAvatarDirectory(options_.avatar_directory, false, &avatar_error);
  if (avatars.valid()) {
    (void)UnlinkFileAt(avatars.get(), AvatarName(user_id));
  }

  locked.store.order.erase(std::remove(locked.store.order.begin(),
                                       locked.store.order.end(), user_id),
                           locked.store.order.end());
  locked.store.last_launched.reset();
  if (locked.active.has_value() && !locked.active->guest &&
      locked.active->user_id == user_id) {
    ActiveAccountPointer replacement;
    for (const std::int64_t remaining : locked.store.order) {
      if (locked.HasAccountDirectory(remaining)) {
        replacement.guest = false;
        replacement.user_id = remaining;
        break;
      }
    }
    if (!locked.Commit(error) || !locked.SetActive(replacement, error)) {
      return false;
    }
    return true;
  }
  return locked.Commit(error);
}

bool AccountStore::ClearSessionArtifacts(std::string* error) const {
  std::string local_error;
  if (error == nullptr) {
    error = &local_error;
  }
  if (!options_.assume_exclusive) {
    *error = kNotExclusive;
    return false;
  }
  return RemoveHydrationKeys(options_.app_storage_file, error) &&
         DeleteCookieJar(options_.webview_data_directory, error);
}

ProfileRefreshResult AccountStore::RefreshProfiles(
    services::HttpClient& http_client, bool force) const {
  ProfileRefreshResult result;
  if (!options_.assume_exclusive) {
    result.error = kNotExclusive;
    return result;
  }
  AccountStoreSnapshot snapshot;
  if (!Load(&snapshot, &result.error)) {
    return result;
  }
  const std::int64_t now = Now();
  std::vector<std::int64_t> due;
  for (const SavedAccount& account : snapshot.accounts) {
    if (force || account.avatar_file.empty() ||
        now - account.profile_refreshed_at >= kProfileRefreshIntervalSeconds ||
        now < account.profile_refreshed_at) {
      due.push_back(account.user_id);
    }
  }
  if (due.empty()) {
    return result;
  }

  struct Fetched {
    std::int64_t user_id = 0;
    bool names = false;
    std::string username;
    std::string display_name;
    bool avatar = false;
  };
  std::vector<Fetched> fetched;
  for (const std::int64_t user_id : due) {
    Fetched entry;
    entry.user_id = user_id;
    entry.names = FetchPublicNames(http_client, user_id, &entry.username,
                                   &entry.display_name);
    fetched.push_back(std::move(entry));
  }
  std::string avatar_error;
  const ScopedFd avatars =
      OpenAvatarDirectory(options_.avatar_directory, true, &avatar_error);
  for (std::size_t first = 0; avatars.valid() && first < due.size();
       first += kThumbnailBatchSize) {
    const std::vector<std::int64_t> batch(
        due.begin() + static_cast<std::ptrdiff_t>(first),
        due.begin() + static_cast<std::ptrdiff_t>(
                          std::min(due.size(), first + kThumbnailBatchSize)));
    for (const auto& [user_id, url] : FetchAvatarUrls(http_client, batch)) {
      services::HttpRequest request = PublicRequest(url, kMaximumAvatarBytes);
      request.headers = {"Accept: image/png"};
      const services::HttpResponse image = http_client.Get(request);
      if (!image.transport_ok || image.status_code != 200 ||
          image.body.size() < kPngSignature.size() ||
          !std::equal(kPngSignature.begin(), kPngSignature.end(),
                      image.body.begin(),
                      [](unsigned char expected, char actual) {
                        return expected == static_cast<unsigned char>(actual);
                      })) {
        continue;
      }
      if (WritePrivateFileAt(avatars.get(), AvatarName(user_id), image.body)) {
        for (Fetched& entry : fetched) {
          if (entry.user_id == user_id) {
            entry.avatar = true;
          }
        }
      }
    }
  }

  LockedStore locked;
  if (!locked.Open(options_.auth_root, false, &result.error)) {
    return result;
  }
  for (const Fetched& entry : fetched) {
    result.names_updated += entry.names ? 1 : 0;
    result.avatars_updated += entry.avatar ? 1 : 0;
    result.failed += entry.names && entry.avatar ? 0 : 1;
    if (!entry.names && !entry.avatar) {
      continue;
    }
    const ScopedFd directory(
        openat(locked.accounts.get(), AccountName(entry.user_id).c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.valid()) {
      continue;
    }
    AccountRecord record = ReadAccountRecordAt(directory.get(), entry.user_id);
    if (entry.names) {
      record.account.username = entry.username;
      record.account.display_name = entry.display_name;
    }
    if (entry.names && entry.avatar) {
      record.account.profile_refreshed_at = now;
    }
    (void)WriteAccountRecordAt(directory.get(), record);
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
