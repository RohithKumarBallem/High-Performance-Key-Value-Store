#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "io.h"
#include "sstable.h"
#include "test_framework.h"

namespace {

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

// Builds a table of even-numbered keys 0,2,4,... with every 10th a tombstone.
std::shared_ptr<kv::SSTable> BuildTable(const std::string& path, int n, const kv::Options& opts,
                                        kv::BlockCache* cache, kv::Counters* counters) {
  kv::SSTableBuilder b(opts, path);
  CHECK_OK(b.Open());
  for (int i = 0; i < n; i++) {
    const int k = i * 2;
    if (i % 10 == 9) {
      CHECK_OK(b.Add(Key(k), kv::ValueType::kDeletion, ""));
    } else {
      CHECK_OK(b.Add(Key(k), kv::ValueType::kValue, "v" + std::to_string(k)));
    }
  }
  CHECK_OK(b.Finish());
  std::shared_ptr<kv::SSTable> t;
  CHECK_OK(kv::SSTable::Open(opts, path, 1, cache, counters, &t));
  return t;
}

}  // namespace

TEST(SSTable_PointLookups) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  opts.block_size = 256;  // force many blocks
  kv::BlockCache cache(1 << 20);
  kv::Counters counters;
  auto t = BuildTable(dir.file("t.sst"), 1000, opts, &cache, &counters);

  CHECK_EQ(t->num_entries(), uint64_t(1000));
  CHECK_EQ(t->smallest_key(), Key(0));
  CHECK_EQ(t->largest_key(), Key(1998));

  for (int i = 0; i < 1000; i++) {
    std::string value;
    kv::SSTable::LookupResult r;
    CHECK_OK(t->Get(Key(i * 2), &value, &r));
    if (i % 10 == 9) {
      CHECK(r == kv::SSTable::LookupResult::kDeleted);
    } else {
      CHECK(r == kv::SSTable::LookupResult::kFound);
      CHECK_EQ(value, "v" + std::to_string(i * 2));
    }
  }
  // Odd keys are inside the table's key range but absent.
  for (int i = 0; i < 1000; i++) {
    std::string value;
    kv::SSTable::LookupResult r;
    CHECK_OK(t->Get(Key(i * 2 + 1), &value, &r));
    CHECK(r == kv::SSTable::LookupResult::kNotFound);
  }
}

TEST(SSTable_BloomFilterSkipsAbsentKeys) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  kv::BlockCache cache(0);  // cache off so block_reads reflects real lookups
  kv::Counters counters;
  auto t = BuildTable(dir.file("t.sst"), 5000, opts, &cache, &counters);

  // Odd keys below the table's largest key (9998): inside its range, absent.
  std::string value;
  kv::SSTable::LookupResult r;
  for (int i = 0; i < 4999; i++) CHECK_OK(t->Get(Key(i * 2 + 1), &value, &r));

  const uint64_t skipped = counters.bloom_filter_skips.load();
  const uint64_t fp = counters.bloom_false_positives.load();
  CHECK_EQ(counters.table_lookups.load(), uint64_t(4999));
  CHECK_EQ(skipped + fp, uint64_t(4999));
  CHECK(skipped > 4800);  // ~1% false-positive rate
  CHECK_EQ(counters.block_reads.load(), fp);
}

TEST(SSTable_WithoutBloomFilterStillCorrect) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  opts.use_bloom_filter = false;
  kv::BlockCache cache(0);
  kv::Counters counters;
  auto t = BuildTable(dir.file("t.sst"), 500, opts, &cache, &counters);

  std::string value;
  kv::SSTable::LookupResult r;
  CHECK_OK(t->Get(Key(2), &value, &r));
  CHECK(r == kv::SSTable::LookupResult::kFound);
  CHECK_OK(t->Get(Key(3), &value, &r));
  CHECK(r == kv::SSTable::LookupResult::kNotFound);
  CHECK_EQ(counters.bloom_filter_skips.load(), uint64_t(0));
  CHECK_EQ(counters.block_reads.load(), uint64_t(2));
}

TEST(SSTable_IteratorAndSeek) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  opts.block_size = 128;
  kv::BlockCache cache(0);
  kv::Counters counters;
  auto t = BuildTable(dir.file("t.sst"), 300, opts, &cache, &counters);

  auto it = t->NewIterator(false);
  int count = 0;
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    CHECK(count == 0 || std::string(it->key()) > prev);
    prev = std::string(it->key());
    count++;
  }
  CHECK_OK(it->status());
  CHECK_EQ(count, 300);

  // Seek to a missing key lands on the next present one.
  it->Seek(Key(101));
  CHECK(it->Valid());
  CHECK_EQ(std::string(it->key()), Key(102));
  it->Seek(Key(102));
  CHECK_EQ(std::string(it->key()), Key(102));
  it->Seek(Key(599));  // largest key is 598
  CHECK(!it->Valid());
  it->Seek("");
  CHECK_EQ(std::string(it->key()), Key(0));
}

TEST(SSTable_DetectsCorruptedDataBlock) {
  kvtest::TempDir dir("sst");
  const std::string path = dir.file("t.sst");
  kv::Options opts;
  kv::BlockCache cache(0);
  kv::Counters counters;
  BuildTable(path, 200, opts, &cache, &counters);

  std::string bytes;
  CHECK_OK(kv::ReadFileToString(path, &bytes));
  bytes[20] ^= 0x01;  // inside the first data block
  CHECK_OK(kv::WriteFileAtomic(path, bytes));

  std::shared_ptr<kv::SSTable> t;
  CHECK_OK(kv::SSTable::Open(opts, path, 2, &cache, &counters, &t));  // index is intact
  std::string value;
  kv::SSTable::LookupResult r;
  kv::Status s = t->Get(Key(0), &value, &r);
  CHECK(s.IsCorruption());
}

TEST(SSTable_RejectsTruncatedOrForeignFiles) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  kv::BlockCache cache(0);
  kv::Counters counters;

  CHECK_OK(kv::WriteFileAtomic(dir.file("tiny.sst"), "not a table"));
  std::shared_ptr<kv::SSTable> t;
  CHECK(kv::SSTable::Open(opts, dir.file("tiny.sst"), 1, &cache, &counters, &t).IsCorruption());

  BuildTable(dir.file("good.sst"), 100, opts, &cache, &counters);
  std::string bytes;
  CHECK_OK(kv::ReadFileToString(dir.file("good.sst"), &bytes));
  CHECK_OK(kv::WriteFileAtomic(dir.file("cut.sst"), bytes.substr(0, bytes.size() - 10)));
  CHECK(kv::SSTable::Open(opts, dir.file("cut.sst"), 2, &cache, &counters, &t).IsCorruption());
}

TEST(SSTable_BuilderRejectsOutOfOrderKeys) {
  kvtest::TempDir dir("sst");
  kv::SSTableBuilder b(kv::Options(), dir.file("t.sst"));
  CHECK_OK(b.Open());
  CHECK_OK(b.Add("b", kv::ValueType::kValue, "1"));
  CHECK(b.Add("a", kv::ValueType::kValue, "2").IsInvalidArgument());
  CHECK(b.Add("b", kv::ValueType::kValue, "3").IsInvalidArgument());
  b.Abandon();
}

TEST(MergingIterator_NewestSourceWinsAndTombstonesShadow) {
  kvtest::TempDir dir("sst");
  kv::Options opts;
  kv::BlockCache cache(0);
  kv::Counters counters;

  auto write = [&](const std::string& name, uint64_t number,
                   std::vector<std::tuple<std::string, kv::ValueType, std::string>> rows) {
    kv::SSTableBuilder b(opts, dir.file(name));
    CHECK_OK(b.Open());
    for (auto& [k, t, v] : rows) CHECK_OK(b.Add(k, t, v));
    CHECK_OK(b.Finish());
    std::shared_ptr<kv::SSTable> table;
    CHECK_OK(kv::SSTable::Open(opts, dir.file(name), number, &cache, &counters, &table));
    return table;
  };
  using V = kv::ValueType;
  auto newer = write("n.sst", 2, {{"a", V::kValue, "new-a"}, {"b", V::kDeletion, ""}, {"d", V::kValue, "new-d"}});
  auto older = write("o.sst", 1, {{"a", V::kValue, "old-a"}, {"b", V::kValue, "old-b"}, {"c", V::kValue, "old-c"}});

  std::vector<std::unique_ptr<kv::KVIterator>> children;
  auto i1 = newer->NewIterator(false);
  i1->SeekToFirst();
  auto i2 = older->NewIterator(false);
  i2->SeekToFirst();
  children.push_back(std::move(i1));
  children.push_back(std::move(i2));
  kv::MergingIterator merged(std::move(children));

  std::vector<std::string> seen;
  for (; merged.Valid(); merged.Next()) {
    seen.push_back(std::string(merged.key()) + "=" +
                   (merged.type() == V::kDeletion ? "<del>" : std::string(merged.value())));
  }
  CHECK_EQ(seen.size(), size_t(4));
  CHECK_EQ(seen[0], std::string("a=new-a"));
  CHECK_EQ(seen[1], std::string("b=<del>"));
  CHECK_EQ(seen[2], std::string("c=old-c"));
  CHECK_EQ(seen[3], std::string("d=new-d"));
}
