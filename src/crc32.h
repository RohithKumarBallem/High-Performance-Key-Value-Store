#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "coding.h"

namespace kv {

namespace detail {

// Slicing-by-8 tables: table[k][b] is the CRC contribution of byte b followed
// by k zero bytes, which lets the main loop fold 8 input bytes per step.
inline const std::array<std::array<uint32_t, 256>, 8>& Crc32Tables() {
  static const std::array<std::array<uint32_t, 256>, 8> tables = [] {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
      for (int k = 1; k < 8; k++) {
        t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xff];
      }
    }
    return t;
  }();
  return tables;
}

}  // namespace detail

// CRC-32 (IEEE 802.3). `seed` is the CRC of any preceding bytes, so
// Crc32(b, n_b, Crc32(a, n_a)) == Crc32(a || b).
inline uint32_t Crc32(const char* data, size_t n, uint32_t seed = 0) {
  const auto& t = detail::Crc32Tables();
  uint32_t c = ~seed;
  while (n >= 8) {
    const uint32_t lo = DecodeFixed32(data) ^ c;
    const uint32_t hi = DecodeFixed32(data + 4);
    c = t[7][lo & 0xff] ^ t[6][(lo >> 8) & 0xff] ^ t[5][(lo >> 16) & 0xff] ^ t[4][lo >> 24] ^
        t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^ t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
    data += 8;
    n -= 8;
  }
  while (n-- > 0) {
    c = t[0][(c ^ static_cast<unsigned char>(*data++)) & 0xff] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace kv
