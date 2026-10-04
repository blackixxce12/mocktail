#include "launcher/desktop_entry_cleanup.h"

#include <fcntl.h>
#include <glib.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "private_file.h"
#include "runtime/managed_environment.h"

extern char** environ;

namespace mocktail::launcher {
namespace {

constexpr std::size_t kMaximumDesktopEntryBytes = 256U * 1024U;
constexpr char kMainGroup[] = "Desktop Entry";
constexpr char kActionGroupPrefix[] = "Desktop Action ";
constexpr int kMaximumBackups = 10;

class KeyFile final {
 public:
  KeyFile() : file_(g_key_file_new()) {}
  ~KeyFile() { g_key_file_free(file_); }

  KeyFile(const KeyFile&) = delete;
  KeyFile& operator=(const KeyFile&) = delete;

  bool Load(const std::string& bytes, std::string* error) {
    GError* failure = nullptr;
    if (!g_key_file_load_from_data(
            file_, bytes.data(), bytes.size(),
            static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS |
                                       G_KEY_FILE_KEEP_TRANSLATIONS),
            &failure)) {
      *error = failure != nullptr ? failure->message : "invalid desktop entry";
      g_clear_error(&failure);
      return false;
    }
    return true;
  }

  std::vector<std::string> Groups() const {
    std::vector<std::string> groups;
    gchar** names = g_key_file_get_groups(file_, nullptr);
    for (gchar** name = names; name != nullptr && *name != nullptr; ++name) {
      groups.emplace_back(*name);
    }
    g_strfreev(names);
    return groups;
  }

  std::vector<std::string> Keys(const std::string& group) const {
    std::vector<std::string> keys;
    gchar** names = g_key_file_get_keys(file_, group.c_str(), nullptr, nullptr);
    for (gchar** name = names; name != nullptr && *name != nullptr; ++name) {
      keys.emplace_back(*name);
    }
    g_strfreev(names);
    return keys;
  }

  bool HasGroup(const std::string& group) const {
    return g_key_file_has_group(file_, group.c_str());
  }

  // The value as written (key-file escapes kept), for comparisons.
  std::optional<std::string> Raw(const std::string& group,
                                 const std::string& key) const {
    gchar* value =
        g_key_file_get_value(file_, group.c_str(), key.c_str(), nullptr);
    if (value == nullptr) {
      return std::nullopt;
    }
    std::string copy(value);
    g_free(value);
    return copy;
  }

  std::optional<std::string> String(const std::string& group,
                                    const std::string& key) const {
    gchar* value =
        g_key_file_get_string(file_, group.c_str(), key.c_str(), nullptr);
    if (value == nullptr) {
      return std::nullopt;
    }
    std::string copy(value);
    g_free(value);
    return copy;
  }

 private:
  GKeyFile* file_;
};

struct ExecToken {
  std::string text;
  // [begin, end) of the argument in Exec, quotes included.
  std::size_t begin = 0;
  std::size_t end = 0;
};

// Splits an Exec value into arguments as the desktop entry specification
// quotes them: whole arguments in double quotes, with \" \` \$ \\ escapes.
bool TokenizeExec(const std::string& exec, std::vector<ExecToken>* tokens) {
  tokens->clear();
  std::size_t index = 0;
  while (index < exec.size()) {
    if (exec[index] == ' ' || exec[index] == '\t') {
      ++index;
      continue;
    }
    ExecToken token;
    token.begin = index;
    if (exec[index] == '"') {
      ++index;
      bool closed = false;
      while (index < exec.size()) {
        if (exec[index] == '\\' && index + 1 < exec.size()) {
          token.text += exec[index + 1];
          index += 2;
          continue;
        }
        if (exec[index] == '"') {
          closed = true;
          ++index;
          break;
        }
        token.text += exec[index++];
      }
      if (!closed ||
          (index < exec.size() && exec[index] != ' ' && exec[index] != '\t')) {
        return false;
      }
    } else {
      while (index < exec.size() && exec[index] != ' ' && exec[index] != '\t') {
        token.text += exec[index++];
      }
    }
    token.end = index;
    tokens->push_back(std::move(token));
  }
  return true;
}

std::string Basename(const std::string& program) {
  const std::size_t slash = program.rfind('/');
  return slash == std::string::npos ? program : program.substr(slash + 1);
}

bool ParseAssignment(const std::string& token, EnvAssignment* assignment) {
  const std::size_t equals = token.find('=');
  if (equals == std::string::npos || equals == 0) {
    return false;
  }
  const std::string name = token.substr(0, equals);
  if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    return false;
  }
  for (const char character : name) {
    if (std::isalnum(static_cast<unsigned char>(character)) == 0 &&
        character != '_') {
      return false;
    }
  }
  assignment->name = name;
  assignment->value = token.substr(equals + 1);
  return true;
}

bool ContainsIgnoringCase(std::string_view text, std::string_view needle) {
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [](char left, char right) {
                       return std::toupper(static_cast<unsigned char>(left)) ==
                              std::toupper(static_cast<unsigned char>(right));
                     }) != text.end();
}

// A Roblox session or the file holding one, a name that reads like a
// credential, or user:password@ before the host of a URL (with or without
// its scheme), as proxy variables carry them.
bool IsSensitiveAssignment(const EnvAssignment& assignment) {
  if (assignment.name == "MOCKTAIL_ROBLOX_COOKIES" ||
      assignment.name == "MOCKTAIL_COOKIE_FILE") {
    return true;
  }
  for (const std::string_view marker :
       {"COOKIE", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL"}) {
    if (ContainsIgnoringCase(assignment.name, marker)) {
      return true;
    }
  }
  constexpr std::string_view kKeySuffix = "_KEY";
  const std::string_view name = assignment.name;
  if (name.size() > kKeySuffix.size() &&
      ContainsIgnoringCase(name.substr(name.size() - kKeySuffix.size()),
                           kKeySuffix)) {
    return true;
  }
  if (ContainsIgnoringCase(assignment.value, "ROBLOSECURITY")) {
    return true;
  }
  std::string_view authority = assignment.value;
  const std::size_t scheme = authority.find("://");
  if (scheme != std::string_view::npos) {
    authority.remove_prefix(scheme + 3);
  }
  authority = authority.substr(0, authority.find_first_of("/?#"));
  return authority.find('@') != std::string_view::npos;
}

bool IsMocktailProgram(const std::string& program) {
  std::string name = Basename(program);
  std::transform(name.begin(), name.end(), name.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return name.find("mocktail") != std::string::npos;
}

// Key-file escaping for a string value (what g_key_file_set_string writes).
std::string EscapeKeyFileValue(const std::string& value) {
  std::string escaped;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case ' ':
        escaped += index == 0 ? "\\s" : " ";
        break;
      default:
        escaped += character;
        break;
    }
  }
  return escaped;
}

std::string TrimLeft(const std::string& text) {
  std::size_t index = 0;
  while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
    ++index;
  }
  return text.substr(index);
}

// Replaces the one Exec line of [Desktop Entry]; every other byte stays.
bool RewriteExecLine(const std::string& bytes, const std::string& exec,
                     std::string* rewritten) {
  std::string group;
  std::size_t found_begin = std::string::npos;
  std::size_t found_end = 0;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    std::size_t end = bytes.find('\n', begin);
    const std::size_t next = end == std::string::npos ? bytes.size() : end + 1;
    if (end == std::string::npos) {
      end = bytes.size();
    }
    std::size_t content_end = end;
    if (content_end > begin && bytes[content_end - 1] == '\r') {
      --content_end;
    }
    const std::string line =
        TrimLeft(bytes.substr(begin, content_end - begin));
    if (!line.empty() && line.front() == '[') {
      const std::size_t close = line.find(']');
      group = close == std::string::npos ? std::string()
                                         : line.substr(1, close - 1);
    } else if (group == kMainGroup && !line.empty() && line.front() != '#') {
      const std::size_t equals = line.find('=');
      std::string key = line.substr(0, equals);
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
        key.pop_back();
      }
      if (equals != std::string::npos && key == "Exec") {
        if (found_begin != std::string::npos) {
          return false;
        }
        found_begin = begin;
        found_end = content_end;
      }
    }
    begin = next;
  }
  if (found_begin == std::string::npos) {
    return false;
  }
  *rewritten = bytes.substr(0, found_begin) + "Exec=" +
               EscapeKeyFileValue(exec) + bytes.substr(found_end);
  return true;
}

// True when the rewritten entry differs from the original only in Exec.
bool OnlyExecChanged(const KeyFile& original, const std::string& rewritten,
                     const std::string& exec) {
  KeyFile parsed;
  std::string error;
  if (!parsed.Load(rewritten, &error) ||
      parsed.String(kMainGroup, "Exec") != exec ||
      parsed.Groups() != original.Groups()) {
    return false;
  }
  for (const std::string& group : original.Groups()) {
    if (parsed.Keys(group) != original.Keys(group)) {
      return false;
    }
    for (const std::string& key : original.Keys(group)) {
      if (group == kMainGroup && key == "Exec") {
        continue;
      }
      if (parsed.Raw(group, key) != original.Raw(group, key)) {
        return false;
      }
    }
  }
  return true;
}

bool IsActionGroup(const std::string& group) {
  return group.rfind(kActionGroupPrefix, 0) == 0;
}

// Keys that may differ without the user's copy adding anything: the Exec
// being cleaned, the marker the packaged entry carries for
// register_url_handler.sh, and desktop actions the copy predates.
bool IgnorableMainKey(const std::string& key) {
  return key == "Exec" || key == "X-Mocktail-Managed";
}

bool AddsNothingToSystemEntry(const KeyFile& user, const KeyFile& system,
                              const std::string& new_exec) {
  if (system.String(kMainGroup, "Exec") != new_exec) {
    return false;
  }
  for (const std::string& group : user.Groups()) {
    if (!system.HasGroup(group)) {
      return false;
    }
    for (const std::string& key : user.Keys(group)) {
      if (group == kMainGroup && IgnorableMainKey(key)) {
        continue;
      }
      if (system.Raw(group, key) != user.Raw(group, key)) {
        return false;
      }
    }
  }
  for (const std::string& group : system.Groups()) {
    if (!user.HasGroup(group)) {
      if (IsActionGroup(group)) {
        continue;
      }
      return false;
    }
    for (const std::string& key : system.Keys(group)) {
      if (user.Raw(group, key).has_value()) {
        continue;
      }
      if (group == kMainGroup && (IgnorableMainKey(key) || key == "Actions")) {
        continue;
      }
      return false;
    }
  }
  return true;
}

std::filesystem::path FirstSystemEntry(const DesktopEntryPaths& paths) {
  for (const std::filesystem::path& candidate : paths.system_files) {
    if (candidate == paths.user_file) {
      continue;
    }
    struct stat metadata = {};
    if (stat(candidate.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode)) {
      return candidate;
    }
  }
  return {};
}

bool RunDatabaseTool(const std::string& tool,
                     const std::filesystem::path& directory,
                     std::string* warning) {
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    *warning = "cannot prepare " + tool;
    return false;
  }
  (void)posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0);
  (void)posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                         O_WRONLY, 0);
  (void)posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0);
  std::string program = tool;
  std::string argument = directory.string();
  char* arguments[] = {program.data(), argument.data(), nullptr};
  pid_t child = -1;
  const int spawned = posix_spawnp(&child, program.c_str(), &actions, nullptr,
                                   arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    *warning = "cannot run " + tool + ": " + std::strerror(spawned);
    return false;
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      *warning = "cannot wait for " + tool;
      return false;
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    *warning = tool + " did not finish successfully";
    return false;
  }
  return true;
}

}  // namespace

DesktopEntryPaths DefaultDesktopEntryPaths(
    const runtime::Environment& environment) {
  DesktopEntryPaths paths;
  std::filesystem::path data_home =
      environment.GetOr("XDG_DATA_HOME", "");
  if (!data_home.is_absolute()) {
    const std::filesystem::path home = environment.GetOr("HOME", "");
    data_home = home.is_absolute() ? home / ".local/share"
                                   : std::filesystem::path();
  }
  if (!data_home.empty()) {
    paths.user_file = data_home / "applications" / kDesktopEntryFileName;
  }
  std::string directories = environment.GetOr("XDG_DATA_DIRS", "");
  if (directories.empty()) {
    directories = "/usr/local/share:/usr/share";
  }
  std::size_t begin = 0;
  while (begin <= directories.size()) {
    std::size_t end = directories.find(':', begin);
    if (end == std::string::npos) {
      end = directories.size();
    }
    const std::filesystem::path directory =
        directories.substr(begin, end - begin);
    if (directory.is_absolute()) {
      paths.system_files.push_back(directory / "applications" /
                                   kDesktopEntryFileName);
    }
    begin = end + 1;
  }
  return paths;
}

bool InspectDesktopEntry(const DesktopEntryPaths& paths,
                         DesktopEntryInspection* inspection,
                         std::string* error) {
  *inspection = DesktopEntryInspection();
  inspection->path = paths.user_file;
  if (paths.user_file.empty()) {
    return true;
  }
  struct stat metadata = {};
  if (lstat(paths.user_file.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    *error = "cannot inspect " + paths.user_file.string() + ": " +
             std::strerror(errno);
    return false;
  }
  inspection->found = true;
  if (!S_ISREG(metadata.st_mode)) {
    inspection->note = S_ISLNK(metadata.st_mode)
                           ? "the shortcut is a symlink; it is left alone"
                           : "the shortcut is not a regular file";
    return true;
  }
  inspection->mode = metadata.st_mode & 07777;
  if (!internal::ReadRegularFile(paths.user_file, kMaximumDesktopEntryBytes,
                                 &inspection->bytes, &inspection->identity,
                                 error)) {
    return false;
  }

  KeyFile user;
  std::string parse_error;
  if (!user.Load(inspection->bytes, &parse_error)) {
    inspection->note = "the shortcut cannot be parsed: " + parse_error;
    return true;
  }
  const std::optional<std::string> exec = user.String(kMainGroup, "Exec");
  if (!exec.has_value()) {
    inspection->note = "the shortcut has no Exec line";
    return true;
  }
  inspection->exec = *exec;
  std::vector<ExecToken> tokens;
  if (!TokenizeExec(*exec, &tokens) || tokens.empty()) {
    inspection->note = "the shortcut's Exec line cannot be parsed";
    return true;
  }
  if (Basename(tokens.front().text) != "env") {
    return true;
  }
  std::size_t command = 1;
  for (; command < tokens.size(); ++command) {
    EnvAssignment assignment;
    if (!ParseAssignment(tokens[command].text, &assignment)) {
      break;
    }
    assignment.managed =
        runtime::FindManagedEnvironmentVariable(assignment.name) != nullptr;
    assignment.sensitive = IsSensitiveAssignment(assignment);
    inspection->env_assignments.push_back(std::move(assignment));
  }
  if (command >= tokens.size()) {
    inspection->env_assignments.clear();
    inspection->note = "the shortcut's env line starts no program";
    return true;
  }
  if (tokens[command].text.empty() || tokens[command].text.front() == '-') {
    inspection->note = "the shortcut runs env with options; edit it by hand";
    return true;
  }
  if (!IsMocktailProgram(tokens[command].text)) {
    inspection->env_assignments.clear();
    inspection->note = "the shortcut does not start Mocktail through env";
    return true;
  }
  // Variables the settings window cannot represent (DRI_PRIME,
  // __NV_PRIME_RENDER_OFFLOAD, LD_PRELOAD, ...) are the user's own: they
  // stay, quoted as before, and only the managed ones go.
  std::string kept;
  bool any_managed = false;
  for (std::size_t index = 1; index < command; ++index) {
    if (inspection->env_assignments[index - 1].managed) {
      any_managed = true;
      continue;
    }
    kept += exec->substr(tokens[index].begin,
                         tokens[index].end - tokens[index].begin);
    kept += ' ';
  }
  if (!any_managed) {
    return true;
  }
  inspection->new_exec =
      (kept.empty() ? std::string()
                    : exec->substr(tokens.front().begin,
                                   tokens.front().end - tokens.front().begin) +
                          ' ' + kept) +
      exec->substr(tokens[command].begin);

  inspection->system_path = FirstSystemEntry(paths);
  if (!inspection->system_path.empty()) {
    std::string system_bytes;
    FileIdentity system_identity;
    std::string system_error;
    KeyFile system;
    if (internal::ReadRegularFile(inspection->system_path,
                                  kMaximumDesktopEntryBytes, &system_bytes,
                                  &system_identity, &system_error) &&
        system_identity.exists && system.Load(system_bytes, &system_error) &&
        AddsNothingToSystemEntry(user, system, inspection->new_exec)) {
      inspection->plan = DesktopEntryCleanupPlan::kDeleteFile;
      return true;
    }
  }
  if (!RewriteExecLine(inspection->bytes, inspection->new_exec,
                       &inspection->rewritten_bytes) ||
      !OnlyExecChanged(user, inspection->rewritten_bytes,
                       inspection->new_exec)) {
    inspection->rewritten_bytes.clear();
    inspection->note = "the shortcut's Exec line cannot be rewritten safely";
    return true;
  }
  inspection->plan = DesktopEntryCleanupPlan::kRewriteExec;
  return true;
}

bool ApplyDesktopEntryCleanup(const DesktopEntryInspection& inspection,
                              const DesktopEntryApplyOptions& options,
                              DesktopEntryApplyResult* result,
                              std::string* error) {
  *result = DesktopEntryApplyResult();
  if (!inspection.found ||
      inspection.plan == DesktopEntryCleanupPlan::kNone) {
    *error = "there is nothing to clean up in the shortcut";
    return false;
  }
  const std::filesystem::path& path = inspection.path;
  if (!internal::VerifyUnchanged(path, inspection.identity, inspection.bytes,
                                 kMaximumDesktopEntryBytes, error)) {
    return false;
  }

  bool created = false;
  for (int attempt = 0; attempt < kMaximumBackups && !created; ++attempt) {
    std::filesystem::path backup = path.string() + ".mocktail-backup";
    if (attempt > 0) {
      backup += "." + std::to_string(attempt);
    }
    if (!internal::CreateExclusiveFile(backup, inspection.bytes,
                                       S_IRUSR | S_IWUSR, &created, error)) {
      return false;
    }
    if (created) {
      result->backup_path = backup;
    }
  }
  if (!created) {
    *error = "too many shortcut backups next to " + path.string();
    return false;
  }

  const std::filesystem::path directory = path.parent_path();
  if (inspection.plan == DesktopEntryCleanupPlan::kDeleteFile) {
    if (!internal::VerifyUnchanged(path, inspection.identity, inspection.bytes,
                                   kMaximumDesktopEntryBytes, error)) {
      return false;
    }
    if (unlink(path.c_str()) != 0) {
      *error = "cannot remove " + path.string() + ": " + std::strerror(errno);
      return false;
    }
    if (!internal::SyncDirectory(directory, error)) {
      return false;
    }
  } else {
    FileIdentity published;
    if (!internal::PublishAtomically(path, inspection.rewritten_bytes,
                                     static_cast<mode_t>(inspection.mode),
                                     inspection.identity, &published,
                                     error)) {
      return false;
    }
  }

  if (options.refresh_database && !options.database_tool.empty()) {
    result->database_refreshed = RunDatabaseTool(
        options.database_tool, directory, &result->database_warning);
  }
  return true;
}

}  // namespace mocktail::launcher
