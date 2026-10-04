#include "yaml_layout.h"

#include <yaml.h>

#include <algorithm>
#include <utility>

namespace mocktail::launcher::internal {
namespace {

bool IsBlank(char character) { return character == ' ' || character == '\t'; }

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsBlank(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsBlank(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

bool IsKeyCharacter(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_' ||
         character == '-';
}

std::size_t Utf8SequenceLength(unsigned char lead) {
  if (lead < 0x80U) {
    return 1;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((lead & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

// The text after a quoted scalar or plain token must be empty or a comment.
bool OnlyCommentFollows(std::string_view after) {
  if (after.empty()) {
    return true;
  }
  if (!IsBlank(after.front())) {
    return false;
  }
  after = Trim(after);
  return after.empty() || after.front() == '#';
}

Mark ToMark(const yaml_mark_t& mark) { return {mark.line, mark.column}; }

std::string JoinPath(const std::string& parent, std::string_view key) {
  return parent.empty() ? std::string(key)
                        : parent + "." + std::string(key);
}

struct Frame {
  int node = -1;
  bool expect_key = true;
  std::string key;
  Mark key_start;
  Mark key_end;
  std::size_t sequence_index = 0;
};

class LayoutBuilder {
 public:
  explicit LayoutBuilder(Layout* layout) : layout_(layout) {}

  // Returns false when the event stream cannot be indexed (complex keys).
  bool OnNode(const yaml_event_t& event) {
    LayoutNode node;
    node.start = ToMark(event.start_mark);
    node.end = ToMark(event.end_mark);
    if (!stack_.empty()) {
      Frame& parent = stack_.back();
      LayoutNode& parent_node = layout_->nodes[parent.node];
      if (parent_node.kind == NodeKind::kMapping) {
        if (parent.expect_key) {
          if (event.type != YAML_SCALAR_EVENT) {
            layout_->problem = "complex mapping keys are not supported";
            return false;
          }
          parent.key.assign(
              reinterpret_cast<const char*>(event.data.scalar.value),
              event.data.scalar.length);
          parent.key_start = ToMark(event.start_mark);
          parent.key_end = ToMark(event.end_mark);
          parent.expect_key = false;
          if (parent_node.child_column < 0) {
            parent_node.child_column =
                static_cast<long>(parent.key_start.column);
          }
          Touch(parent.key_end.line);
          return true;
        }
        node.path = JoinPath(parent_node.path, parent.key);
        node.has_key = true;
        node.key_start = parent.key_start;
        node.key_end = parent.key_end;
        parent.expect_key = true;
      } else {
        node.path = parent_node.path + "[" +
                    std::to_string(parent.sequence_index++) + "]";
      }
      node.parent = parent.node;
    }

    switch (event.type) {
      case YAML_SCALAR_EVENT:
        node.kind = NodeKind::kScalar;
        node.value.assign(
            reinterpret_cast<const char*>(event.data.scalar.value),
            event.data.scalar.length);
        node.plain = event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
        node.block_scalar =
            event.data.scalar.style == YAML_LITERAL_SCALAR_STYLE ||
            event.data.scalar.style == YAML_FOLDED_SCALAR_STYLE;
        node.last_line = node.end.line;
        // A block scalar ends at the start of the line after its text.
        if (node.end.column == 0 && node.end.line > node.start.line) {
          node.last_line = node.end.line - 1;
        }
        break;
      case YAML_ALIAS_EVENT:
        node.kind = NodeKind::kAlias;
        node.last_line = node.end.line;
        break;
      case YAML_MAPPING_START_EVENT:
        node.kind = NodeKind::kMapping;
        node.flow = event.data.mapping_start.style == YAML_FLOW_MAPPING_STYLE;
        node.last_line = node.start.line;
        break;
      default:
        node.kind = NodeKind::kSequence;
        node.flow =
            event.data.sequence_start.style == YAML_FLOW_SEQUENCE_STYLE;
        node.last_line = node.start.line;
        break;
    }

    const int index = static_cast<int>(layout_->nodes.size());
    const std::size_t last_line = node.last_line;
    const bool collection = node.kind == NodeKind::kMapping ||
                            node.kind == NodeKind::kSequence;
    const std::string path = node.path;
    if (node.parent >= 0) {
      layout_->nodes[node.parent].children.push_back(index);
    }
    layout_->nodes.push_back(std::move(node));
    if (!layout_->by_path.emplace(path, index).second) {
      layout_->duplicate_paths.insert(path);
    }
    if (collection) {
      Frame frame;
      frame.node = index;
      stack_.push_back(std::move(frame));
    } else {
      Touch(last_line);
    }
    return true;
  }

  void OnCollectionEnd(const yaml_event_t& event) {
    if (stack_.empty()) {
      return;
    }
    LayoutNode& node = layout_->nodes[stack_.back().node];
    const Mark mark = ToMark(event.start_mark);
    if (node.flow) {
      node.last_line = std::max(node.last_line,
                                static_cast<std::size_t>(event.end_mark.line));
      node.range_end = node.last_line + 1;
    } else {
      node.range_end = std::max(mark.line, node.last_line + 1);
    }
    const std::size_t last_line = node.last_line;
    stack_.pop_back();
    Touch(last_line);
  }

 private:
  void Touch(std::size_t line) {
    for (const Frame& frame : stack_) {
      LayoutNode& node = layout_->nodes[frame.node];
      node.last_line = std::max(node.last_line, line);
    }
  }

  Layout* layout_;
  std::vector<Frame> stack_;
};

}  // namespace

TextLines::TextLines(std::string_view text) : text_(text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFU &&
      static_cast<unsigned char>(text[1]) == 0xBBU &&
      static_cast<unsigned char>(text[2]) == 0xBFU) {
    first_line_skip_ = 3;
  }
  bool newline_known = false;
  starts_.push_back(0);
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char character = static_cast<unsigned char>(text[index]);
    std::size_t break_length = 0;
    if (character == '\n') {
      break_length = 1;
      if (!newline_known) {
        newline_ = "\n";
        newline_known = true;
      }
    } else if (character == '\r') {
      const bool crlf = index + 1 < text.size() && text[index + 1] == '\n';
      break_length = crlf ? 2 : 1;
      if (!newline_known) {
        newline_ = crlf ? "\r\n" : "\r";
        newline_known = true;
      }
    } else if (character == 0xC2U && index + 1 < text.size() &&
               static_cast<unsigned char>(text[index + 1]) == 0x85U) {
      break_length = 2;
    } else if (character == 0xE2U && index + 2 < text.size() &&
               static_cast<unsigned char>(text[index + 1]) == 0x80U &&
               (static_cast<unsigned char>(text[index + 2]) == 0xA8U ||
                static_cast<unsigned char>(text[index + 2]) == 0xA9U)) {
      break_length = 3;
    }
    if (break_length == 0) {
      ++index;
      continue;
    }
    content_ends_.push_back(index);
    index += break_length;
    if (index < text.size()) {
      starts_.push_back(index);
    }
  }
  if (content_ends_.size() < starts_.size()) {
    content_ends_.push_back(text.size());
  }
}

std::size_t TextLines::start(std::size_t line) const {
  return line < count() ? starts_[line] : text_.size();
}

std::size_t TextLines::content_end(std::size_t line) const {
  return line < count() ? content_ends_[line] : text_.size();
}

std::size_t TextLines::next(std::size_t line) const {
  return line + 1 < count() ? starts_[line + 1] : text_.size();
}

std::string_view TextLines::content(std::size_t line) const {
  if (line >= count()) {
    return {};
  }
  return text_.substr(starts_[line], content_ends_[line] - starts_[line]);
}

std::size_t TextLines::ByteOffset(std::size_t line, std::size_t column) const {
  if (line >= count()) {
    return text_.size();
  }
  std::size_t offset = starts_[line] + (line == 0 ? first_line_skip_ : 0);
  const std::size_t end = content_ends_[line];
  for (std::size_t walked = 0; walked < column && offset < end; ++walked) {
    offset += Utf8SequenceLength(static_cast<unsigned char>(text_[offset]));
  }
  return std::min(offset, end);
}

LineKind ClassifyLine(std::string_view content, std::size_t* column) {
  std::size_t index = 0;
  while (index < content.size() && IsBlank(content[index])) {
    ++index;
  }
  *column = index;
  if (index == content.size()) {
    return LineKind::kBlank;
  }
  return content[index] == '#' ? LineKind::kComment : LineKind::kContent;
}

bool ParseCommentedKey(std::string_view content, CommentedKey* parsed) {
  std::size_t index = 0;
  while (index < content.size() && content[index] == ' ') {
    ++index;
  }
  if (index == content.size() || content[index] != '#') {
    return false;
  }
  const std::size_t hash_column = index++;
  std::size_t spaces = 0;
  while (index < content.size() && content[index] == ' ') {
    ++index;
    ++spaces;
  }
  const std::size_t key_begin = index;
  while (index < content.size() && IsKeyCharacter(content[index])) {
    ++index;
  }
  if (index == key_begin || index == content.size() || content[index] != ':') {
    return false;
  }
  const std::size_t key_end = index++;
  if (index < content.size() && !IsBlank(content[index])) {
    return false;
  }
  parsed->hash_column = hash_column;
  parsed->key_column = hash_column + (spaces > 0 ? spaces - 1 : 0);
  parsed->key = std::string(content.substr(key_begin, key_end - key_begin));
  parsed->rest = std::string(Trim(content.substr(index)));
  return true;
}

bool LooksLikeCommentedValue(std::string_view rest) {
  rest = Trim(rest);
  if (rest.empty() || rest.front() == '#') {
    return true;
  }
  if (rest.front() == '"') {
    std::size_t index = 1;
    while (index < rest.size() && rest[index] != '"') {
      index += rest[index] == '\\' ? 2 : 1;
    }
    return index < rest.size() && OnlyCommentFollows(rest.substr(index + 1));
  }
  if (rest.front() == '\'') {
    std::size_t index = 1;
    while (index < rest.size()) {
      if (rest[index] == '\'') {
        if (index + 1 < rest.size() && rest[index + 1] == '\'') {
          index += 2;
          continue;
        }
        break;
      }
      ++index;
    }
    return index < rest.size() && OnlyCommentFollows(rest.substr(index + 1));
  }
  std::size_t index = 0;
  while (index < rest.size() && !IsBlank(rest[index])) {
    ++index;
  }
  return OnlyCommentFollows(rest.substr(index));
}

int Layout::Find(std::string_view path) const {
  const auto found = by_path.find(std::string(path));
  return found == by_path.end() ? -1 : found->second;
}

Layout BuildLayout(std::string_view text) {
  Layout layout;
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    layout.problem = "cannot initialize the YAML parser";
    return layout;
  }
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(text.data()),
      text.size());
  LayoutBuilder builder(&layout);
  bool ok = true;
  bool seen_document = false;
  while (true) {
    yaml_event_t event;
    if (!yaml_parser_parse(&parser, &event)) {
      layout.problem = parser.problem != nullptr ? parser.problem
                                                 : "invalid YAML";
      if (parser.context != nullptr) {
        layout.problem += std::string(" ") + parser.context;
      }
      layout.has_problem_mark = true;
      layout.problem_mark = ToMark(parser.problem_mark);
      ok = false;
      break;
    }
    bool done = false;
    switch (event.type) {
      case YAML_DOCUMENT_START_EVENT:
        // Only the first document counts, as in Mocktail's loader.
        done = seen_document;
        seen_document = true;
        break;
      case YAML_DOCUMENT_END_EVENT:
      case YAML_STREAM_END_EVENT:
        done = true;
        break;
      case YAML_SCALAR_EVENT:
      case YAML_ALIAS_EVENT:
      case YAML_MAPPING_START_EVENT:
      case YAML_SEQUENCE_START_EVENT:
        ok = builder.OnNode(event);
        done = !ok;
        break;
      case YAML_MAPPING_END_EVENT:
      case YAML_SEQUENCE_END_EVENT:
        builder.OnCollectionEnd(event);
        break;
      default:
        break;
    }
    yaml_event_delete(&event);
    if (done) {
      break;
    }
  }
  yaml_parser_delete(&parser);
  layout.parsed = ok;
  if (!ok) {
    layout.nodes.clear();
    layout.by_path.clear();
    layout.duplicate_paths.clear();
  }
  return layout;
}

}  // namespace mocktail::launcher::internal
