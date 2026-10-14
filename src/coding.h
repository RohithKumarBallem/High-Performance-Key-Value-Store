#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kv {

// Fixed-width little-endian encoding. Explicit shifts keep the on-disk format
// independent of host endianness; compilers lower these to plain loads/stores.

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, sizeof(buf));
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

}  // namespace kv
