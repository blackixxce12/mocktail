#ifndef MOCKTAIL_LAUNCHER_UI_SETTINGS_DRAFT_H_
#define MOCKTAIL_LAUNCHER_UI_SETTINGS_DRAFT_H_

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/config_document.h"

namespace mocktail::launcher_ui {

// The staged edit of config.yaml behind the settings window. Rows change the
// working copy; nothing reaches the disk until Save(). Keys are dotted
// config.yaml paths ("graphics.vsync"). Values are the scalars as the loader
// reads them ("true", "direct-vulkan", "1600"); nullopt means the key is
// absent (or commented out), so the runtime's built-in default applies.
//
// A file that cannot be read or does not pass the real loader puts the
// draft into the read-only state: Get() then answers with the first-run
// template's defaults and every change is refused, so the window can show
// the error and still start.
class SettingsDraft {
 public:
  SettingsDraft();

  // Reads `path`. Returns false only when the file cannot be read at all
  // (a symlink, a directory, too large, unreadable); a missing file reads
  // as the template and is created by the first Save(). Either way
  // load_error() then describes any problem.
  bool Load(const std::filesystem::path& path);
  // Tests: the given text as if read from `path` (which Save() writes).
  void LoadBytes(std::string bytes, std::filesystem::path path = {});

  const std::filesystem::path& path() const { return path_; }
  // Why the file is not usable as it is; empty when it is.
  const std::string& load_error() const { return load_error_; }
  // The config.yaml line the error names (from "line N" in the loader's
  // message), or 0.
  int load_error_line() const { return load_error_line_; }
  bool read_only() const { return !load_error_.empty(); }

  // The value the next start reads from config.yaml after Save().
  std::optional<std::string> Get(std::string_view key) const;
  // The value on disk (as last loaded or saved).
  std::optional<std::string> Saved(std::string_view key) const;
  // The first-run template's value, nullopt when the template leaves the
  // key commented out (frame_rate_limit, vsync, proxy_host, ...).
  std::optional<std::string> TemplateValue(std::string_view key) const;

  // Each refuses (false, with *error set) in the read-only state, and when
  // ConfigDocument refuses the edit.
  bool Set(std::string_view key, std::string_view value,
           launcher::ScalarKind kind, std::string* error);
  bool Unset(std::string_view key, std::string* error);
  // The template value, or Unset when the template has none.
  bool ResetToTemplate(std::string_view key, launcher::ScalarKind kind,
                       std::string* error);

  // Keys whose value differs from the saved file, in key order.
  std::vector<std::string> ChangedKeys() const;
  bool IsChanged(std::string_view key) const;
  // Any difference from the file, including whole-text replacements.
  bool HasChanges() const;
  // Drops every unsaved change.
  void Discard();

  // The working copy through the real loader (see ConfigDocument).
  bool Validate(std::string* error) const;
  // Validates and publishes the working copy (ConfigDocument::Save).
  bool Save(std::string* error);
  // Reads the file again, dropping unsaved changes.
  bool Reload(std::string* error);
  // The file on disk is no longer the one last loaded or saved.
  bool ChangedOnDisk() const;

  // Replaces the whole working text (reset all settings).
  bool ReplaceAll(std::string bytes, std::string* error);
  // Writes `bytes` over the file right away and reloads it; works in the
  // read-only state too (restore the launcher backup of a broken file).
  // The bytes must pass the loader, and the file must still be the one
  // last loaded.
  bool RestoreBytes(std::string bytes, std::string* error);
  const std::string& working_bytes() const { return working_.bytes(); }

 private:
  void Adopt(launcher::ConfigDocument document);
  void ClassifyLoadError();
  void InvalidateCache() const;

  std::filesystem::path path_;
  launcher::ConfigDocument saved_;
  launcher::ConfigDocument working_;
  launcher::ConfigDocument template_;
  std::set<std::string, std::less<>> touched_;
  std::string load_error_;
  int load_error_line_ = 0;
  mutable std::map<std::string, std::optional<std::string>, std::less<>>
      working_cache_;
  mutable std::map<std::string, std::optional<std::string>, std::less<>>
      saved_cache_;
};

// "line N" from a loader or YAML message, or 0.
int LineNumberFromMessage(std::string_view message);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_SETTINGS_DRAFT_H_
