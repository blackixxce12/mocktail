#include "launcher_ui/settings_draft.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "launcher/file_identity.h"
#include "runtime/runtime_config_bootstrap.h"

namespace mocktail::launcher_ui {
namespace {

constexpr char kReadOnlyError[] =
    "config.yaml has to be fixed before settings can be changed";
// The names of the copies kept before the whole file is replaced
// (ConfigDocument::SaveKeepingCopy).
constexpr char kResetCopyLabel[] = "before-reset";
constexpr char kRestoreCopyLabel[] = "before-restore";

launcher::FileIdentity CurrentIdentity(const std::filesystem::path& path) {
  launcher::FileIdentity identity;
  struct stat status = {};
  if (path.empty() || lstat(path.c_str(), &status) != 0) {
    return identity;
  }
  identity.exists = true;
  identity.dev = status.st_dev;
  identity.ino = status.st_ino;
  identity.size = status.st_size;
  identity.mtime = status.st_mtim;
  return identity;
}

std::optional<std::string> Cached(
    const launcher::ConfigDocument& document, std::string_view key,
    std::map<std::string, std::optional<std::string>, std::less<>>* cache) {
  const auto found = cache->find(key);
  if (found != cache->end()) {
    return found->second;
  }
  std::optional<std::string> value = document.Get(key);
  cache->emplace(std::string(key), value);
  return value;
}

}  // namespace

int LineNumberFromMessage(std::string_view message) {
  constexpr std::string_view kLine = "line ";
  std::size_t position = 0;
  while ((position = message.find(kLine, position)) != std::string_view::npos) {
    const bool word_start =
        position == 0 ||
        !std::isalnum(static_cast<unsigned char>(message[position - 1]));
    position += kLine.size();
    if (!word_start) {
      continue;
    }
    int line = 0;
    std::size_t digits = 0;
    while (
        position + digits < message.size() &&
        std::isdigit(static_cast<unsigned char>(message[position + digits])) &&
        digits < 7) {
      line = line * 10 + (message[position + digits] - '0');
      ++digits;
    }
    if (digits > 0 && line > 0) {
      return line;
    }
  }
  return 0;
}

SettingsDraft::SettingsDraft()
    : template_(launcher::ConfigDocument::FromBytes(
          std::string(runtime::DefaultRuntimeConfigYaml()))) {}

bool SettingsDraft::Load(const std::filesystem::path& path) {
  path_ = path;
  launcher::ConfigDocument document;
  std::string error;
  if (!launcher::ConfigDocument::Load(path, &document, &error)) {
    // Nothing usable: show the template, refuse changes, keep the reason.
    Adopt(launcher::ConfigDocument::FromBytes(
        std::string(runtime::DefaultRuntimeConfigYaml())));
    load_error_ = error.empty() ? "config.yaml cannot be read" : error;
    load_error_line_ = 0;
    return false;
  }
  Adopt(std::move(document));
  ClassifyLoadError();
  return true;
}

void SettingsDraft::LoadBytes(std::string bytes, std::filesystem::path path) {
  path_ = std::move(path);
  Adopt(launcher::ConfigDocument::FromBytes(std::move(bytes)));
  ClassifyLoadError();
}

void SettingsDraft::Adopt(launcher::ConfigDocument document) {
  saved_ = document;
  working_ = std::move(document);
  touched_.clear();
  replaced_all_ = false;
  load_error_.clear();
  load_error_line_ = 0;
  InvalidateCache();
  saved_cache_.clear();
}

void SettingsDraft::ClassifyLoadError() {
  std::string error;
  if (!saved_.Validate(&error)) {
    load_error_ = error.empty() ? "config.yaml is not valid" : error;
    load_error_line_ = LineNumberFromMessage(load_error_);
  }
}

bool SettingsDraft::file_is_blank() const {
  // A file that could not be read at all was replaced by the template.
  if (!read_only()) return false;
  const std::string& bytes = saved_.bytes();
  return std::all_of(bytes.begin(), bytes.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; });
}

void SettingsDraft::InvalidateCache() const { working_cache_.clear(); }

std::optional<std::string> SettingsDraft::Get(std::string_view key) const {
  if (read_only()) {
    return TemplateValue(key);
  }
  return Cached(working_, key, &working_cache_);
}

std::optional<std::string> SettingsDraft::Saved(std::string_view key) const {
  if (read_only()) {
    return TemplateValue(key);
  }
  return Cached(saved_, key, &saved_cache_);
}

std::optional<std::string> SettingsDraft::TemplateValue(
    std::string_view key) const {
  return template_.Get(key);
}

bool SettingsDraft::Set(std::string_view key, std::string_view value,
                        launcher::ScalarKind kind, std::string* error) {
  if (read_only()) {
    if (error != nullptr) *error = kReadOnlyError;
    return false;
  }
  if (!working_.Set(key, value, kind, error)) {
    return false;
  }
  touched_.emplace(key);
  InvalidateCache();
  return true;
}

bool SettingsDraft::Unset(std::string_view key, std::string* error) {
  if (read_only()) {
    if (error != nullptr) *error = kReadOnlyError;
    return false;
  }
  if (!working_.Unset(key, error)) {
    return false;
  }
  touched_.emplace(key);
  InvalidateCache();
  return true;
}

bool SettingsDraft::ResetToTemplate(std::string_view key,
                                    launcher::ScalarKind kind,
                                    std::string* error) {
  const std::optional<std::string> value = TemplateValue(key);
  return value.has_value() ? Set(key, *value, kind, error) : Unset(key, error);
}

std::vector<std::string> SettingsDraft::ChangedKeys() const {
  std::vector<std::string> keys;
  if (read_only()) {
    return keys;
  }
  for (const std::string& key : touched_) {
    if (Get(key) != Saved(key)) {
      keys.push_back(key);
    }
  }
  return keys;
}

bool SettingsDraft::IsChanged(std::string_view key) const {
  return !read_only() && touched_.count(key) != 0 && Get(key) != Saved(key);
}

bool SettingsDraft::HasChanges() const {
  return !read_only() && working_.bytes() != saved_.bytes();
}

void SettingsDraft::Discard() {
  working_ = saved_;
  touched_.clear();
  replaced_all_ = false;
  InvalidateCache();
}

bool SettingsDraft::Validate(std::string* error) const {
  return working_.Validate(error);
}

bool SettingsDraft::Save(std::string* error, std::filesystem::path* kept) {
  if (kept != nullptr) kept->clear();
  if (read_only()) {
    if (error != nullptr) *error = kReadOnlyError;
    return false;
  }
  if (path_.empty()) {
    if (error != nullptr) *error = "no config.yaml path";
    return false;
  }
  std::filesystem::path copy;
  const bool saved =
      replaced_all_
          ? working_.SaveKeepingCopy(path_, kResetCopyLabel, &copy, error)
          : working_.Save(path_, error);
  if (!saved) {
    return false;
  }
  if (kept != nullptr) *kept = std::move(copy);
  saved_ = working_;
  touched_.clear();
  replaced_all_ = false;
  saved_cache_.clear();
  return true;
}

bool SettingsDraft::Reload(std::string* error) {
  if (!Load(path_)) {
    if (error != nullptr) *error = load_error_;
    return false;
  }
  return true;
}

bool SettingsDraft::ChangedOnDisk() const {
  return !launcher::SameFileIdentity(CurrentIdentity(path_), saved_.identity());
}

bool SettingsDraft::ReplaceAll(std::string bytes, std::string* error) {
  if (read_only()) {
    if (error != nullptr) *error = kReadOnlyError;
    return false;
  }
  working_.ReplaceAll(std::move(bytes));
  replaced_all_ = true;
  InvalidateCache();
  return true;
}

bool SettingsDraft::RestoreBytes(std::string bytes, std::string* error,
                                 std::filesystem::path* kept) {
  if (kept != nullptr) kept->clear();
  if (path_.empty()) {
    if (error != nullptr) *error = "no config.yaml path";
    return false;
  }
  launcher::ConfigDocument document = saved_;
  document.ReplaceAll(std::move(bytes));
  std::filesystem::path copy;
  if (!document.SaveKeepingCopy(path_, kRestoreCopyLabel, &copy, error)) {
    return false;
  }
  if (kept != nullptr) *kept = std::move(copy);
  Adopt(std::move(document));
  ClassifyLoadError();
  return true;
}

}  // namespace mocktail::launcher_ui
