#include "launcher/fast_flags_document.h"

#include <sys/stat.h>

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

#include "private_file.h"
#include "utf8.h"

namespace mocktail::launcher {
namespace {

constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr std::size_t kMaximumNameBytes = 256;
// The name of the copy Save() keeps of a file with comments.
constexpr char kCommentedCopyLabel[] = "with-comments";

bool SameEntries(const std::vector<FastFlagEntry>& left,
                 const std::vector<FastFlagEntry>& right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                    [](const FastFlagEntry& a, const FastFlagEntry& b) {
                      return a.name == b.name && a.kind == b.kind &&
                             a.value == b.value;
                    });
}

bool IsValidName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumNameBytes) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

// Canonical decimal text of a 64-bit integer (signed or unsigned).
bool NormalizeInteger(std::string_view text, std::string* normalized) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  if (!text.empty() && text.front() == '-') {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
      return false;
    }
    *normalized = std::to_string(value);
    return true;
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(begin, end, value);
  if (text.empty() || parsed.ec != std::errc() || parsed.ptr != end) {
    return false;
  }
  *normalized = std::to_string(value);
  return true;
}

bool EntryFromJson(const std::string& name, const nlohmann::ordered_json& value,
                   FastFlagEntry* entry) {
  entry->name = name;
  if (value.is_string()) {
    entry->kind = FastFlagValueKind::kString;
    entry->value = value.get<std::string>();
    return true;
  }
  if (value.is_boolean()) {
    entry->kind = FastFlagValueKind::kBoolean;
    entry->value = value.get<bool>() ? "true" : "false";
    return true;
  }
  if (value.is_number_unsigned()) {
    entry->kind = FastFlagValueKind::kInteger;
    entry->value = std::to_string(value.get<std::uint64_t>());
    return true;
  }
  if (value.is_number_integer()) {
    entry->kind = FastFlagValueKind::kInteger;
    entry->value = std::to_string(value.get<std::int64_t>());
    return true;
  }
  return false;
}

// What Mocktail itself puts into the client settings for these settings,
// before fflags.json entries are considered.
bool ManagedSettings(const runtime::FrameRatePolicy& frame_rate,
                     const runtime::PerformancePolicy& performance,
                     nlohmann::json* managed) {
  std::string merged;
  std::string audio;
  std::string menu;
  std::string error;
  if (!runtime::MergeRuntimeClientSettingsOverrides(frame_rate, performance,
                                                    "{}", &merged, &error) ||
      !runtime::MergeAudioCaptureClientSettingsOverrides(true, merged, &audio,
                                                         &error) ||
      !runtime::MergeAudioDeviceMenuClientSettingsOverrides(audio, &menu,
                                                            &error)) {
    return false;
  }
  *managed = nlohmann::json::parse(menu, nullptr, false);
  return managed->is_object();
}

std::string ManagedValueText(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return value.dump();
}

const std::set<std::string>& AllManagedFlagNames() {
  static const std::set<std::string> names = []() {
    std::set<std::string> collected = {"FIntDebugFRMQualityLevelOverride"};
    runtime::FrameRatePolicy fixed;
    fixed.mode = runtime::FrameRateLimitMode::kFixed;
    fixed.fixed_fps = 60;
    runtime::PerformancePolicy throughput;
    throughput.multithreaded_rendering = true;
    throughput.physical_core_count = 4;
    throughput.physics_worker_mode = runtime::PhysicsWorkerMode::kThroughput;
    runtime::PerformancePolicy latency;
    latency.multithreaded_rendering = true;
    latency.physical_core_count = 4;
    latency.physics_worker_mode = runtime::PhysicsWorkerMode::kLatency;
    for (const runtime::PerformancePolicy& performance :
         std::array<runtime::PerformancePolicy, 2>{throughput, latency}) {
      nlohmann::json managed;
      if (ManagedSettings(fixed, performance, &managed)) {
        for (const auto& [name, value] : managed.items()) {
          (void)value;
          collected.insert(name);
        }
      }
    }
    return collected;
  }();
  return names;
}

}  // namespace

std::string FastFlagEntry::RobloxValue() const {
  if (kind == FastFlagValueKind::kBoolean) {
    return value == "true" ? "True" : "False";
  }
  return value;
}

bool FastFlagsDocument::Load(const std::filesystem::path& path,
                             FastFlagsDocument* out, std::string* error) {
  std::string bytes;
  FileIdentity identity;
  if (!internal::ReadRegularFile(path, kMaximumBytes, &bytes, &identity,
                                 error)) {
    return false;
  }
  FastFlagsDocument document;
  if (identity.exists && !FromBytes(bytes, &document, error)) {
    *error = path.string() + ": " + *error;
    return false;
  }
  document.identity_ = identity;
  document.disk_bytes_ = std::move(bytes);
  *out = std::move(document);
  return true;
}

bool FastFlagsDocument::FromBytes(std::string_view bytes,
                                  FastFlagsDocument* out, std::string* error) {
  if (bytes.size() > kMaximumBytes) {
    *error = "fflags.json exceeds the 64 KiB limit";
    return false;
  }
  const nlohmann::ordered_json parsed =
      nlohmann::ordered_json::parse(bytes, nullptr, false, true);
  if (parsed.is_discarded() || !parsed.is_object()) {
    *error = "fflags.json must contain a JSON object";
    return false;
  }
  FastFlagsDocument document;
  // Parsed with comments ignored, as the runtime reads the file
  // (client_settings_service.cc); without that it only parses when it has
  // none.
  document.has_comments_ =
      nlohmann::ordered_json::parse(bytes, nullptr, false, false)
          .is_discarded();
  for (const auto& [name, value] : parsed.items()) {
    FastFlagEntry entry;
    if (name.empty()) {
      *error = "fflags.json contains an empty flag name";
      return false;
    }
    if (!EntryFromJson(name, value, &entry)) {
      *error = "fflags.json value of " + name +
               " must be a string, a boolean or an integer";
      return false;
    }
    document.entries_.push_back(std::move(entry));
  }
  document.saved_entries_ = document.entries_;
  *out = std::move(document);
  return true;
}

const FastFlagEntry* FastFlagsDocument::Find(std::string_view name) const {
  const auto found = std::find_if(
      entries_.begin(), entries_.end(),
      [name](const FastFlagEntry& entry) { return entry.name == name; });
  return found == entries_.end() ? nullptr : &*found;
}

bool FastFlagsDocument::Set(std::string_view name, FastFlagValueKind kind,
                            std::string_view value, std::string* error) {
  if (!IsValidName(name)) {
    *error = "a flag name uses only letters, digits and underscores";
    return false;
  }
  FastFlagEntry entry;
  entry.name = std::string(name);
  entry.kind = kind;
  switch (kind) {
    case FastFlagValueKind::kBoolean: {
      std::string lower(value);
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](char character) {
                       return character >= 'A' && character <= 'Z'
                                  ? static_cast<char>(character - 'A' + 'a')
                                  : character;
                     });
      if (lower != "true" && lower != "false") {
        *error = std::string(name) + ": a boolean must be true or false";
        return false;
      }
      entry.value = lower;
      break;
    }
    case FastFlagValueKind::kInteger:
      if (!NormalizeInteger(value, &entry.value)) {
        *error = std::string(name) + ": not a 64-bit decimal integer";
        return false;
      }
      break;
    case FastFlagValueKind::kString:
      if (!internal::IsValidUtf8(value)) {
        *error = std::string(name) + ": the value is not valid UTF-8";
        return false;
      }
      entry.value = std::string(value);
      break;
  }
  const auto existing = std::find_if(
      entries_.begin(), entries_.end(),
      [name](const FastFlagEntry& candidate) {
        return candidate.name == name;
      });
  if (existing != entries_.end()) {
    *existing = std::move(entry);
  } else {
    entries_.push_back(std::move(entry));
  }
  return true;
}

bool FastFlagsDocument::Remove(std::string_view name) {
  const auto existing = std::find_if(
      entries_.begin(), entries_.end(),
      [name](const FastFlagEntry& entry) { return entry.name == name; });
  if (existing == entries_.end()) {
    return false;
  }
  entries_.erase(existing);
  return true;
}

std::string FastFlagsDocument::Serialize() const {
  nlohmann::ordered_json object = nlohmann::ordered_json::object();
  for (const FastFlagEntry& entry : entries_) {
    switch (entry.kind) {
      case FastFlagValueKind::kString:
        object[entry.name] = entry.value;
        break;
      case FastFlagValueKind::kBoolean:
        object[entry.name] = entry.value == "true";
        break;
      case FastFlagValueKind::kInteger:
        if (!entry.value.empty() && entry.value.front() == '-') {
          std::int64_t number = 0;
          (void)std::from_chars(entry.value.data(),
                          entry.value.data() + entry.value.size(), number);
          object[entry.name] = number;
        } else {
          std::uint64_t number = 0;
          (void)std::from_chars(entry.value.data(),
                          entry.value.data() + entry.value.size(), number);
          object[entry.name] = number;
        }
        break;
    }
  }
  return object.dump(2, ' ', false,
                     nlohmann::ordered_json::error_handler_t::replace) +
         "\n";
}

bool FastFlagsDocument::Save(const std::filesystem::path& path,
                             std::string* error, std::filesystem::path* kept) {
  if (kept != nullptr) kept->clear();
  const std::string bytes = Serialize();
  if (bytes.size() > kMaximumBytes) {
    *error = "fflags.json would exceed the 64 KiB limit Mocktail reads";
    return false;
  }
  if (!internal::VerifyUnchanged(path, identity_, disk_bytes_, kMaximumBytes,
                                 error)) {
    return false;
  }
  // Notes and flags switched off in comments would be gone for good.
  std::filesystem::path copy;
  if (identity_.exists && has_comments_ &&
      !internal::KeepCopyBeside(path, kCommentedCopyLabel, disk_bytes_, &copy,
                                error)) {
    return false;
  }
  FileIdentity published;
  if (!internal::PublishAtomically(path, bytes, kPrivateFileMode, identity_,
                                   &published, error)) {
    return false;
  }
  identity_ = published;
  disk_bytes_ = bytes;
  saved_entries_ = entries_;
  has_comments_ = false;
  if (kept != nullptr) *kept = std::move(copy);
  return true;
}

std::vector<FastFlagConflict> FastFlagsDocument::FindManagedConflicts(
    const runtime::FrameRatePolicy& frame_rate,
    const runtime::PerformancePolicy& performance) const {
  std::vector<FastFlagConflict> conflicts;
  nlohmann::json managed;
  if (!ManagedSettings(frame_rate, performance, &managed)) {
    return conflicts;
  }
  for (const FastFlagEntry& entry : entries_) {
    const auto found = managed.find(entry.name);
    if (found == managed.end()) {
      continue;
    }
    FastFlagConflict conflict;
    conflict.name = entry.name;
    conflict.file_value = entry.RobloxValue();
    conflict.managed_value = ManagedValueText(*found);
    if (conflict.file_value == conflict.managed_value) {
      continue;
    }
    // Startup merges the file under the policies; a policy that checks for
    // compatible values rejects it, the others overwrite it.
    nlohmann::json base = nlohmann::json::object();
    base[entry.name] = conflict.file_value;
    std::string merged;
    std::string error;
    conflict.effect =
        runtime::MergeRuntimeClientSettingsOverrides(
            frame_rate, performance, base.dump(), &merged, &error)
            ? FastFlagConflictEffect::kOverridden
            : FastFlagConflictEffect::kBlocksStart;
    conflicts.push_back(std::move(conflict));
  }
  return conflicts;
}

bool FastFlagsDocument::IsManagedFlag(std::string_view name) {
  return AllManagedFlagNames().count(std::string(name)) != 0;
}

bool FastFlagsDocument::HasUnsavedChanges() const {
  return !SameEntries(entries_, saved_entries_) ||
         (!identity_.exists && !entries_.empty());
}

}  // namespace mocktail::launcher
