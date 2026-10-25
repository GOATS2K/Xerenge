#pragma once

#include <cstdint>
#include <string_view>

namespace xerenge::locale {

// The hash the game finds a string by (sub_823510E0): CRC-32's table, from
// all ones, shifted arithmetically, not inverted at the end.
inline int32_t KeyHash(std::string_view key) {
  static const auto table = [] {
    struct T {
      uint32_t v[256];
    } t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t.v[i] = c;
    }
    return t;
  }();
  int32_t h = -1;
  for (char c : key) {
    h = int32_t(table.v[uint8_t(uint32_t(h) ^ uint8_t(c))] ^ uint32_t(h >> 8));
  }
  return h;
}

}  // namespace xerenge::locale
