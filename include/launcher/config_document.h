#ifndef MOCKTAIL_LAUNCHER_CONFIG_DOCUMENT_H_
#define MOCKTAIL_LAUNCHER_CONFIG_DOCUMENT_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "launcher/file_identity.h"

namespace mocktail::launcher {

// How Set() writes a value. kString is always double-quoted (with YAML
// escapes), so free text such as a window title, an audio device name or
// "{place_name}" round-trips exactly. kBool is true/false, kInteger a decimal
// number, and kEnum a plain lowercase-style token such as direct-vulkan.
enum class ScalarKind { kBool, kInteger, kEnum, kString };

// Edits config.yaml in place without losing the user's comments, key order or
// layout. Mocktail never rewrites the file itself, and users keep notes in it,
// so every change is a targeted edit of the lines that hold the value:
//   - an existing single-line value is replaced; a trailing "# comment" stays;
//   - a missing key whose commented example ("# key: value") sits inside the
//     same section is uncommented there;
//   - otherwise the key is inserted after the section's last value, indented
//     like its siblings;
//   - a missing section (or nested block such as integrations.fleasion) is
//     copied from the first-run template with its comments, at the template's
//     position;
//   - a section header is never left without values.
// Every edit is re-parsed and checked: the edited key must read back as the
// new value and every other value must be unchanged, otherwise the edit is
// undone and Set() fails.
//
// Paths are dotted, e.g. "graphics.vsync" or
// "integrations.discord_rpc.join.enabled".
class ConfigDocument {
 public:
  ConfigDocument();

  // Reads path (regular file, no symlink, at most 1 MiB). A missing file
  // yields the first-run template, which Save() then creates. The YAML is not
  // required to be valid; Validate() reports problems.
  static bool Load(const std::filesystem::path& path, ConfigDocument* out,
                   std::string* error);
  static ConfigDocument FromBytes(std::string bytes);

  // Where Save() keeps its one-time copy of the original file.
  static std::filesystem::path BackupPath(
      const std::filesystem::path& config_file);

  // The text sections are copied from. Defaults to DefaultRuntimeConfigYaml().
  void SetTemplate(std::string template_yaml);

  // Replaces the whole text (restore a backup, reset to defaults) while
  // keeping the identity of the loaded file, so Save() still detects
  // concurrent changes.
  void ReplaceAll(std::string bytes);

  // The scalar at path as the loader sees it (quotes and escapes removed).
  // nullopt when the key is absent, commented out, not a scalar, or the YAML
  // does not parse.
  std::optional<std::string> Get(std::string_view dotted) const;
  bool IsMapping(std::string_view dotted) const;

  // Setting the value a key already has (as the loader reads it) changes
  // nothing, so unchanged rows keep their original quoting. Refuses to
  // replace a mapping (for example a detailed device: block) with a scalar
  // unless force_scalar is true; the old mapping lines are then kept as
  // comments.
  bool Set(std::string_view dotted, std::string_view value, ScalarKind kind,
           std::string* error, bool force_scalar = false);

  // Comments the key out ("# key: value"). A section left without values is
  // commented out as well. Removing an absent key succeeds.
  bool Unset(std::string_view dotted, std::string* error);

  // Runs the bytes through Mocktail's real runtime loader (with an empty
  // environment) and the updater's updates: parser. The error keeps the
  // loader's message; YAML syntax errors and errors about one key carry the
  // line number.
  bool Validate(std::string* error) const;

  // Validates, then publishes atomically with mode 0600. Refuses symlinks and
  // a file that changed (or appeared) since Load()/the last Save(). Before the
  // first save over an existing file, a copy of it is kept at BackupPath()
  // (mode 0600) unless a backup already exists.
  bool Save(const std::filesystem::path& path, std::string* error);

  const std::string& bytes() const { return bytes_; }
  FileIdentity identity() const { return identity_; }
  bool HasUnsavedChanges() const { return bytes_ != disk_bytes_; }

 private:
  std::string bytes_;
  std::string disk_bytes_;
  std::string template_yaml_;
  FileIdentity identity_;
};

}  // namespace mocktail::launcher

#endif  // MOCKTAIL_LAUNCHER_CONFIG_DOCUMENT_H_
