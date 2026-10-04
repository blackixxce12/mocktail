#ifndef MOCKTAIL_LAUNCHER_FILE_IDENTITY_H_
#define MOCKTAIL_LAUNCHER_FILE_IDENTITY_H_

#include <sys/types.h>

#include <ctime>

namespace mocktail::launcher {

// What a file looked like when an editor read it. Saving compares the file on
// disk against this record and refuses to overwrite a file that someone else
// changed (or created, when exists is false) in the meantime.
struct FileIdentity {
  dev_t dev = 0;
  ino_t ino = 0;
  off_t size = 0;
  timespec mtime = {};
  bool exists = false;
};

bool SameFileIdentity(const FileIdentity& left, const FileIdentity& right);

}  // namespace mocktail::launcher

#endif  // MOCKTAIL_LAUNCHER_FILE_IDENTITY_H_
