#include "utf8.h"

namespace mocktail::launcher::internal {

bool DecodeUtf8(std::string_view text, std::size_t* index,
                char32_t* code_point) {
  if (*index >= text.size()) {
    return false;
  }
  const auto byte = [&text](std::size_t position) {
    return static_cast<unsigned char>(text[position]);
  };
  const unsigned char lead = byte(*index);
  std::size_t length = 0;
  char32_t value = 0;
  char32_t minimum = 0;
  if (lead < 0x80U) {
    *code_point = lead;
    ++*index;
    return true;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    value = lead & 0x1FU;
    minimum = 0x80;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    value = lead & 0x0FU;
    minimum = 0x800;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    value = lead & 0x07U;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (*index + length > text.size()) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const unsigned char continuation = byte(*index + offset);
    if ((continuation & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return false;
  }
  *code_point = value;
  *index += length;
  return true;
}

bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    char32_t code_point = 0;
    if (!DecodeUtf8(text, &index, &code_point)) {
      return false;
    }
  }
  return true;
}

}  // namespace mocktail::launcher::internal
