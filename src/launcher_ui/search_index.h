#ifndef MOCKTAIL_LAUNCHER_UI_SEARCH_INDEX_H_
#define MOCKTAIL_LAUNCHER_UI_SEARCH_INDEX_H_

#include <string>
#include <string_view>
#include <vector>

namespace mocktail::launcher_ui {

// One searchable setting. Everything is matched case-insensitively (and
// with ё read as е), in any language the strings are in.
struct SearchEntry {
  // The section id ("graphics").
  std::string section;
  std::string title;
  std::string subtitle;
  // config.yaml keys, environment variable names, English and Russian
  // synonyms ("fps", "кадры").
  std::vector<std::string> keywords;
};

class SearchIndex {
 public:
  // Returns the entry's id.
  int Add(SearchEntry entry);
  void SetSubtitle(int id, std::string subtitle);
  const SearchEntry* Get(int id) const;
  std::size_t size() const { return entries_.size(); }

  // Ids of the entries matching every word of `query`, best first: words
  // that start the title, then words in the title, then the rest. An empty
  // query matches nothing.
  std::vector<int> Match(std::string_view query) const;

 private:
  struct Folded {
    std::string title;
    std::string rest;
    std::vector<std::string> keywords;
  };
  void Fold(int id);

  std::vector<SearchEntry> entries_;
  std::vector<Folded> folded_;
};

// Lower case for matching (Unicode case folding, ё -> е).
std::string FoldForSearch(std::string_view text);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_SEARCH_INDEX_H_
