#include "launcher_ui/search_index.h"

#include <glib.h>

#include <algorithm>
#include <numeric>
#include <utility>

namespace mocktail::launcher_ui {
namespace {

std::vector<std::string> Words(std::string_view text) {
  std::vector<std::string> words;
  std::string current;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!current.empty()) words.push_back(std::move(current));
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) words.push_back(std::move(current));
  return words;
}

bool StartsWord(const std::string& haystack, const std::string& word) {
  std::size_t position = 0;
  while ((position = haystack.find(word, position)) != std::string::npos) {
    if (position == 0 || haystack[position - 1] == ' ' ||
        haystack[position - 1] == '.' || haystack[position - 1] == '_' ||
        haystack[position - 1] == '-' || haystack[position - 1] == '(') {
      return true;
    }
    ++position;
  }
  return false;
}

}  // namespace

std::string FoldForSearch(std::string_view text) {
  if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()),
                       nullptr)) {
    return {};
  }
  gchar* folded =
      g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
  std::string result(folded != nullptr ? folded : "");
  g_free(folded);
  // ё and е are the same letter for search purposes.
  constexpr std::string_view kYo = "\xd1\x91";
  constexpr std::string_view kYe = "\xd0\xb5";
  std::size_t position = 0;
  while ((position = result.find(kYo, position)) != std::string::npos) {
    result.replace(position, kYo.size(), kYe);
    position += kYe.size();
  }
  return result;
}

int SearchIndex::Add(SearchEntry entry) {
  entries_.push_back(std::move(entry));
  folded_.emplace_back();
  const int id = static_cast<int>(entries_.size() - 1);
  Fold(id);
  return id;
}

void SearchIndex::SetSubtitle(int id, std::string subtitle) {
  if (id < 0 || static_cast<std::size_t>(id) >= entries_.size()) return;
  entries_[static_cast<std::size_t>(id)].subtitle = std::move(subtitle);
  Fold(id);
}

const SearchEntry* SearchIndex::Get(int id) const {
  if (id < 0 || static_cast<std::size_t>(id) >= entries_.size()) {
    return nullptr;
  }
  return &entries_[static_cast<std::size_t>(id)];
}

void SearchIndex::Fold(int id) {
  const SearchEntry& entry = entries_[static_cast<std::size_t>(id)];
  Folded& folded = folded_[static_cast<std::size_t>(id)];
  folded.title = FoldForSearch(entry.title);
  folded.rest =
      FoldForSearch(entry.subtitle) + "\n" + FoldForSearch(entry.section);
  folded.keywords.clear();
  for (const std::string& keyword : entry.keywords) {
    folded.keywords.push_back(FoldForSearch(keyword));
    folded.rest += "\n" + folded.keywords.back();
  }
}

std::vector<int> SearchIndex::Match(std::string_view query) const {
  const std::vector<std::string> words = Words(FoldForSearch(query));
  std::vector<std::pair<int, int>> scored;  // (score, id)
  if (words.empty()) return {};
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const Folded& folded = folded_[index];
    int score = 0;
    bool all = true;
    for (const std::string& word : words) {
      if (StartsWord(folded.title, word)) {
        score += 4;
      } else if (folded.title.find(word) != std::string::npos) {
        score += 3;
      } else if (std::find(folded.keywords.begin(), folded.keywords.end(),
                           word) != folded.keywords.end()) {
        score += 2;
      } else if (folded.rest.find(word) != std::string::npos) {
        score += 1;
      } else {
        all = false;
        break;
      }
    }
    if (all) scored.emplace_back(score, static_cast<int>(index));
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto& left, const auto& right) {
                     return left.first > right.first;
                   });
  std::vector<int> ids;
  ids.reserve(scored.size());
  for (const auto& [score, id] : scored) ids.push_back(id);
  return ids;
}

}  // namespace mocktail::launcher_ui
