#include <random>
#include <string>

#include "crc32.h"
#include "test_framework.h"

namespace {

// Straightforward bitwise CRC-32, used as the reference.
uint32_t ReferenceCrc(const std::string& s, uint32_t seed = 0) {
  uint32_t c = ~seed;
  for (unsigned char b : s) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return ~c;
}

}  // namespace

TEST(Crc32_KnownVector) {
  // The standard CRC-32 check value.
  const std::string s = "123456789";
  CHECK_EQ(kv::Crc32(s.data(), s.size()), uint32_t(0xCBF43926));
  CHECK_EQ(kv::Crc32("", 0), uint32_t(0));
}

TEST(Crc32_MatchesReferenceAtEveryLength) {
  std::mt19937 rng(5);
  std::string data(300, '\0');
  for (auto& ch : data) ch = static_cast<char>(rng());
  for (size_t len = 0; len <= data.size(); len++) {
    const std::string piece = data.substr(0, len);
    CHECK_EQ(kv::Crc32(piece.data(), piece.size()), ReferenceCrc(piece));
  }
}

TEST(Crc32_IncrementalEqualsOneShot) {
  const std::string a = "hello, ", b = "incremental world";
  const uint32_t whole = kv::Crc32((a + b).data(), a.size() + b.size());
  const uint32_t part = kv::Crc32(b.data(), b.size(), kv::Crc32(a.data(), a.size()));
  CHECK_EQ(whole, part);
}
