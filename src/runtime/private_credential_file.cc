#include "runtime/private_credential_file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/runtime_paths.h"
#include "services/auth_service.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr uintmax_t kMaximumCookieFileBytes =
    kMaximumPrivateCredentialFileBytes;

void ClearSensitiveString(std::string* value);

class ScopedFileDescriptor final {
 public:
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  int get() const { return descriptor_; }

 private:
  int descriptor_ = -1;
};

bool WriteAll(int descriptor, const char* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t result = write(descriptor, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

bool WriteAllAt(int descriptor, const char* data, size_t size, off_t offset) {
  size_t written = 0;
  while (written < size) {
    const ssize_t result = pwrite(descriptor, data + written, size - written,
                                  offset + static_cast<off_t>(written));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

int OpenCookieWriterLock(int directory_descriptor, std::string_view filename) {
  const std::string lock_name =
      "." + std::string(filename) + ".mocktail-writer.lock";
  const int lock_descriptor =
      openat(directory_descriptor, lock_name.c_str(),
             O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (lock_descriptor < 0) {
    return -1;
  }
  struct stat metadata = {};
  if (fstat(lock_descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
      fchmod(lock_descriptor, S_IRUSR | S_IWUSR) != 0 ||
      flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
    close(lock_descriptor);
    return -1;
  }
  return lock_descriptor;
}

bool FindExistingDirectoryAncestor(const std::filesystem::path& directory,
                                   std::filesystem::path* ancestor) {
  if (directory.empty() || ancestor == nullptr) {
    return false;
  }
  std::filesystem::path candidate = directory;
  while (true) {
    struct stat metadata = {};
    if (lstat(candidate.c_str(), &metadata) == 0) {
      if (!S_ISDIR(metadata.st_mode) || S_ISLNK(metadata.st_mode)) {
        return false;
      }
      *ancestor = candidate;
      return true;
    }
    if (errno != ENOENT) {
      return false;
    }
    std::filesystem::path parent = candidate.parent_path();
    if (parent.empty() && candidate != ".") {
      parent = ".";
    }
    if (parent.empty() || parent == candidate) {
      return false;
    }
    candidate = parent;
  }
}

bool FsyncDirectoryChain(const std::filesystem::path& first,
                         const std::filesystem::path& last) {
  std::filesystem::path current = first;
  while (true) {
    const int descriptor =
        open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
      return false;
    }
    const ScopedFileDescriptor directory(descriptor);
    struct stat metadata = {};
    if (fstat(directory.get(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        fsync(directory.get()) != 0) {
      return false;
    }
    if (current == last) {
      return true;
    }
    std::filesystem::path parent = current.parent_path();
    if (parent.empty() && current != ".") {
      parent = ".";
    }
    if (parent.empty() || parent == current) {
      return false;
    }
    current = parent;
  }
}

bool SameFile(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool SameFileVersion(const struct stat& left, const struct stat& right) {
  return SameFile(left, right) && left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

bool ReadDescriptorContents(int descriptor, std::string* contents) {
  if (contents == nullptr || lseek(descriptor, 0, SEEK_SET) < 0) {
    return false;
  }
  ClearSensitiveString(contents);
  std::array<char, 4096> buffer = {};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fill(buffer.begin(), buffer.end(), '\0');
      ClearSensitiveString(contents);
      return false;
    }
    const size_t byte_count = static_cast<size_t>(bytes);
    if (contents->size() > kMaximumCookieFileBytes - byte_count) {
      std::fill(buffer.begin(), buffer.end(), '\0');
      ClearSensitiveString(contents);
      return false;
    }
    contents->append(buffer.data(), byte_count);
  }
  std::fill(buffer.begin(), buffer.end(), '\0');
  return true;
}

bool IsCookieFileDelimiter(char character) {
  return character == ';' || character == '\r' || character == '\n' ||
         character == '\t';
}

struct CookieRedactionRange {
  size_t begin = 0;
  size_t end = 0;
  size_t marker = 0;
};

std::vector<CookieRedactionRange> FindCookieRedactionRanges(
    std::string_view original, std::string_view redacted) {
  std::vector<CookieRedactionRange> ranges;
  if (original.size() != redacted.size()) {
    return ranges;
  }
  size_t segment_begin = 0;
  while (segment_begin < original.size()) {
    size_t segment_end = segment_begin;
    while (segment_end < original.size() &&
           !IsCookieFileDelimiter(original[segment_end])) {
      ++segment_end;
    }
    const size_t length = segment_end - segment_begin;
    if (original.substr(segment_begin, length) !=
        redacted.substr(segment_begin, length)) {
      size_t marker = segment_begin;
      while (marker < segment_end && original[marker] == redacted[marker]) {
        ++marker;
      }
      if (marker == segment_end) {
        return {};
      }
      ranges.push_back({segment_begin, segment_end, marker});
    }
    segment_begin = segment_end + (segment_end < original.size() ? 1 : 0);
  }
  return ranges;
}

void ClearSensitiveString(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* byte = value->empty() ? nullptr : value->data();
  for (size_t index = 0; index < value->size(); ++index) {
    byte[index] = '\0';
  }
  value->clear();
}

}  // namespace

void SecurelyClearString(std::string* value) { ClearSensitiveString(value); }

int OpenPrivateFileWriterLock(int directory_descriptor,
                              std::string_view filename) {
  return OpenCookieWriterLock(directory_descriptor, filename);
}

bool WritePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view contents) {
  if (path.empty() || contents.empty() ||
      contents.size() > kMaximumCookieFileBytes) {
    return false;
  }
  std::error_code error;
  const std::filesystem::path resolved_path =
      std::filesystem::absolute(path, error).lexically_normal();
  if (error || resolved_path.empty()) {
    return false;
  }
  const std::filesystem::path parent = resolved_path.parent_path();
  std::filesystem::path existing_ancestor;
  if (!FindExistingDirectoryAncestor(parent, &existing_ancestor)) {
    return false;
  }
  const bool parent_existed = existing_ancestor == parent;
  if (!RuntimePaths::EnsureDirectory(parent, &error) || error) {
    return false;
  }
  struct stat parent_status = {};
  if (lstat(parent.c_str(), &parent_status) != 0 ||
      !S_ISDIR(parent_status.st_mode) || S_ISLNK(parent_status.st_mode) ||
      parent_status.st_uid != geteuid() ||
      chmod(parent.c_str(), S_IRWXU) != 0) {
    return false;
  }
  const std::string filename = resolved_path.filename().string();
  if (filename.empty() || filename == "." || filename == "..") {
    return false;
  }
  const int directory_descriptor =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_descriptor < 0) {
    return false;
  }
  const ScopedFileDescriptor directory(directory_descriptor);
  struct stat opened_parent_status = {};
  if (fstat(directory.get(), &opened_parent_status) != 0 ||
      opened_parent_status.st_dev != parent_status.st_dev ||
      opened_parent_status.st_ino != parent_status.st_ino ||
      !S_ISDIR(opened_parent_status.st_mode) ||
      opened_parent_status.st_uid != geteuid()) {
    return false;
  }
  const int lock_descriptor = OpenCookieWriterLock(directory.get(), filename);
  if (lock_descriptor < 0) {
    return false;
  }
  const ScopedFileDescriptor lock(lock_descriptor);

  struct stat destination_status = {};
  const int destination_result =
      fstatat(directory.get(), filename.c_str(), &destination_status,
              AT_SYMLINK_NOFOLLOW);
  if (destination_result == 0) {
    if (!S_ISREG(destination_status.st_mode) ||
        S_ISLNK(destination_status.st_mode) ||
        destination_status.st_uid != geteuid() ||
        destination_status.st_nlink != 1) {
      return false;
    }
  } else if (errno != ENOENT) {
    return false;
  }

  static std::atomic<uint64_t> serial{0};
  const std::string temporary = "." + filename + ".tmp-" +
                                std::to_string(getpid()) + "-" +
                                std::to_string(serial.fetch_add(1));
  const int descriptor = openat(
      directory.get(), temporary.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return false;
  }
  const ScopedFileDescriptor file(descriptor);
  const bool stored = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
                      WriteAll(file.get(), contents.data(), contents.size()) &&
                      fsync(file.get()) == 0;
  if (!stored || renameat(directory.get(), temporary.c_str(), directory.get(),
                          filename.c_str()) != 0) {
    (void)unlinkat(directory.get(), temporary.c_str(), 0);
    return false;
  }
  if (fsync(directory.get()) != 0) {
    return false;
  }
  const std::filesystem::path first_created_parent =
      parent.parent_path().empty() ? std::filesystem::path(".")
                                   : parent.parent_path();
  return parent_existed ||
         FsyncDirectoryChain(first_created_parent, existing_ancestor);
}

PrivateCredentialFileReadResult ReadPrivateCredentialFile(
    const std::filesystem::path& path, bool missing_is_unavailable) {
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT && !missing_is_unavailable) {
      return {};
    }
    return {PrivateCredentialFileStatus::kUnavailable,
            {},
            "configured Roblox cookie file is unavailable"};
  }
  const ScopedFileDescriptor file(descriptor);

  struct stat metadata = {};
  if (fstat(file.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid()) {
    return {PrivateCredentialFileStatus::kUnavailable,
            {},
            "Roblox cookie source is not a regular file"};
  }
  if ((metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return {PrivateCredentialFileStatus::kUnavailable,
            {},
            "Roblox cookie file permissions are not private"};
  }
  if (metadata.st_size < 0 ||
      static_cast<uintmax_t>(metadata.st_size) > kMaximumCookieFileBytes) {
    return {PrivateCredentialFileStatus::kUnavailable,
            {},
            "Roblox cookie file exceeds the safe read limit"};
  }

  std::string value;
  value.reserve(static_cast<size_t>(metadata.st_size));
  std::array<char, 4096> buffer = {};
  while (true) {
    const ssize_t bytes = read(file.get(), buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fill(buffer.begin(), buffer.end(), '\0');
      ClearSensitiveString(&value);
      return {PrivateCredentialFileStatus::kUnavailable,
              {},
              "Roblox cookie file could not be read"};
    }
    const size_t byte_count = static_cast<size_t>(bytes);
    if (value.size() > kMaximumCookieFileBytes - byte_count) {
      std::fill(buffer.begin(), buffer.end(), '\0');
      ClearSensitiveString(&value);
      return {PrivateCredentialFileStatus::kUnavailable,
              {},
              "Roblox cookie file exceeds the safe read limit"};
    }
    value.append(buffer.data(), byte_count);
  }
  std::fill(buffer.begin(), buffer.end(), '\0');
  struct stat verification_metadata = {};
  if (fstat(file.get(), &verification_metadata) != 0 ||
      !SameFileVersion(metadata, verification_metadata) ||
      static_cast<uintmax_t>(verification_metadata.st_size) != value.size()) {
    ClearSensitiveString(&value);
    return {PrivateCredentialFileStatus::kUnavailable,
            {},
            "Roblox cookie file changed while it was read"};
  }
  if (value.empty()) {
    if (missing_is_unavailable) {
      return {PrivateCredentialFileStatus::kUnavailable,
              {},
              "configured Roblox cookie file is empty"};
    }
    return {};
  }
  return {PrivateCredentialFileStatus::kFound, std::move(value), {}};
}

bool ClearRejectedCookieFile(const std::filesystem::path& path,
                             std::string_view loaded_contents,
                             std::string* error) {
  const std::filesystem::path parent = path.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path.parent_path();
  const std::string filename = path.filename().string();
  if (filename.empty() || filename == "." || filename == "..") {
    if (error != nullptr) {
      *error = "rejected Roblox cookie path is invalid";
    }
    return false;
  }

  const int directory_descriptor =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_descriptor < 0) {
    if (error != nullptr) {
      *error = "rejected Roblox cookie could not be cleared safely";
    }
    return false;
  }
  const ScopedFileDescriptor directory(directory_descriptor);
  const int lock_descriptor = OpenCookieWriterLock(directory.get(), filename);
  if (lock_descriptor < 0) {
    if (error != nullptr) {
      *error = "rejected Roblox cookie could not be cleared safely";
    }
    return false;
  }
  const ScopedFileDescriptor lock(lock_descriptor);

  std::string current_contents;
  std::string redacted_contents;
  std::string verification_contents;
  std::string rejected_value;
  const auto clear_buffers = [&]() {
    ClearSensitiveString(&current_contents);
    ClearSensitiveString(&redacted_contents);
    ClearSensitiveString(&verification_contents);
    ClearSensitiveString(&rejected_value);
  };
  const auto fail = [&](std::string message) {
    clear_buffers();
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };

  rejected_value =
      services::AuthService::ExtractRoblosecurityValue(loaded_contents);
  if (!services::AuthService::RedactRejectedRoblosecurity(
          loaded_contents, rejected_value, &redacted_contents)) {
    return fail("rejected Roblox cookie could not be identified safely");
  }
  const std::vector<CookieRedactionRange> ranges =
      FindCookieRedactionRanges(loaded_contents, redacted_contents);
  if (ranges.empty()) {
    return fail("rejected Roblox cookie could not be identified safely");
  }

  const auto source_metadata_error = [&](const struct stat& metadata) {
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 || metadata.st_size < 0 ||
        static_cast<uintmax_t>(metadata.st_size) > kMaximumCookieFileBytes) {
      return std::string(
          "Roblox cookie changed while authentication was "
          "checked");
    }
    if (metadata.st_nlink != 1) {
      return std::string("managed Roblox cookie has unexpected hard links");
    }
    return std::string();
  };

  int source_descriptor = openat(directory.get(), filename.c_str(),
                                 O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (source_descriptor < 0 && errno == EACCES) {
    const int read_only_descriptor = openat(directory.get(), filename.c_str(),
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (read_only_descriptor < 0) {
      return fail("rejected Roblox cookie could not be cleared safely");
    }
    const ScopedFileDescriptor read_only(read_only_descriptor);
    struct stat read_only_metadata = {};
    if (fstat(read_only.get(), &read_only_metadata) != 0) {
      return fail("Roblox cookie changed while authentication was checked");
    }
    const std::string metadata_error =
        source_metadata_error(read_only_metadata);
    if (!metadata_error.empty()) {
      return fail(metadata_error);
    }
    struct stat read_only_path_metadata = {};
    if (fstatat(directory.get(), filename.c_str(), &read_only_path_metadata,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !SameFile(read_only_path_metadata, read_only_metadata) ||
        fchmod(read_only.get(), S_IRUSR | S_IWUSR) != 0) {
      return fail("Roblox cookie changed while authentication was checked");
    }
    source_descriptor = openat(directory.get(), filename.c_str(),
                               O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  }
  if (source_descriptor < 0) {
    if (errno == ENOENT) {
      clear_buffers();
      return true;
    }
    return fail("rejected Roblox cookie could not be cleared safely");
  }
  const ScopedFileDescriptor source(source_descriptor);
  struct stat source_metadata = {};
  if (fstat(source.get(), &source_metadata) != 0) {
    return fail("Roblox cookie changed while authentication was checked");
  }
  const std::string metadata_error = source_metadata_error(source_metadata);
  if (!metadata_error.empty()) {
    return fail(metadata_error);
  }
  if (flock(source.get(), LOCK_EX | LOCK_NB) != 0 ||
      !ReadDescriptorContents(source.get(), &current_contents) ||
      current_contents != loaded_contents) {
    return fail("Roblox cookie changed while authentication was checked");
  }
  struct stat path_metadata = {};
  if (fstatat(directory.get(), filename.c_str(), &path_metadata,
              AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(path_metadata, source_metadata)) {
    return fail("Roblox cookie changed while authentication was checked");
  }
  struct stat prewrite_metadata = {};
  if (fstat(source.get(), &prewrite_metadata) != 0 ||
      !SameFileVersion(source_metadata, prewrite_metadata) ||
      prewrite_metadata.st_nlink != 1 ||
      fchmod(source.get(), S_IRUSR | S_IWUSR) != 0) {
    return fail("Roblox cookie changed while authentication was checked");
  }

  // Invalidate later duplicates before the effective first segment. A crash
  // before the final marker write therefore leaves the original rejected
  // credential authoritative; after it, no exact security-cookie name remains.
  constexpr char kRedactionMarker = ' ';
  for (size_t index = ranges.size(); index > 1; --index) {
    const CookieRedactionRange& range = ranges[index - 1];
    if (!WriteAllAt(source.get(), &kRedactionMarker, 1,
                    static_cast<off_t>(range.marker))) {
      return fail("rejected Roblox cookie could not be cleared safely");
    }
  }
  if ((ranges.size() > 1 && fsync(source.get()) != 0) ||
      !WriteAllAt(source.get(), &kRedactionMarker, 1,
                  static_cast<off_t>(ranges.front().marker)) ||
      fsync(source.get()) != 0) {
    return fail("rejected Roblox cookie could not be cleared safely");
  }
  for (const CookieRedactionRange& range : ranges) {
    if (!WriteAllAt(source.get(), redacted_contents.data() + range.begin,
                    range.end - range.begin, static_cast<off_t>(range.begin))) {
      return fail("rejected Roblox cookie could not be cleared safely");
    }
  }
  if (fsync(source.get()) != 0 ||
      !ReadDescriptorContents(source.get(), &verification_contents) ||
      verification_contents != redacted_contents) {
    return fail("rejected Roblox cookie could not be cleared safely");
  }
  if (fstatat(directory.get(), filename.c_str(), &path_metadata,
              AT_SYMLINK_NOFOLLOW) != 0 ||
      !SameFile(path_metadata, source_metadata)) {
    return fail("Roblox cookie changed while authentication was checked");
  }
  clear_buffers();
  return true;
}

bool PersistRobloxCookie(const std::filesystem::path& path,
                         std::string_view cookie_value) {
  if (cookie_value.empty()) {
    return false;
  }
  std::string formatted;
  constexpr std::string_view kPrefix = ".ROBLOSECURITY=";
  if (cookie_value.compare(0, kPrefix.size(), kPrefix) != 0) {
    formatted = std::string(kPrefix) + std::string(cookie_value) + "\n";
  } else {
    formatted = std::string(cookie_value) + "\n";
  }
  const bool result = WritePrivateFileAtomically(path, formatted);
  ClearSensitiveString(&formatted);
  return result;
}

RobloxSessionValidation ValidateRobloxSession(
    services::AuthService& auth_service, std::string_view cookie_value) {
  constexpr std::string_view kPrefix = ".ROBLOSECURITY=";
  std::string header;
  if (cookie_value.compare(0, kPrefix.size(), kPrefix) != 0) {
    header = kPrefix;
  }
  header.append(cookie_value);
  services::AuthSession session = auth_service.ResolveSession(header, false);
  ClearSensitiveString(&header);
  RobloxSessionValidation validation;
  validation.http_status = session.http_status;
  switch (session.status) {
    case services::AuthSessionStatus::kAuthenticated:
      validation.status = BrowserSignInStatus::kAccepted;
      validation.identity = std::move(session.identity);
      return validation;
    case services::AuthSessionStatus::kUnavailable:
      validation.status = BrowserSignInStatus::kUnverified;
      return validation;
    case services::AuthSessionStatus::kGuest:
    case services::AuthSessionStatus::kInvalid:
      break;
  }
  validation.status = BrowserSignInStatus::kRejected;
  return validation;
}

BrowserSignInStatus PersistValidatedRobloxCookie(
    const std::filesystem::path& path, services::AuthService& auth_service,
    std::string_view cookie_value) {
  const RobloxSessionValidation validation =
      ValidateRobloxSession(auth_service, cookie_value);
  if (validation.status != BrowserSignInStatus::kAccepted) {
    return validation.status;
  }
  return PersistRobloxCookie(path, cookie_value)
             ? BrowserSignInStatus::kAccepted
             : BrowserSignInStatus::kStoreFailed;
}

}  // namespace runtime
}  // namespace mocktail
