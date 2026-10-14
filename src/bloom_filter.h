#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kv {

// Bloom filter used to skip SSTables that definitely don't contain a key.
//
// Serialized layout: [bit array][k: 1 byte]. Probe positions come from double
// hashing (Kirsch & Mitzenmacher) over one 64-bit hash of the key.
class BloomFilter {
 public:
  static uint64_t HashKey(std::string_view key);

  // Builds a serialized filter from the hashes of all keys in a table.
  static std::string Build(const std::vector<uint64_t>& key_hashes, int bits_per_key);

  // False means "definitely absent". An empty or unrecognised filter answers
  // true for every key, so it can never cause a false negative.
  static bool MayContain(std::string_view filter, std::string_view key);
};

}  // namespace kv
