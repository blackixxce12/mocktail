#include "launcher/config_document.h"

#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "private_file.h"
#include "runtime/environment.h"
#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_file.h"
#include "update/update_config.h"
#include "utf8.h"
#include "yaml_layout.h"

namespace mocktail::launcher {
namespace {

using internal::ClassifyLine;
using internal::CommentedKey;
using internal::Layout;
using internal::LayoutNode;
using internal::LineKind;
using internal::NodeKind;
using internal::TextLines;

constexpr std::size_t kMaximumConfigBytes = 1024U * 1024U;
constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr std::size_t kDefaultIndentStep = 2;

// Flattened view used to prove that an edit changed only what it meant to.
// Collections get a marker value, so a mapping turning into a scalar (or the
// reverse) is a visible change too.
using FlatMap = std::map<std::string, std::string>;

FlatMap Flatten(const Layout& layout) {
  FlatMap flat;
  for (const LayoutNode& node : layout.nodes) {
    if (node.path.empty()) {
      continue;
    }
    std::string value;
    switch (node.kind) {
      case NodeKind::kScalar:
        value = node.value;
        break;
      case NodeKind::kMapping:
        value = "\x01mapping";
        break;
      case NodeKind::kSequence:
        value = "\x01sequence";
        break;
      case NodeKind::kAlias:
        value = "\x01alias";
        break;
    }
    flat.emplace(node.path, std::move(value));
  }
  return flat;
}

bool IsUnder(const std::string& path, const std::string& prefix) {
  if (prefix.empty()) {
    return true;
  }
  if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix)) {
    return false;
  }
  return path.size() == prefix.size() || path[prefix.size()] == '.' ||
         path[prefix.size()] == '[';
}

bool IsAncestorOrSelf(const std::string& ancestor, const std::string& path) {
  return IsUnder(path, ancestor);
}

// Every value outside `changeable` must be identical before and after.
bool OnlyChangedWhere(const FlatMap& before, const FlatMap& after,
                      const std::function<bool(const std::string&)>&
                          changeable) {
  for (const auto& [path, value] : before) {
    if (changeable(path)) {
      continue;
    }
    const auto found = after.find(path);
    if (found == after.end() || found->second != value) {
      return false;
    }
  }
  for (const auto& [path, value] : after) {
    if (changeable(path)) {
      continue;
    }
    if (before.find(path) == before.end()) {
      return false;
    }
  }
  return true;
}

bool SplitPath(std::string_view dotted, std::vector<std::string>* parts,
               std::string* error) {
  parts->clear();
  std::size_t begin = 0;
  while (true) {
    const std::size_t dot = dotted.find('.', begin);
    const std::string_view part = dotted.substr(
        begin, dot == std::string_view::npos ? std::string_view::npos
                                             : dot - begin);
    bool valid = !part.empty();
    for (const char character : part) {
      if (!((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-')) {
        valid = false;
      }
    }
    if (!valid) {
      *error = "invalid configuration key: " + std::string(dotted);
      return false;
    }
    parts->emplace_back(part);
    if (dot == std::string_view::npos) {
      return true;
    }
    begin = dot + 1;
  }
}

std::string JoinParts(const std::vector<std::string>& parts,
                      std::size_t count) {
  std::string joined;
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      joined += '.';
    }
    joined += parts[index];
  }
  return joined;
}

std::string Hex(char32_t value, int digits) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text(static_cast<std::size_t>(digits), '0');
  for (int position = digits - 1; position >= 0; --position) {
    text[static_cast<std::size_t>(position)] = kDigits[value & 0xFU];
    value >>= 4U;
  }
  return text;
}

// libyaml accepts only printable characters in a stream; everything else is
// written with an escape so the loader reads back exactly the given string.
bool QuoteString(std::string_view value, std::string* quoted,
                 std::string* error) {
  std::string text = "\"";
  std::size_t index = 0;
  while (index < value.size()) {
    const std::size_t begin = index;
    char32_t code_point = 0;
    if (!internal::DecodeUtf8(value, &index, &code_point)) {
      *error = "the value is not valid UTF-8";
      return false;
    }
    switch (code_point) {
      case U'"':
        text += "\\\"";
        continue;
      case U'\\':
        text += "\\\\";
        continue;
      case U'\0':
        text += "\\0";
        continue;
      case U'\t':
        text += "\\t";
        continue;
      case U'\n':
        text += "\\n";
        continue;
      case U'\r':
        text += "\\r";
        continue;
      case 0x85:
        text += "\\N";
        continue;
      case 0x2028:
        text += "\\L";
        continue;
      case 0x2029:
        text += "\\P";
        continue;
      default:
        break;
    }
    const bool printable =
        (code_point >= 0x20 && code_point <= 0x7E) ||
        (code_point >= 0xA0 && code_point <= 0xD7FF) ||
        (code_point >= 0xE000 && code_point <= 0xFFFD &&
         code_point != 0xFEFF) ||
        (code_point >= 0x10000 && code_point <= 0x10FFFF);
    if (printable) {
      text.append(value.substr(begin, index - begin));
    } else if (code_point <= 0xFF) {
      text += "\\x" + Hex(code_point, 2);
    } else {
      text += "\\u" + Hex(code_point, 4);
    }
  }
  text += '"';
  *quoted = std::move(text);
  return true;
}

bool FormatScalar(std::string_view value, ScalarKind kind,
                  std::string* formatted, std::string* error) {
  switch (kind) {
    case ScalarKind::kBool:
      if (value != "true" && value != "false") {
        *error = "a boolean must be true or false";
        return false;
      }
      *formatted = std::string(value);
      return true;
    case ScalarKind::kInteger: {
      std::string_view digits = value;
      if (!digits.empty() && digits.front() == '-') {
        digits.remove_prefix(1);
      }
      bool valid = !digits.empty() && digits.size() <= 19 &&
                   (digits.size() == 1 || digits.front() != '0');
      for (const char character : digits) {
        valid = valid && character >= '0' && character <= '9';
      }
      if (!valid) {
        *error = "not a decimal integer: " + std::string(value);
        return false;
      }
      *formatted = std::string(value);
      return true;
    }
    case ScalarKind::kEnum: {
      bool valid = !value.empty() && value.size() <= 64;
      for (std::size_t index = 0; valid && index < value.size(); ++index) {
        const char character = value[index];
        const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        valid = alphanumeric ||
                (index != 0 &&
                 (character == '-' || character == '_' || character == '.'));
      }
      if (!valid) {
        *error = "not a plain option value: " + std::string(value);
        return false;
      }
      *formatted = std::string(value);
      return true;
    }
    case ScalarKind::kString:
      return QuoteString(value, formatted, error);
  }
  return false;
}

std::string Spaces(std::size_t count) { return std::string(count, ' '); }

// The comment-only part after a value in a commented line ("off  # note").
std::string TrailingComment(std::string_view rest) {
  if (rest.empty()) {
    return {};
  }
  if (rest.front() == '#') {
    return std::string(rest);
  }
  std::size_t index = 0;
  if (rest.front() == '"' || rest.front() == '\'') {
    const char quote = rest.front();
    index = 1;
    while (index < rest.size() && rest[index] != quote) {
      index += (quote == '"' && rest[index] == '\\') ? 2 : 1;
    }
    ++index;
  }
  const std::size_t hash = rest.find(" #", std::min(index, rest.size()));
  if (hash == std::string_view::npos) {
    return {};
  }
  return std::string(rest.substr(hash + 1));
}

// Column a comment line's text would have with "# " removed.
std::size_t UncommentedColumn(std::string_view content) {
  std::size_t index = 0;
  while (index < content.size() && content[index] == ' ') {
    ++index;
  }
  const std::size_t hash = index++;
  std::size_t spaces = 0;
  while (index < content.size() && content[index] == ' ') {
    ++index;
    ++spaces;
  }
  return hash + (spaces > 0 ? spaces - 1 : 0);
}

// First line of the comment block that sits directly above `line` at exactly
// `column`. Such comments describe the key on `line` and move with it.
std::size_t LeadingCommentStart(const TextLines& lines, std::size_t line,
                                std::size_t column) {
  while (line > 0) {
    std::size_t comment_column = 0;
    if (ClassifyLine(lines.content(line - 1), &comment_column) !=
            LineKind::kComment ||
        comment_column != column) {
      break;
    }
    --line;
  }
  return line;
}

// Last line that belongs to a block mapping's children: its last value, or a
// later comment indented deeper than the children (a nested example).
std::size_t ChildrenEndLine(const TextLines& lines, const LayoutNode& mapping) {
  std::size_t last = mapping.last_line;
  const std::size_t end = std::min(mapping.range_end, lines.count());
  for (std::size_t line = mapping.last_line + 1; line < end; ++line) {
    std::size_t column = 0;
    const LineKind kind = ClassifyLine(lines.content(line), &column);
    if (kind == LineKind::kBlank) {
      continue;
    }
    if (kind == LineKind::kComment &&
        static_cast<long>(column) > mapping.child_column) {
      last = line;
      continue;
    }
    break;
  }
  return last;
}

// Lines [begin, end) a key's commented example may appear in, and the column
// its children use.
struct SearchRange {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::size_t column = 0;
};

SearchRange MappingRange(const Layout& layout, const TextLines& lines,
                         int mapping) {
  const LayoutNode& node = layout.nodes[mapping];
  SearchRange range;
  if (node.has_key) {
    range.begin = node.key_start.line + 1;
    range.end = std::min(node.range_end, lines.count());
  } else {
    range.begin = 0;
    range.end = lines.count();
  }
  range.column =
      node.child_column >= 0 ? static_cast<std::size_t>(node.child_column) : 0;
  return range;
}

struct FoundComment {
  std::size_t line = 0;
  std::size_t column = 0;
  std::string key;
  std::string rest;
};

// Finds "# key:" lines for keys[depth..] inside range. Intermediate keys must
// be bare headers ("# text:") whose commented children follow, deeper.
bool FindCommentedChain(
    const TextLines& lines, SearchRange range,
    const std::vector<std::string>& keys, std::size_t depth,
    const std::function<bool(const std::string&)>& accept_last,
    std::vector<FoundComment>* found) {
  const bool last = depth + 1 == keys.size();
  for (std::size_t line = range.begin; line < range.end; ++line) {
    CommentedKey commented;
    if (!internal::ParseCommentedKey(lines.content(line), &commented) ||
        commented.key != keys[depth] ||
        commented.key_column != range.column) {
      continue;
    }
    if (last) {
      if (!accept_last(commented.rest)) {
        continue;
      }
      found->push_back({line, range.column, commented.key, commented.rest});
      return true;
    }
    if (!commented.rest.empty() && commented.rest.front() != '#') {
      continue;
    }
    SearchRange children;
    children.begin = line + 1;
    children.end = children.begin;
    bool have_column = false;
    while (children.end < range.end) {
      std::size_t column = 0;
      const std::string_view content = lines.content(children.end);
      if (ClassifyLine(content, &column) != LineKind::kComment ||
          UncommentedColumn(content) <= range.column) {
        break;
      }
      CommentedKey child;
      if (!have_column && internal::ParseCommentedKey(content, &child)) {
        children.column = child.key_column;
        have_column = true;
      }
      ++children.end;
    }
    if (!have_column) {
      continue;
    }
    found->push_back({line, range.column, commented.key, commented.rest});
    if (FindCommentedChain(lines, children, keys, depth + 1, accept_last,
                           found)) {
      return true;
    }
    found->pop_back();
  }
  return false;
}

std::size_t DetectIndentStep(const Layout& layout) {
  for (const LayoutNode& node : layout.nodes) {
    if (node.kind == NodeKind::kMapping && !node.flow && node.has_key &&
        node.child_column > static_cast<long>(node.key_start.column)) {
      return static_cast<std::size_t>(node.child_column) -
             node.key_start.column;
    }
  }
  return kDefaultIndentStep;
}

// Inserts complete lines (each ending in a break) before line `line`
// (count() appends at the end, adding a break to an unterminated last line).
void InsertBeforeLine(std::string* text, const TextLines& lines,
                      std::size_t line, const std::string& block) {
  if (line >= lines.count()) {
    std::string prefix;
    if (!text->empty() && !lines.has_break(lines.count() - 1)) {
      prefix = std::string(lines.newline());
    }
    text->append(prefix + block);
    return;
  }
  text->insert(lines.start(line), block);
}

void InsertAfterLine(std::string* text, const TextLines& lines,
                     std::size_t line, const std::string& block) {
  if (line + 1 >= lines.count()) {
    InsertBeforeLine(text, lines, lines.count(), block);
    return;
  }
  text->insert(lines.next(line), block);
}

bool LineIsBlank(const TextLines& lines, std::size_t line) {
  std::size_t column = 0;
  return line < lines.count() &&
         ClassifyLine(lines.content(line), &column) == LineKind::kBlank;
}

// Appends a new top-level section, separated from the text above by one
// blank line.
void AppendSection(std::string* text, const TextLines& lines,
                   const std::string& block) {
  std::string prefix;
  const bool empty = text->empty();
  if (!empty && !lines.has_break(lines.count() - 1)) {
    prefix += lines.newline();
  }
  if (!empty && !LineIsBlank(lines, lines.count() - 1)) {
    prefix += lines.newline();
  }
  text->append(prefix + block);
}

class EmptyEnvironment final : public runtime::Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

void ReplaceAllOccurrences(std::string* text, const std::string& from,
                           std::string_view to) {
  if (from.empty()) {
    return;
  }
  std::size_t position = 0;
  while ((position = text->find(from, position)) != std::string::npos) {
    text->replace(position, from.size(), to);
    position += to.size();
  }
}

// Adds "(line N)" to a loader message that names one key.
std::string WithLineHint(std::string message, const Layout& layout) {
  std::string candidate = message.substr(0, message.find(' '));
  const std::string unknown_updates = "unknown updates key: ";
  const std::string unknown_runtime = "unknown runtime configuration key: ";
  if (message.rfind(unknown_updates, 0) == 0) {
    candidate = "updates." + message.substr(unknown_updates.size());
  } else if (message.rfind(unknown_runtime, 0) == 0) {
    candidate = message.substr(unknown_runtime.size());
  }
  const int node = layout.Find(candidate);
  if (node < 0) {
    return message;
  }
  const LayoutNode& found = layout.nodes[node];
  const std::size_t line =
      (found.has_key ? found.key_start.line : found.start.line) + 1;
  return message + " (line " + std::to_string(line) + ")";
}

std::string DescribeParseProblem(const Layout& layout) {
  if (!layout.has_problem_mark) {
    return "invalid YAML: " + layout.problem;
  }
  return "invalid YAML at line " +
         std::to_string(layout.problem_mark.line + 1) + ", column " +
         std::to_string(layout.problem_mark.column + 1) + ": " +
         layout.problem;
}

// The block of template lines for a mapping: its leading comments, its key
// and every line of its body, re-indented by `shift` columns.
bool TemplateBlockText(const TextLines& lines, const LayoutNode& node,
                       long shift, std::string_view newline,
                       std::string* block) {
  const std::size_t key_line = node.key_start.line;
  const std::size_t key_column = node.key_start.column;
  const std::size_t first = LeadingCommentStart(lines, key_line, key_column);
  std::size_t last = node.last_line;
  const std::size_t end = std::min(node.range_end, lines.count());
  for (std::size_t line = node.last_line + 1; line < end; ++line) {
    std::size_t column = 0;
    const LineKind kind = ClassifyLine(lines.content(line), &column);
    if (kind == LineKind::kBlank) {
      continue;
    }
    if (kind == LineKind::kComment && column > key_column) {
      last = line;
      continue;
    }
    break;
  }
  std::string text;
  for (std::size_t line = first; line <= last; ++line) {
    std::string content(lines.content(line));
    std::size_t column = 0;
    if (ClassifyLine(content, &column) != LineKind::kBlank) {
      if (shift > 0) {
        content.insert(0, Spaces(static_cast<std::size_t>(shift)));
      } else if (shift < 0) {
        const std::size_t remove = static_cast<std::size_t>(-shift);
        if (column < remove) {
          return false;
        }
        content.erase(0, remove);
      }
    } else {
      content.clear();
    }
    text += content;
    text += newline;
  }
  *block = std::move(text);
  return true;
}

class Editor {
 public:
  Editor(std::string text, const std::string& template_yaml)
      : text_(std::move(text)), template_yaml_(template_yaml) {}

  const std::string& text() const { return text_; }

  bool Set(const std::vector<std::string>& parts, std::string_view value,
           const std::string& formatted, bool force_scalar,
           std::string* error) {
    const std::string path = JoinParts(parts, parts.size());
    // Each pass either finishes or copies one missing block from the
    // template and tries again, so a few passes always suffice.
    for (std::size_t pass = 0; pass <= parts.size() + 1; ++pass) {
      const Layout layout = internal::BuildLayout(text_);
      if (!CheckEditable(layout, parts, error)) {
        return false;
      }
      const TextLines lines(text_);
      const FlatMap before = Flatten(layout);
      const int existing = layout.Find(path);
      if (existing >= 0) {
        // The loader ignores quoting style, so an equal value is left as
        // written (a UI may write back every setting it shows).
        if (layout.nodes[existing].kind == NodeKind::kScalar &&
            layout.nodes[existing].value == value) {
          return true;
        }
        return ReplaceExisting(layout, lines, before, existing, path,
                               formatted, force_scalar, error);
      }

      int ancestor = 0;
      std::size_t depth = 0;
      int null_parent = -1;
      for (; depth + 1 < parts.size(); ++depth) {
        const std::string prefix = JoinParts(parts, depth + 1);
        const int node = layout.Find(prefix);
        if (node < 0) {
          break;
        }
        const LayoutNode& found = layout.nodes[node];
        if (found.kind == NodeKind::kMapping) {
          ancestor = node;
          continue;
        }
        if (found.is_null() && found.has_key) {
          null_parent = node;
          ++depth;
          break;
        }
        *error = prefix + " is not a section, so " + path +
                 " cannot be added to it";
        return false;
      }
      const std::vector<std::string> missing(parts.begin() + depth,
                                             parts.end());
      const auto new_or_ancestor = [&path](const std::string& candidate) {
        return IsAncestorOrSelf(candidate, path);
      };

      if (null_parent >= 0) {
        const LayoutNode& header = layout.nodes[null_parent];
        const std::size_t step = DetectIndentStep(layout);
        std::string candidate = text_;
        InsertAfterLine(&candidate, lines, header.key_start.line,
                        ChainLines(header.key_start.column + step, step,
                                   missing, formatted, lines.newline()));
        return Commit(std::move(candidate), before, new_or_ancestor, path,
                      formatted, error);
      }

      const LayoutNode& parent = layout.nodes[ancestor];
      if (parent.flow) {
        *error = parent.path + " is written in flow style ({...}); add " +
                 path + " to it by hand";
        return false;
      }

      std::string candidate;
      if (TryUncomment(layout, lines, ancestor, missing, formatted,
                       &candidate) &&
          Commit(candidate, before, new_or_ancestor, path, formatted,
                 nullptr)) {
        return true;
      }

      if (missing.size() >= 2 || layout.nodes[ancestor].path.empty()) {
        FlatMap added;
        const std::string block_path =
            parent.path.empty() ? missing.front()
                                : parent.path + "." + missing.front();
        if (InsertTemplateBlock(layout, lines, ancestor, block_path,
                                &candidate, &added)) {
          const Layout inserted = internal::BuildLayout(candidate);
          if (inserted.parsed &&
              OnlyChangedWhere(before, Flatten(inserted),
                               [&block_path](const std::string& changed) {
                                 return IsUnder(changed, block_path);
                               })) {
            FlatMap after_block;
            for (const auto& [changed, value] : Flatten(inserted)) {
              if (IsUnder(changed, block_path)) {
                after_block.emplace(changed, value);
              }
            }
            if (after_block == added) {
              text_ = std::move(candidate);
              continue;
            }
          }
        }
      }

      candidate = text_;
      const std::size_t column =
          parent.child_column >= 0
              ? static_cast<std::size_t>(parent.child_column)
              : 0;
      std::size_t step = DetectIndentStep(layout);
      if (parent.has_key &&
          parent.child_column > static_cast<long>(parent.key_start.column)) {
        step = static_cast<std::size_t>(parent.child_column) -
               parent.key_start.column;
      }
      const std::string block =
          ChainLines(column, step, missing, formatted, lines.newline());
      if (parent.path.empty()) {
        AppendSection(&candidate, lines, block);
      } else {
        InsertAfterLine(&candidate, lines, ChildrenEndLine(lines, parent),
                        block);
      }
      return Commit(std::move(candidate), before, new_or_ancestor, path,
                    formatted, error);
    }
    *error = "cannot place " + path + " in config.yaml";
    return false;
  }

  bool Unset(const std::vector<std::string>& parts, std::string* error) {
    const std::string path = JoinParts(parts, parts.size());
    const Layout layout = internal::BuildLayout(text_);
    if (!CheckEditable(layout, parts, error)) {
      return false;
    }
    const int node = layout.Find(path);
    if (node < 0) {
      return true;
    }
    const LayoutNode& target = layout.nodes[node];
    if (target.kind != NodeKind::kScalar) {
      *error = path + " is not a single value";
      return false;
    }
    const TextLines lines(text_);
    struct CommentMark {
      std::size_t line;
      std::size_t column;
    };
    std::vector<CommentMark> marks;
    marks.push_back({target.key_start.line, target.key_start.column});
    for (std::size_t line = target.key_start.line + 1;
         line <= target.last_line && line < lines.count(); ++line) {
      std::size_t column = 0;
      if (ClassifyLine(lines.content(line), &column) == LineKind::kContent) {
        marks.push_back({line, column});
      }
    }
    std::vector<std::string> emptied;
    int current = target.parent;
    while (current > 0 && layout.nodes[current].children.size() == 1) {
      const LayoutNode& section = layout.nodes[current];
      if (section.flow || !section.has_key) {
        *error = section.path + " is written in flow style ({...}); remove " +
                 path + " by hand";
        return false;
      }
      emptied.push_back(section.path);
      marks.push_back({section.key_start.line, section.key_start.column});
      current = section.parent;
    }
    if (layout.nodes[current].flow) {
      *error = layout.nodes[current].path +
               " is written in flow style ({...}); remove " + path +
               " by hand";
      return false;
    }
    if (current == 0 && layout.nodes[0].children.size() == 1) {
      *error = "cannot comment out the only setting in config.yaml";
      return false;
    }
    std::sort(marks.begin(), marks.end(),
              [](const CommentMark& left, const CommentMark& right) {
                return left.line > right.line;
              });
    std::string candidate = text_;
    for (const CommentMark& mark : marks) {
      candidate.insert(lines.ByteOffset(mark.line, mark.column), "# ");
    }
    const Layout after = internal::BuildLayout(candidate);
    const auto removable = [&path, &emptied](const std::string& changed) {
      if (changed == path) {
        return true;
      }
      return std::find(emptied.begin(), emptied.end(), changed) !=
             emptied.end();
    };
    if (!after.parsed ||
        !OnlyChangedWhere(Flatten(layout), Flatten(after), removable) ||
        after.Find(path) >= 0) {
      *error = "cannot comment out " + path + " without changing other "
               "settings; edit config.yaml by hand";
      return false;
    }
    for (const std::string& section : emptied) {
      if (after.Find(section) >= 0) {
        *error = "cannot comment out " + path + " cleanly";
        return false;
      }
    }
    text_ = std::move(candidate);
    return true;
  }

 private:
  static bool CheckEditable(const Layout& layout,
                            const std::vector<std::string>& parts,
                            std::string* error) {
    if (!layout.parsed) {
      *error = "config.yaml cannot be edited: " + DescribeParseProblem(layout);
      return false;
    }
    if (layout.nodes.empty() || layout.nodes[0].kind != NodeKind::kMapping) {
      *error = "config.yaml must contain a mapping of sections";
      return false;
    }
    for (std::size_t count = 1; count <= parts.size(); ++count) {
      const std::string prefix = JoinParts(parts, count);
      if (layout.duplicate_paths.count(prefix) != 0) {
        *error = "config.yaml contains " + prefix +
                 " more than once; remove the duplicate first";
        return false;
      }
    }
    return true;
  }

  static std::string ChainLines(std::size_t column, std::size_t step,
                                const std::vector<std::string>& keys,
                                const std::string& formatted,
                                std::string_view newline) {
    std::string block;
    for (std::size_t index = 0; index < keys.size(); ++index) {
      block += Spaces(column + index * step) + keys[index] + ":";
      if (index + 1 == keys.size()) {
        block += " " + formatted;
      }
      block += newline;
    }
    return block;
  }

  bool Commit(std::string candidate, const FlatMap& before,
              const std::function<bool(const std::string&)>& changeable,
              const std::string& path, const std::string& formatted,
              std::string* error) {
    const Layout after = internal::BuildLayout(candidate);
    bool valid = after.parsed && OnlyChangedWhere(before, Flatten(after),
                                                  changeable);
    if (valid) {
      const int node = after.Find(path);
      valid = node >= 0 && after.nodes[node].kind == NodeKind::kScalar &&
              after.duplicate_paths.count(path) == 0;
      if (valid) {
        // The written text must read back as the requested value.
        const Layout expected = internal::BuildLayout("v: " + formatted);
        const int value = expected.Find("v");
        valid = value >= 0 &&
                expected.nodes[value].value == after.nodes[node].value;
      }
    }
    if (!valid) {
      if (error != nullptr) {
        *error = "cannot change " + path +
                 " without disturbing other settings; edit config.yaml by "
                 "hand";
      }
      return false;
    }
    text_ = std::move(candidate);
    return true;
  }

  bool ReplaceExisting(const Layout& layout, const TextLines& lines,
                       const FlatMap& before, int index,
                       const std::string& path, const std::string& formatted,
                       bool force_scalar, std::string* error) {
    const LayoutNode& node = layout.nodes[index];
    std::string candidate = text_;
    if (node.kind == NodeKind::kScalar) {
      if (node.block_scalar || node.start.line != node.last_line) {
        *error = path + " spans several lines; edit it in config.yaml by hand";
        return false;
      }
      const std::size_t begin =
          lines.ByteOffset(node.start.line, node.start.column);
      const std::size_t end = lines.ByteOffset(node.end.line, node.end.column);
      if (begin == end) {
        candidate.insert(begin, " " + formatted);
      } else {
        candidate.replace(begin, end - begin, formatted);
      }
      return Commit(std::move(candidate), before,
                    [&path](const std::string& changed) {
                      return changed == path;
                    },
                    path, formatted, error);
    }
    if (node.kind != NodeKind::kMapping) {
      *error = path + " is not a single value; edit it in config.yaml by hand";
      return false;
    }
    if (!force_scalar) {
      *error = path + " holds a detailed mapping; confirm replacing it with "
                      "a single value";
      return false;
    }
    if (!node.has_key) {
      *error = "cannot replace the whole configuration";
      return false;
    }
    if (node.flow) {
      if (node.start.line != node.last_line) {
        *error = path + " spans several lines; edit it in config.yaml by hand";
        return false;
      }
      const std::size_t begin =
          lines.ByteOffset(node.start.line, node.start.column);
      const std::size_t end = lines.ByteOffset(node.end.line, node.end.column);
      candidate.replace(begin, end - begin, formatted);
    } else {
      // Keep the old block as comments, in the template's "# device:" shape.
      for (std::size_t line = node.last_line; line > node.key_start.line;
           --line) {
        std::size_t column = 0;
        if (ClassifyLine(lines.content(line), &column) != LineKind::kContent) {
          continue;
        }
        candidate.insert(lines.ByteOffset(line, node.key_start.column), "# ");
      }
      const std::size_t key_end =
          lines.ByteOffset(node.key_end.line, node.key_end.column);
      std::size_t colon = key_end;
      while (colon < lines.content_end(node.key_end.line) &&
             (text_[colon] == ' ' || text_[colon] == '\t')) {
        ++colon;
      }
      if (colon >= lines.content_end(node.key_end.line) ||
          text_[colon] != ':') {
        *error = "cannot find the ':' after " + path;
        return false;
      }
      candidate.insert(colon + 1, " " + formatted);
    }
    return Commit(std::move(candidate), before,
                  [&path](const std::string& changed) {
                    return IsUnder(changed, path);
                  },
                  path, formatted, error);
  }

  // Template text for the commented example of the same key, which may hold
  // prose-like values such as "Join Server".
  std::optional<std::string> TemplateCommentRest(
      const std::string& ancestor_path,
      const std::vector<std::string>& missing) const {
    const Layout layout = internal::BuildLayout(template_yaml_);
    if (!layout.parsed || layout.nodes.empty()) {
      return std::nullopt;
    }
    const int ancestor = ancestor_path.empty() ? 0 : layout.Find(ancestor_path);
    if (ancestor < 0 || layout.nodes[ancestor].kind != NodeKind::kMapping ||
        layout.nodes[ancestor].flow) {
      return std::nullopt;
    }
    const TextLines lines(template_yaml_);
    std::vector<FoundComment> found;
    if (!FindCommentedChain(lines, MappingRange(layout, lines, ancestor),
                            missing, 0,
                            [](const std::string&) { return true; }, &found)) {
      return std::nullopt;
    }
    return found.back().rest;
  }

  bool TryUncomment(const Layout& layout, const TextLines& lines, int ancestor,
                    const std::vector<std::string>& missing,
                    const std::string& formatted,
                    std::string* candidate) const {
    const std::optional<std::string> template_rest =
        TemplateCommentRest(layout.nodes[ancestor].path, missing);
    const auto accept = [&template_rest](const std::string& rest) {
      return internal::LooksLikeCommentedValue(rest) ||
             (template_rest.has_value() && rest == *template_rest);
    };
    std::vector<FoundComment> found;
    if (!FindCommentedChain(lines, MappingRange(layout, lines, ancestor),
                            missing, 0, accept, &found)) {
      return false;
    }
    std::string text = text_;
    for (std::size_t index = found.size(); index-- > 0;) {
      const FoundComment& comment = found[index];
      std::string line = Spaces(comment.column) + comment.key + ":";
      const bool last = index + 1 == found.size();
      if (last) {
        line += " " + formatted;
      }
      const std::string comment_text =
          last ? TrailingComment(comment.rest)
               : (comment.rest.empty() ? std::string() : comment.rest);
      if (!comment_text.empty()) {
        line += "  " + comment_text;
      }
      const std::size_t begin = lines.start(comment.line);
      text.replace(begin, lines.content_end(comment.line) - begin, line);
    }
    *candidate = std::move(text);
    return true;
  }

  // Copies the template's block for block_path (comments included) into the
  // document at the template's position. *added receives the template's
  // flattened values for the block.
  bool InsertTemplateBlock(const Layout& layout, const TextLines& lines,
                           int ancestor, const std::string& block_path,
                           std::string* candidate, FlatMap* added) const {
    const Layout source = internal::BuildLayout(template_yaml_);
    if (!source.parsed || source.nodes.empty()) {
      return false;
    }
    const int block = source.Find(block_path);
    if (block < 0 || source.nodes[block].kind != NodeKind::kMapping ||
        source.nodes[block].flow || !source.nodes[block].has_key) {
      return false;
    }
    const int source_parent = source.nodes[block].parent;
    const LayoutNode& parent = layout.nodes[ancestor];
    const long shift =
        (parent.child_column >= 0 ? parent.child_column : 0) -
        (source.nodes[source_parent].child_column >= 0
             ? source.nodes[source_parent].child_column
             : 0);
    const TextLines source_lines(template_yaml_);
    std::string text;
    if (!TemplateBlockText(source_lines, source.nodes[block], shift,
                           lines.newline(), &text)) {
      return false;
    }

    // The template's next sibling that the document has decides where the
    // block goes; otherwise it ends the parent.
    int following = -1;
    const std::vector<int>& siblings = source.nodes[source_parent].children;
    const auto position = std::find(siblings.begin(), siblings.end(), block);
    for (auto next = position == siblings.end() ? siblings.end()
                                                : position + 1;
         next != siblings.end(); ++next) {
      const int found = layout.Find(source.nodes[*next].path);
      if (found >= 0 && layout.nodes[found].parent == ancestor &&
          layout.nodes[found].has_key) {
        following = found;
        break;
      }
    }

    std::string result = text_;
    if (following >= 0) {
      const LayoutNode& next = layout.nodes[following];
      const std::size_t line =
          LeadingCommentStart(lines, next.key_start.line,
                              next.key_start.column);
      if (parent.path.empty()) {
        if (line > 0 && !LineIsBlank(lines, line - 1)) {
          text.insert(0, lines.newline());
        }
        text += lines.newline();
      }
      InsertBeforeLine(&result, lines, line, text);
    } else if (parent.path.empty()) {
      AppendSection(&result, lines, text);
    } else {
      InsertAfterLine(&result, lines, ChildrenEndLine(lines, parent), text);
    }

    added->clear();
    for (const auto& [path, value] : Flatten(source)) {
      if (IsUnder(path, block_path)) {
        added->emplace(path, value);
      }
    }
    *candidate = std::move(result);
    return true;
  }

  std::string text_;
  const std::string& template_yaml_;
};

}  // namespace

ConfigDocument::ConfigDocument()
    : template_yaml_(runtime::DefaultRuntimeConfigYaml()) {}

bool ConfigDocument::Load(const std::filesystem::path& path,
                          ConfigDocument* out, std::string* error) {
  std::string bytes;
  FileIdentity identity;
  if (!internal::ReadRegularFile(path, kMaximumConfigBytes, &bytes, &identity,
                                 error)) {
    return false;
  }
  ConfigDocument document;
  document.identity_ = identity;
  if (identity.exists) {
    document.disk_bytes_ = bytes;
    document.bytes_ = std::move(bytes);
  } else {
    document.bytes_ = std::string(runtime::DefaultRuntimeConfigYaml());
  }
  *out = std::move(document);
  return true;
}

ConfigDocument ConfigDocument::FromBytes(std::string bytes) {
  ConfigDocument document;
  document.bytes_ = std::move(bytes);
  return document;
}

std::filesystem::path ConfigDocument::BackupPath(
    const std::filesystem::path& config_file) {
  return config_file.string() + ".launcher-backup";
}

void ConfigDocument::SetTemplate(std::string template_yaml) {
  template_yaml_ = std::move(template_yaml);
}

void ConfigDocument::ReplaceAll(std::string bytes) {
  bytes_ = std::move(bytes);
}

std::optional<std::string> ConfigDocument::Get(std::string_view dotted) const {
  const Layout layout = internal::BuildLayout(bytes_);
  const int node = layout.parsed ? layout.Find(dotted) : -1;
  if (node < 0 || layout.nodes[node].kind != NodeKind::kScalar ||
      layout.nodes[node].path.empty()) {
    return std::nullopt;
  }
  return layout.nodes[node].value;
}

bool ConfigDocument::IsMapping(std::string_view dotted) const {
  const Layout layout = internal::BuildLayout(bytes_);
  const int node = layout.parsed ? layout.Find(dotted) : -1;
  return node >= 0 && layout.nodes[node].kind == NodeKind::kMapping;
}

bool ConfigDocument::Set(std::string_view dotted, std::string_view value,
                         ScalarKind kind, std::string* error,
                         bool force_scalar) {
  std::vector<std::string> parts;
  if (!SplitPath(dotted, &parts, error)) {
    return false;
  }
  std::string formatted;
  if (!FormatScalar(value, kind, &formatted, error)) {
    *error = std::string(dotted) + ": " + *error;
    return false;
  }
  Editor editor(bytes_, template_yaml_);
  if (!editor.Set(parts, value, formatted, force_scalar, error)) {
    return false;
  }
  bytes_ = editor.text();
  return true;
}

bool ConfigDocument::Unset(std::string_view dotted, std::string* error) {
  std::vector<std::string> parts;
  if (!SplitPath(dotted, &parts, error)) {
    return false;
  }
  Editor editor(bytes_, template_yaml_);
  if (!editor.Unset(parts, error)) {
    return false;
  }
  bytes_ = editor.text();
  return true;
}

bool ConfigDocument::Validate(std::string* error) const {
  // libyaml's document loader reports a position the runtime loader drops.
  {
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
      *error = "cannot initialize the YAML parser";
      return false;
    }
    yaml_parser_set_input_string(
        &parser, reinterpret_cast<const unsigned char*>(bytes_.data()),
        bytes_.size());
    yaml_document_t document;
    const bool loaded = yaml_parser_load(&parser, &document) != 0;
    if (loaded) {
      yaml_document_delete(&document);
    } else {
      Layout problem;
      problem.problem = parser.problem != nullptr ? parser.problem
                                                  : "invalid YAML";
      if (parser.context != nullptr) {
        problem.problem += std::string(" ") + parser.context;
      }
      problem.has_problem_mark = true;
      problem.problem_mark = {parser.problem_mark.line,
                              parser.problem_mark.column};
      *error = DescribeParseProblem(problem);
    }
    yaml_parser_delete(&parser);
    if (!loaded) {
      return false;
    }
  }

  std::error_code filesystem_error;
  std::filesystem::path base =
      std::filesystem::temp_directory_path(filesystem_error);
  if (filesystem_error || base.empty()) {
    base = "/tmp";
  }
  std::string pattern = (base / "mocktail-config-check-XXXXXX").string();
  if (mkdtemp(pattern.data()) == nullptr) {
    *error = "cannot create a private directory to check config.yaml";
    return false;
  }
  const std::filesystem::path directory = pattern;
  const std::filesystem::path file = directory / "config.yaml";
  bool created = false;
  std::string write_error;
  if (!internal::CreateExclusiveFile(file, bytes_, kPrivateFileMode, &created,
                                     &write_error) ||
      !created) {
    (void)rmdir(directory.c_str());
    *error = "cannot write config.yaml for checking: " + write_error;
    return false;
  }
  std::string message;
  const runtime::RuntimeConfigLoadResult runtime_result =
      runtime::LoadRuntimeConfig(EmptyEnvironment(), file);
  if (!runtime_result) {
    message = runtime_result.error;
  } else {
    const update::UpdateConfigResult update_result =
        update::LoadUpdateConfig(file);
    if (!update_result) {
      message = update_result.error;
    }
  }
  (void)unlink(file.c_str());
  (void)rmdir(directory.c_str());
  if (message.empty()) {
    return true;
  }
  ReplaceAllOccurrences(&message, file.string(), "config.yaml");
  *error = WithLineHint(std::move(message), internal::BuildLayout(bytes_));
  return false;
}

bool ConfigDocument::Save(const std::filesystem::path& path,
                          std::string* error) {
  if (!Validate(error)) {
    return false;
  }
  if (!internal::VerifyUnchanged(path, identity_, disk_bytes_,
                                 kMaximumConfigBytes, error)) {
    return false;
  }
  if (identity_.exists) {
    bool created = false;
    if (!internal::CreateExclusiveFile(BackupPath(path), disk_bytes_,
                                       kPrivateFileMode, &created, error)) {
      return false;
    }
  }
  FileIdentity published;
  if (!internal::PublishAtomically(path, bytes_, kPrivateFileMode, identity_,
                                   &published, error)) {
    return false;
  }
  identity_ = published;
  disk_bytes_ = bytes_;
  return true;
}

}  // namespace mocktail::launcher
