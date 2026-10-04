#ifndef MOCKTAIL_LAUNCHER_UTF8_H_
#define MOCKTAIL_LAUNCHER_UTF8_H_

#include <cstddef>
#include <string_view>

namespace mocktail::launcher::internal {

// Decodes one UTF-8 sequence at *index and advances it. Rejects overlong
// forms, surrogates and values above U+10FFFF.
bool DecodeUtf8(std::string_view text, std::size_t* index,
                char32_t* code_point);

bool IsValidUtf8(std::string_view text);

}  // namespace mocktail::launcher::internal

#endif  // MOCKTAIL_LAUNCHER_UTF8_H_
