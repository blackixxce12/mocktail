#include "private_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace mocktail::launcher {

bool SameFileIdentity(const FileIdentity& left, const FileIdentity& right) {
  if (left.exists != right.exists) {
    return false;
  }
  if (!left.exists) {
    return true;
  }
  return left.dev == right.dev && left.ino == right.ino &&
         left.size == right.size &&
         left.mtime.tv_sec == right.mtime.tv_sec &&
         left.mtime.tv_nsec == right.mtime.tv_nsec;
}

namespace internal {
namespace {

class ScopedDescriptor final {
 public:
  explicit ScopedDescriptor(int descriptor) : descriptor_(descriptor) {}
  ~ScopedDescriptor() { Reset(); }

  ScopedDescriptor(const ScopedDescriptor&) = delete;
  ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

  int get() const { return descriptor_; }
  bool Close() {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor < 0 || close(descriptor) == 0;
  }
  void Reset() {
    if (descriptor_ >= 0) {
      (void)close(descriptor_);
      descriptor_ = -1;
    }
  }

 private:
  int descriptor_;
};

std::string Describe(std::string_view action, const std::filesystem::path& path,
                     int error_number) {
  return std::string(action) + " " + path.string() + ": " +
         std::strerror(error_number);
}

FileIdentity IdentityFromStat(const struct stat& metadata) {
  FileIdentity identity;
  identity.dev = metadata.st_dev;
  identity.ino = metadata.st_ino;
  identity.size = metadata.st_size;
  identity.mtime = metadata.st_mtim;
  identity.exists = true;
  return identity;
}

bool WriteAll(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

std::string ChangedOnDisk(const std::filesystem::path& path) {
  return path.string() +
         " changed on disk after it was read; reload it before saving";
}

}  // namespace

bool ReadRegularFile(const std::filesystem::path& path, std::size_t max_bytes,
                     std::string* bytes, FileIdentity* identity,
                     std::string* error) {
  bytes->clear();
  *identity = FileIdentity();
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return true;
    }
    *error = errno == ELOOP ? "refusing to follow the symlink " + path.string()
                            : Describe("cannot open", path, errno);
    return false;
  }
  ScopedDescriptor file(descriptor);
  struct stat metadata = {};
  if (fstat(file.get(), &metadata) != 0) {
    *error = Describe("cannot inspect", path, errno);
    return false;
  }
  if (!S_ISREG(metadata.st_mode)) {
    *error = path.string() + " is not a regular file";
    return false;
  }
  if (metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > max_bytes) {
    *error = path.string() + " is larger than " +
             std::to_string(max_bytes / 1024U) + " KiB";
    return false;
  }
  bytes->reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 16U * 1024U> buffer = {};
  while (true) {
    const ssize_t count = read(file.get(), buffer.data(), buffer.size());
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = Describe("cannot read", path, errno);
      return false;
    }
    if (bytes->size() + static_cast<std::size_t>(count) > max_bytes) {
      *error = path.string() + " is larger than " +
               std::to_string(max_bytes / 1024U) + " KiB";
      return false;
    }
    bytes->append(buffer.data(), static_cast<std::size_t>(count));
  }
  *identity = IdentityFromStat(metadata);
  return true;
}

bool CurrentFileIdentity(const std::filesystem::path& path,
                         FileIdentity* identity, std::string* error) {
  *identity = FileIdentity();
  struct stat metadata = {};
  if (lstat(path.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    *error = Describe("cannot inspect", path, errno);
    return false;
  }
  if (S_ISLNK(metadata.st_mode)) {
    *error = "refusing to replace the symlink " + path.string();
    return false;
  }
  if (!S_ISREG(metadata.st_mode)) {
    *error = path.string() + " is not a regular file";
    return false;
  }
  *identity = IdentityFromStat(metadata);
  return true;
}

bool VerifyUnchanged(const std::filesystem::path& path,
                     const FileIdentity& expected,
                     std::string_view expected_bytes, std::size_t max_bytes,
                     std::string* error) {
  FileIdentity current;
  if (!CurrentFileIdentity(path, &current, error)) {
    return false;
  }
  if (!SameFileIdentity(current, expected)) {
    *error = ChangedOnDisk(path);
    return false;
  }
  if (!expected.exists) {
    return true;
  }
  // The modification time can be too coarse to notice a quick rewrite of the
  // same size, so the content is compared as well.
  std::string bytes;
  FileIdentity read_identity;
  std::string read_error;
  if (!ReadRegularFile(path, max_bytes, &bytes, &read_identity, &read_error) ||
      !SameFileIdentity(read_identity, expected) || bytes != expected_bytes) {
    *error = ChangedOnDisk(path);
    return false;
  }
  return true;
}

bool SyncDirectory(const std::filesystem::path& directory,
                   std::string* error) {
  const int descriptor =
      open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (descriptor < 0) {
    *error = Describe("cannot open directory", directory, errno);
    return false;
  }
  ScopedDescriptor handle(descriptor);
  if (fsync(handle.get()) != 0) {
    *error = Describe("cannot synchronize directory", directory, errno);
    return false;
  }
  return true;
}

bool PublishAtomically(const std::filesystem::path& path,
                       std::string_view bytes, mode_t mode,
                       const FileIdentity& expected, FileIdentity* published,
                       std::string* error) {
  if (path.filename().empty()) {
    *error = "the destination must name a file";
    return false;
  }
  const std::filesystem::path parent =
      path.parent_path().empty() ? std::filesystem::path(".")
                                 : path.parent_path();
  const std::string pattern =
      (parent / ("." + path.filename().string() + ".tmp-XXXXXX")).string();
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const int descriptor = mkostemp(temporary.data(), O_CLOEXEC);
  if (descriptor < 0) {
    *error = Describe("cannot create a temporary file next to", path, errno);
    return false;
  }
  ScopedDescriptor file(descriptor);
  const auto discard = [&temporary]() { (void)unlink(temporary.data()); };

  struct stat metadata = {};
  if (fchmod(file.get(), mode) != 0 || !WriteAll(file.get(), bytes) ||
      fsync(file.get()) != 0 || fstat(file.get(), &metadata) != 0) {
    const int saved_errno = errno;
    file.Reset();
    discard();
    *error = Describe("cannot write a temporary file next to", path,
                      saved_errno);
    return false;
  }
  if (!file.Close()) {
    const int saved_errno = errno;
    discard();
    *error = Describe("cannot close a temporary file next to", path,
                      saved_errno);
    return false;
  }

  FileIdentity current;
  if (!CurrentFileIdentity(path, &current, error)) {
    discard();
    return false;
  }
  if (!SameFileIdentity(current, expected)) {
    discard();
    *error = ChangedOnDisk(path);
    return false;
  }
  if (expected.exists) {
    if (rename(temporary.data(), path.c_str()) != 0) {
      const int saved_errno = errno;
      discard();
      *error = Describe("cannot replace", path, saved_errno);
      return false;
    }
  } else {
    if (link(temporary.data(), path.c_str()) != 0) {
      const int saved_errno = errno;
      discard();
      *error = saved_errno == EEXIST ? ChangedOnDisk(path)
                                     : Describe("cannot create", path,
                                                saved_errno);
      return false;
    }
    discard();
  }
  *published = IdentityFromStat(metadata);
  return SyncDirectory(parent, error);
}

bool CreateExclusiveFile(const std::filesystem::path& path,
                         std::string_view bytes, mode_t mode, bool* created,
                         std::string* error) {
  *created = false;
  const int descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
           mode);
  if (descriptor < 0) {
    if (errno == EEXIST) {
      return true;
    }
    *error = Describe("cannot create", path, errno);
    return false;
  }
  ScopedDescriptor file(descriptor);
  if (fchmod(file.get(), mode) != 0 || !WriteAll(file.get(), bytes) ||
      fsync(file.get()) != 0 || !file.Close()) {
    const int saved_errno = errno;
    file.Reset();
    (void)unlink(path.c_str());
    *error = Describe("cannot write", path, saved_errno);
    return false;
  }
  *created = true;
  return true;
}

bool KeepCopyBeside(const std::filesystem::path& file, std::string_view label,
                    std::string_view bytes, std::filesystem::path* kept,
                    std::string* error) {
  constexpr mode_t kPrivateMode = S_IRUSR | S_IWUSR;
  // Enough for a few saves within one second.
  constexpr int kMaximumAttempts = 100;
  std::array<char, 32> stamp{};
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  if (localtime_r(&now, &local) == nullptr ||
      std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local) ==
          0) {
    *error = "cannot read the clock to name a copy of " + file.string();
    return false;
  }
  const std::string base =
      file.string() + "." + std::string(label) + "-" + stamp.data();
  for (int attempt = 1; attempt <= kMaximumAttempts; ++attempt) {
    const std::filesystem::path candidate =
        attempt == 1 ? base : base + "-" + std::to_string(attempt);
    bool created = false;
    if (!CreateExclusiveFile(candidate, bytes, kPrivateMode, &created,
                             error)) {
      return false;
    }
    if (created) {
      *kept = candidate;
      return SyncDirectory(
          file.has_parent_path() ? file.parent_path() : ".", error);
    }
  }
  *error = "cannot find a free name for a copy of " + file.string();
  return false;
}

}  // namespace internal
}  // namespace mocktail::launcher
