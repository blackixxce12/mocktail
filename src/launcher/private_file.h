#ifndef MOCKTAIL_LAUNCHER_PRIVATE_FILE_H_
#define MOCKTAIL_LAUNCHER_PRIVATE_FILE_H_

#include <sys/types.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "launcher/file_identity.h"

namespace mocktail::launcher::internal {

// Reads a regular file without following a final symlink. A missing file is
// not an error: *exists is false and *bytes is empty. A symlink, any other
// non-regular entry, or a file above max_bytes is refused.
bool ReadRegularFile(const std::filesystem::path& path, std::size_t max_bytes,
                     std::string* bytes, FileIdentity* identity,
                     std::string* error);

// lstat()-based identity of whatever is at path now. A symlink or other
// non-regular entry is reported as an error.
bool CurrentFileIdentity(const std::filesystem::path& path,
                         FileIdentity* identity, std::string* error);

// Refuses with a "changed on disk" error unless the file at path still has
// the expected identity and, when it exists, still holds expected_bytes.
bool VerifyUnchanged(const std::filesystem::path& path,
                     const FileIdentity& expected,
                     std::string_view expected_bytes, std::size_t max_bytes,
                     std::string* error);

// Writes bytes to a same-directory temporary file with the given mode,
// fsyncs it, re-checks that path still has the expected identity, and then
// publishes it: rename() over an existing file, or link() when the file did
// not exist (so a file created meanwhile is never clobbered). The directory is
// fsynced afterwards. *published receives the new file's identity.
bool PublishAtomically(const std::filesystem::path& path,
                       std::string_view bytes, mode_t mode,
                       const FileIdentity& expected, FileIdentity* published,
                       std::string* error);

// Creates path exclusively (O_EXCL | O_NOFOLLOW) with mode, writes and fsyncs
// bytes. *created is false, without an error, when an entry already exists.
bool CreateExclusiveFile(const std::filesystem::path& path,
                         std::string_view bytes, mode_t mode, bool* created,
                         std::string* error);

bool SyncDirectory(const std::filesystem::path& directory, std::string* error);

}  // namespace mocktail::launcher::internal

#endif  // MOCKTAIL_LAUNCHER_PRIVATE_FILE_H_
