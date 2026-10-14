#include <string>
#include <vector>

#include "bloom_filter.h"
#include "test_framework.h"

namespace {

std::string Key(int i) { return "key" + std::to_string(i); }

}  // namespace

TEST(Bloom_NoFalseNegatives) {
  std::vector<uint64_t> hashes;
  for (int i = 0; i < 20000; i++) hashes.push_back(kv::BloomFilter::HashKey(Key(i)));
  const std::string filter = kv::BloomFilter::Build(hashes, 10);
  for (int i = 0; i < 20000; i++) CHECK(kv::BloomFilter::MayContain(filter, Key(i)));
}

TEST(Bloom_FalsePositiveRateIsNearTheory) {
  std::vector<uint64_t> hashes;
  for (int i = 0; i < 20000; i++) hashes.push_back(kv::BloomFilter::HashKey(Key(i)));
  const std::string filter = kv::BloomFilter::Build(hashes, 10);

  int false_positives = 0;
  const int probes = 100000;
  for (int i = 0; i < probes; i++) {
    if (kv::BloomFilter::MayContain(filter, "absent" + std::to_string(i))) false_positives++;
  }
  // 10 bits/key theoretically gives ~0.8%; allow generous slack.
  const double rate = static_cast<double>(false_positives) / probes;
  CHECK(rate < 0.02);
}

TEST(Bloom_EmptyOrUnknownFilterNeverRejects) {
  CHECK(kv::BloomFilter::MayContain("", "anything"));
  CHECK(kv::BloomFilter::MayContain(std::string(1, '\x05'), "anything"));
  // Probe count 0 or > 30 marks an encoding we don't understand.
  CHECK(kv::BloomFilter::MayContain(std::string("\0\0\0\0\0\0\0\0\xff", 9), "anything"));
}

TEST(Bloom_EmptyKeySetBuildsValidFilter) {
  const std::string filter = kv::BloomFilter::Build({}, 10);
  CHECK(!kv::BloomFilter::MayContain(filter, "x"));
}
