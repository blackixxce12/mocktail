#ifndef MOCKTAIL_LAUNCHER_DESKTOP_ENTRY_CLEANUP_H_
#define MOCKTAIL_LAUNCHER_DESKTOP_ENTRY_CLEANUP_H_

#include <filesystem>
#include <string>
#include <vector>

#include "launcher/file_identity.h"
#include "runtime/environment.h"

namespace mocktail::launcher {

// Users worked around missing settings with a copy of Mocktail's desktop
// entry in $XDG_DATA_HOME/applications whose Exec is
// "env VAR=VALUE ... mocktail %u" (the reporter's sets SDL_VIDEODRIVER).
// Those variables override config.yaml on every start, including website
// joins, because the entry is also the roblox: URL handler. This finds such
// an entry and plans to remove the variables that override a setting the
// settings window edits; others (a PRIME GPU switch, LD_PRELOAD, MANGOHUD)
// stay. Nothing is changed until ApplyDesktopEntryCleanup() is called.

inline constexpr char kDesktopEntryFileName[] = "space.bigrat.mocktail.desktop";

struct DesktopEntryPaths {
  std::filesystem::path user_file;
  // Candidates for the packaged entry, in XDG_DATA_DIRS order.
  std::vector<std::filesystem::path> system_files;
};

// user_file: $XDG_DATA_HOME (if absolute, else $HOME/.local/share) +
// /applications/space.bigrat.mocktail.desktop; system_files: the same name
// under every absolute $XDG_DATA_DIRS entry (default /usr/local/share and
// /usr/share).
DesktopEntryPaths DefaultDesktopEntryPaths(
    const runtime::Environment& environment);

struct EnvAssignment {
  std::string name;
  std::string value;
  // Overrides a setting the settings window edits
  // (runtime::FindManagedEnvironmentVariable). Only these are removed.
  bool managed = false;
  // The value may hold a secret: a Roblox session or the file holding one,
  // a credential-like name, or a URL with a user name and password (a
  // proxy). Show it redacted.
  bool sensitive = false;
};

enum class DesktopEntryCleanupPlan {
  kNone,
  // The entry adds nothing to the packaged one apart from managed
  // variables: remove it so the packaged entry applies again.
  kDeleteFile,
  // Keep the user's entry but drop the managed VAR=VALUE assignments from
  // Exec, and "env" with them when no other assignment is left.
  kRewriteExec,
};

struct DesktopEntryInspection {
  bool found = false;
  std::filesystem::path path;
  std::filesystem::path system_path;  // empty when no packaged entry exists
  std::string exec;                   // the Exec value, unescaped
  // Every assignment, in Exec order, managed or not.
  std::vector<EnvAssignment> env_assignments;
  DesktopEntryCleanupPlan plan = DesktopEntryCleanupPlan::kNone;
  // Exec after the cleanup; the assignments it keeps are quoted as before.
  std::string new_exec;
  // Why there is no plan although an entry was found (symlink, unsupported
  // Exec, ...). Empty when the entry sets no managed variable.
  std::string note;

  // What was inspected, so Apply can refuse a file changed in between.
  FileIdentity identity;
  unsigned int mode = 0644;
  std::string bytes;
  std::string rewritten_bytes;  // kRewriteExec: the new file content
};

// Reads (never writes) the user's entry and the first packaged entry that
// exists. Returns false only when the user's entry exists but cannot be
// read; a missing entry yields found == false.
bool InspectDesktopEntry(const DesktopEntryPaths& paths,
                         DesktopEntryInspection* inspection,
                         std::string* error);

struct DesktopEntryApplyOptions {
  // Run "<database_tool> <applications dir>" afterwards, best-effort.
  bool refresh_database = true;
  std::string database_tool = "update-desktop-database";
};

struct DesktopEntryApplyResult {
  std::filesystem::path backup_path;
  bool database_refreshed = false;
  std::string database_warning;
};

// Carries out inspection.plan. First keeps a copy of the entry at
// <file>.mocktail-backup (mode 0600; .mocktail-backup.1 ... when one exists),
// refuses if the file changed since it was inspected, then deletes it or
// replaces it atomically (keeping its mode). Only the Exec line changes in
// a rewrite; comments and every other line stay byte-identical.
bool ApplyDesktopEntryCleanup(const DesktopEntryInspection& inspection,
                              const DesktopEntryApplyOptions& options,
                              DesktopEntryApplyResult* result,
                              std::string* error);

}  // namespace mocktail::launcher

#endif  // MOCKTAIL_LAUNCHER_DESKTOP_ENTRY_CLEANUP_H_
