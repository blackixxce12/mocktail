#ifndef MOCKTAIL_LAUNCHER_YAML_LAYOUT_H_
#define MOCKTAIL_LAUNCHER_YAML_LAYOUT_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mocktail::launcher::internal {

// Line table that splits text exactly where libyaml counts line breaks
// (LF, CR LF, CR, NEL, LS and PS), so libyaml marks can be mapped to bytes.
class TextLines {
 public:
  explicit TextLines(std::string_view text);

  std::size_t count() const { return starts_.size(); }
  std::size_t start(std::size_t line) const;
  // End of the line's text, before its line break.
  std::size_t content_end(std::size_t line) const;
  // Start of the next line, or the text size for the last line.
  std::size_t next(std::size_t line) const;
  std::string_view content(std::size_t line) const;
  bool has_break(std::size_t line) const {
    return content_end(line) != next(line);
  }

  // libyaml columns count characters, not bytes: walk the UTF-8 sequences of
  // the line. A line at or past count() maps to the end of the text.
  std::size_t ByteOffset(std::size_t line, std::size_t column) const;

  // The break used for new lines: the file's first CR LF or LF.
  std::string_view newline() const { return newline_; }

 private:
  std::string_view text_;
  std::vector<std::size_t> starts_;
  std::vector<std::size_t> content_ends_;
  std::size_t first_line_skip_ = 0;
  std::string_view newline_ = "\n";
};

enum class LineKind { kBlank, kComment, kContent };

// Classifies one line's content; *column is the column of its first
// non-blank character (indentation is ASCII, so bytes equal characters).
LineKind ClassifyLine(std::string_view content, std::size_t* column);

// "<h spaces>#<s spaces>key: rest". Removing the '#' and one following space
// puts the key at key_column = h + max(s - 1, 0).
struct CommentedKey {
  std::size_t hash_column = 0;
  std::size_t key_column = 0;
  std::string key;
  std::string rest;  // after "key:", without surrounding blanks
};
bool ParseCommentedKey(std::string_view content, CommentedKey* parsed);

// True when the text after "key:" in a commented line looks like a value
// rather than prose: empty, one quoted scalar, or one token without blanks,
// optionally followed by " # comment".
bool LooksLikeCommentedValue(std::string_view rest);

struct Mark {
  std::size_t line = 0;
  std::size_t column = 0;
};

enum class NodeKind { kScalar, kMapping, kSequence, kAlias };

struct LayoutNode {
  NodeKind kind = NodeKind::kScalar;
  std::string path;  // dotted; "" for the root
  int parent = -1;
  bool has_key = false;
  Mark key_start;
  Mark key_end;
  Mark start;
  Mark end;
  bool flow = false;          // flow collection ({...} or [...])
  bool block_scalar = false;  // literal or folded scalar
  bool plain = false;
  std::string value;
  // Last line holding a token of this node (inclusive).
  std::size_t last_line = 0;
  // Collections: the line of the first token after the node (exclusive), so
  // comment lines after the last value are inside [key line, range_end).
  std::size_t range_end = 0;
  // Mappings: column of the first key, -1 if unknown.
  long child_column = -1;
  std::vector<int> children;

  bool is_null() const {
    return kind == NodeKind::kScalar && plain && value.empty();
  }
};

struct Layout {
  bool parsed = false;
  std::string problem;
  bool has_problem_mark = false;
  Mark problem_mark;
  std::vector<LayoutNode> nodes;  // nodes[0] is the root when parsed
  std::unordered_map<std::string, int> by_path;
  std::unordered_set<std::string> duplicate_paths;

  int Find(std::string_view path) const;
};

// Indexes the first YAML document of text with libyaml events.
Layout BuildLayout(std::string_view text);

}  // namespace mocktail::launcher::internal

#endif  // MOCKTAIL_LAUNCHER_YAML_LAYOUT_H_
