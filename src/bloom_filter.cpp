#include "bloom_filter.h"

#include <algorithm>

namespace kv {

namespace {
constexpr int kMaxProbes = 30;
}

uint64_t BloomFilter::HashKey(std::string_view key) {
  // FNV-1a, then a splitmix64 finalizer so every output bit depends on every
  // input byte (FNV alone has weak high bits, which double hashing relies on).
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : key) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

std::string BloomFilter::Build(const std::vector<uint64_t>& key_hashes, int bits_per_key) {
  bits_per_key = std::max(bits_per_key, 1);
  // Optimal probe count is bits_per_key * ln(2).
  const int k = std::clamp(static_cast<int>(bits_per_key * 0.69), 1, kMaxProbes);

  size_t bits = std::max<size_t>(64, key_hashes.size() * static_cast<size_t>(bits_per_key));
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  std::string filter(bytes + 1, '\0');
  filter[bytes] = static_cast<char>(k);
  for (uint64_t h : key_hashes) {
    const uint64_t h1 = static_cast<uint32_t>(h);
    const uint64_t h2 = static_cast<uint32_t>(h >> 32) | 1;
    for (int i = 0; i < k; i++) {
      const uint64_t pos = (h1 + static_cast<uint64_t>(i) * h2) % bits;
      filter[pos / 8] = static_cast<char>(filter[pos / 8] | (1 << (pos % 8)));
    }
  }
  return filter;
}

bool BloomFilter::MayContain(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  const int k = static_cast<unsigned char>(filter.back());
  if (k < 1 || k > kMaxProbes) return true;  // unknown encoding: be conservative

  const size_t bits = (filter.size() - 1) * 8;
  const uint64_t h = HashKey(key);
  const uint64_t h1 = static_cast<uint32_t>(h);
  const uint64_t h2 = static_cast<uint32_t>(h >> 32) | 1;
  for (int i = 0; i < k; i++) {
    const uint64_t pos = (h1 + static_cast<uint64_t>(i) * h2) % bits;
    if ((static_cast<unsigned char>(filter[pos / 8]) & (1 << (pos % 8))) == 0) return false;
  }
  return true;
}

}  // namespace kv
