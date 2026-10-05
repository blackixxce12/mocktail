#ifndef MOCKTAIL_LAUNCHER_FAST_FLAGS_DOCUMENT_H_
#define MOCKTAIL_LAUNCHER_FAST_FLAGS_DOCUMENT_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/file_identity.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail::launcher {

enum class FastFlagValueKind { kString, kBoolean, kInteger };

struct FastFlagEntry {
  std::string name;
  FastFlagValueKind kind = FastFlagValueKind::kString;
  // kString: the text; kBoolean: "true" or "false"; kInteger: decimal.
  std::string value;

  // The value Roblox receives (booleans as True/False), as Mocktail passes
  // fflags.json entries on.
  std::string RobloxValue() const;
};

enum class FastFlagConflictEffect {
  // Mocktail's own policy sets a different value and refuses to start
  // ("... policy conflicts with <flag>").
  kBlocksStart,
  // Mocktail always sets its own value; the entry has no effect.
  kOverridden,
};

struct FastFlagConflict {
  std::string name;
  std::string file_value;
  std::string managed_value;
  FastFlagConflictEffect effect = FastFlagConflictEffect::kOverridden;
};

// Edits <config_root>/fflags.json: a flat JSON object of at most 64 KiB whose
// values are strings, booleans or integers. Mocktail adds these entries
// under its own client-settings policy at startup. Entries keep their file
// order; new ones are appended. Saving rewrites the file as indented JSON,
// so comments in it (which the runtime tolerates) are not kept there; a
// file with comments is first kept whole next to it (Save()).
class FastFlagsDocument {
 public:
  static constexpr std::size_t kMaximumBytes = 64U * 1024U;

  // A missing file is an empty document. Content the runtime would reject
  // (not an object, unsupported values, too large) is an error.
  static bool Load(const std::filesystem::path& path, FastFlagsDocument* out,
                   std::string* error);
  static bool FromBytes(std::string_view bytes, FastFlagsDocument* out,
                        std::string* error);

  const std::vector<FastFlagEntry>& entries() const { return entries_; }
  const FastFlagEntry* Find(std::string_view name) const;

  // Names are letters, digits and underscores. A boolean is true or false
  // (any case), an integer is a 64-bit decimal number, a string valid UTF-8.
  bool Set(std::string_view name, FastFlagValueKind kind,
           std::string_view value, std::string* error);
  // Returns false when there was no such entry.
  bool Remove(std::string_view name);

  std::string Serialize() const;

  // Atomic, mode 0600; refuses symlinks, a file that changed (or appeared)
  // since Load(), and content above 64 KiB. When the file on disk has
  // comments, it is first kept as "<path>.with-comments-<YYYYMMDD-HHMMSS>"
  // (mode 0600, never over an existing file), and *kept (when given) names
  // it; otherwise *kept is left empty.
  bool Save(const std::filesystem::path& path, std::string* error,
            std::filesystem::path* kept = nullptr);

  // The file as loaded has comments, which Save() does not write back.
  bool has_comments() const { return has_comments_; }

  // Entries that collide with values Mocktail sets itself for the given
  // settings (frame-rate target, performance preset incl. the FRM quality
  // level, HTTP, crash-report, texture-memory and audio policies). The FRM
  // level follows MOCKTAIL_GRAPHICS_QUALITY in this process's environment,
  // as at startup.
  std::vector<FastFlagConflict> FindManagedConflicts(
      const runtime::FrameRatePolicy& frame_rate,
      const runtime::PerformancePolicy& performance) const;

  // True for flags some Mocktail setting manages, whatever the current
  // settings are; worth a warning next to such an entry.
  static bool IsManagedFlag(std::string_view name);

  FileIdentity identity() const { return identity_; }
  bool HasUnsavedChanges() const;

 private:
  std::vector<FastFlagEntry> entries_;
  std::vector<FastFlagEntry> saved_entries_;
  std::string disk_bytes_;
  FileIdentity identity_;
  bool has_comments_ = false;
};

}  // namespace mocktail::launcher

#endif  // MOCKTAIL_LAUNCHER_FAST_FLAGS_DOCUMENT_H_
