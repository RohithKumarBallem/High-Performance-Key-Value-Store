#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kv/db.h"
#include "test_framework.h"

namespace {

// Small buffers so modest workloads exercise flushes and multi-level compaction.
kv::Options SmallOptions() {
  kv::Options o;
  o.write_buffer_size = 16 * 1024;
  o.target_file_size = 8 * 1024;
  o.block_size = 512;
  o.level1_max_bytes = 64 * 1024;
  o.level0_compaction_trigger = 3;
  o.level0_stop_writes_trigger = 8;
  o.block_cache_capacity = 64 * 1024;
  return o;
}

std::unique_ptr<kv::DB> OpenDB(const kv::Options& o, const std::string& path) {
  std::unique_ptr<kv::DB> db;
  CHECK_OK(kv::DB::Open(o, path, &db));
  return db;
}

constexpr const char* kMissing = "<missing>";

std::string GetOr(kv::DB* db, const std::string& key) {
  std::string v;
  kv::Status s = db->Get(key, &v);
  if (s.IsNotFound()) return kMissing;
  if (!s.ok()) KV_FAIL("Get(" + key + ") failed: " + s.ToString());
  return v;
}

std::string Key(int i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

std::string Value(int i, int version = 0) {
  return "value-" + std::to_string(i) + "-v" + std::to_string(version) + std::string(40, 'x');
}

using Model = std::map<std::string, std::string>;

void ExpectMatchesModel(kv::DB* db, const Model& model) {
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  CHECK_EQ(rows.size(), model.size());
  size_t i = 0;
  for (const auto& [k, v] : model) {
    CHECK_EQ(rows[i].first, k);
    CHECK_EQ(rows[i].second, v);
    i++;
  }
}

}  // namespace

TEST(DB_PutGetDelete) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(kv::Options(), dir.file("db"));

  CHECK_EQ(GetOr(db.get(), "a"), std::string(kMissing));
  CHECK_OK(db->Put("a", "1"));
  CHECK_OK(db->Put("b", "2"));
  CHECK_EQ(GetOr(db.get(), "a"), std::string("1"));
  CHECK_OK(db->Put("a", "one"));
  CHECK_EQ(GetOr(db.get(), "a"), std::string("one"));
  CHECK_OK(db->Delete("a"));
  CHECK_EQ(GetOr(db.get(), "a"), std::string(kMissing));
  CHECK_EQ(GetOr(db.get(), "b"), std::string("2"));
  CHECK_OK(db->Delete("never-existed"));
  CHECK_OK(db->Put("empty-value", ""));
  CHECK_EQ(GetOr(db.get(), "empty-value"), std::string(""));
}

TEST(DB_RejectsInvalidKeysAndValues) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(kv::Options(), dir.file("db"));
  std::string v;
  CHECK(db->Put("", "x").IsInvalidArgument());
  CHECK(db->Delete("").IsInvalidArgument());
  CHECK(db->Get("", &v).IsInvalidArgument());
  CHECK(db->Put(std::string(65537, 'k'), "x").IsInvalidArgument());
  CHECK_OK(db->Put(std::string(65536, 'k'), "x"));
}

TEST(DB_BinarySafeKeysAndValues) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  const std::string key("a\0b\xff", 4);
  const std::string value("\0\0\xde\xad\0", 5);
  CHECK_OK(db->Put(key, value));
  CHECK_OK(db->Flush());
  CHECK_EQ(GetOr(db.get(), key), value);
}

TEST(DB_PersistsAcrossCleanReopen) {
  kvtest::TempDir dir("db");
  const std::string path = dir.file("db");
  {
    auto db = OpenDB(SmallOptions(), path);
    for (int i = 0; i < 2000; i++) CHECK_OK(db->Put(Key(i), Value(i)));
    CHECK_OK(db->Delete(Key(5)));
  }
  auto db = OpenDB(SmallOptions(), path);
  for (int i = 0; i < 2000; i++) {
    CHECK_EQ(GetOr(db.get(), Key(i)), i == 5 ? std::string(kMissing) : Value(i));
  }
}

TEST(DB_FlushMovesDataToTables) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(kv::Options(), dir.file("db"));
  for (int i = 0; i < 100; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  CHECK_EQ(db->GetStats().level_files[0], uint64_t(0));
  CHECK_OK(db->Flush());
  CHECK_EQ(db->GetStats().level_files[0], uint64_t(1));
  CHECK_EQ(db->GetStats().flushes, uint64_t(1));
  for (int i = 0; i < 100; i++) CHECK_EQ(GetOr(db.get(), Key(i)), Value(i));
  CHECK_OK(db->Flush());  // nothing to flush: a no-op
  CHECK_EQ(db->GetStats().flushes, uint64_t(1));
}

TEST(DB_ModelCheckAgainstStdMap) {
  kvtest::TempDir dir("db");
  const std::string path = dir.file("db");
  const kv::Options opts = SmallOptions();
  auto db = OpenDB(opts, path);

  std::mt19937_64 rng(12345);
  Model model;
  const int kKeySpace = 3000;
  for (int op = 0; op < 40000; op++) {
    const int k = static_cast<int>(rng() % kKeySpace);
    const uint64_t dice = rng() % 100;
    if (dice < 55) {
      const std::string v = Value(k, op);
      CHECK_OK(db->Put(Key(k), v));
      model[Key(k)] = v;
    } else if (dice < 75) {
      CHECK_OK(db->Delete(Key(k)));
      model.erase(Key(k));
    } else {
      auto it = model.find(Key(k));
      CHECK_EQ(GetOr(db.get(), Key(k)), it == model.end() ? std::string(kMissing) : it->second);
    }
    if (op % 10000 == 9999 && op != 39999) {  // restart mid-run: state must carry over
      db.reset();
      db = OpenDB(opts, path);
    }
  }

  db->WaitForBackgroundWork();
  const kv::Stats st = db->GetStats();
  CHECK(st.flushes > 5);
  CHECK(st.compactions > 0);
  uint64_t deep = 0;
  for (int l = 1; l < kv::kNumLevels; l++) deep += st.level_files[l];
  CHECK(deep > 0);  // data actually reached the sorted levels

  for (int k = 0; k < kKeySpace; k++) {
    auto it = model.find(Key(k));
    CHECK_EQ(GetOr(db.get(), Key(k)), it == model.end() ? std::string(kMissing) : it->second);
  }
  ExpectMatchesModel(db.get(), model);
}

TEST(DB_SortedLevelsDoNotOverlap) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  std::mt19937_64 rng(7);
  for (int i = 0; i < 30000; i++) {
    CHECK_OK(db->Put(Key(static_cast<int>(rng() % 5000)), Value(i)));
  }
  db->WaitForBackgroundWork();
  // If compaction produced overlapping tables in a sorted level, point
  // lookups would miss keys that exist. Check every key.
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  CHECK(!rows.empty());
  for (const auto& [k, v] : rows) CHECK_EQ(GetOr(db.get(), k), v);
}

TEST(DB_TombstonesShadowAcrossLevels) {
  kvtest::TempDir dir("db");
  kv::Options opts = SmallOptions();
  auto db = OpenDB(opts, dir.file("db"));

  for (int i = 0; i < 500; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  CHECK_OK(db->Flush());
  // Push the old values down into the sorted levels.
  for (int round = 0; round < 6; round++) {
    for (int i = 1000; i < 1100; i++) CHECK_OK(db->Put(Key(i), Value(i, round)));
    CHECK_OK(db->Flush());
  }
  db->WaitForBackgroundWork();

  for (int i = 0; i < 500; i += 2) CHECK_OK(db->Delete(Key(i)));
  for (int i = 0; i < 500; i++) {
    CHECK_EQ(GetOr(db.get(), Key(i)), i % 2 == 0 ? std::string(kMissing) : Value(i));
  }
  CHECK_OK(db->Flush());
  db->WaitForBackgroundWork();
  for (int i = 0; i < 500; i++) {
    CHECK_EQ(GetOr(db.get(), Key(i)), i % 2 == 0 ? std::string(kMissing) : Value(i));
  }
  // And again after a restart, in case a tombstone was dropped too early.
  db.reset();
  db = OpenDB(opts, dir.file("db"));
  for (int i = 0; i < 500; i++) {
    CHECK_EQ(GetOr(db.get(), Key(i)), i % 2 == 0 ? std::string(kMissing) : Value(i));
  }
}

TEST(DB_ScanRangesAndLimits) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  Model model;
  for (int i = 0; i < 1000; i++) {
    CHECK_OK(db->Put(Key(i), Value(i)));
    model[Key(i)] = Value(i);
    if (i == 400) CHECK_OK(db->Flush());
  }
  for (int i = 100; i < 1000; i += 7) {
    CHECK_OK(db->Delete(Key(i)));
    model.erase(Key(i));
  }
  for (int i = 200; i < 260; i++) {  // overwrite some, newer than the tables
    CHECK_OK(db->Put(Key(i), "new"));
    model[Key(i)] = "new";
  }

  auto expect = [&](const std::string& start, const std::string& end, size_t limit) {
    std::vector<std::pair<std::string, std::string>> got;
    CHECK_OK(db->Scan(start, end, limit, &got));
    std::vector<std::pair<std::string, std::string>> want;
    for (auto it = model.lower_bound(start); it != model.end(); ++it) {
      if (!end.empty() && it->first >= end) break;
      if (limit != 0 && want.size() >= limit) break;
      want.emplace_back(it->first, it->second);
    }
    CHECK_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size(); i++) CHECK(got[i] == want[i]);
  };

  expect("", "", 0);
  expect(Key(150), Key(450), 0);
  expect(Key(150), "", 25);
  expect(Key(990), "", 0);
  expect("zzz", "", 0);
  expect(Key(100), Key(100), 0);
  expect("", Key(3), 0);

  db->WaitForBackgroundWork();
  expect("", "", 0);
  expect(Key(333), Key(777), 40);
}

TEST(DB_ScanSpansMoreThanOneMemtableChunk) {
  kvtest::TempDir dir("db");
  kv::Options opts;  // default 4 MiB buffer: everything stays in the memtable
  auto db = OpenDB(opts, dir.file("db"));
  for (int i = 0; i < 1500; i++) CHECK_OK(db->Put(Key(i), "v"));
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  CHECK_EQ(rows.size(), size_t(1500));
  for (int i = 0; i < 1500; i++) CHECK_EQ(rows[i].first, Key(i));
}

TEST(DB_BloomFilterOnAndOffAgree) {
  kvtest::TempDir dir("db");
  const std::string path = dir.file("db");
  kv::Options opts = SmallOptions();
  {
    auto db = OpenDB(opts, path);
    for (int i = 0; i < 6000; i += 2) CHECK_OK(db->Put(Key(i), Value(i)));
    db->WaitForBackgroundWork();
  }
  for (bool bloom : {true, false}) {
    opts.use_bloom_filter = bloom;
    auto db = OpenDB(opts, path);
    for (int i = 0; i < 6000; i++) {
      CHECK_EQ(GetOr(db.get(), Key(i)), i % 2 == 0 ? Value(i) : std::string(kMissing));
    }
    const kv::Stats st = db->GetStats();
    if (bloom) {
      CHECK(st.bloom_filter_skips > 0);
    } else {
      CHECK_EQ(st.bloom_filter_skips, uint64_t(0));
    }
  }
}

TEST(DB_BloomFilterAvoidsBlockReadsForMisses) {
  kvtest::TempDir dir("db");
  kv::Options opts = SmallOptions();
  opts.block_cache_capacity = 0;
  auto db = OpenDB(opts, dir.file("db"));
  for (int i = 0; i < 4000; i += 2) CHECK_OK(db->Put(Key(i), Value(i)));
  CHECK_OK(db->Flush());
  db->WaitForBackgroundWork();

  const uint64_t before = db->GetStats().block_reads;
  for (int i = 1; i < 3999; i += 2) CHECK_EQ(GetOr(db.get(), Key(i)), std::string(kMissing));
  const kv::Stats st = db->GetStats();
  const uint64_t misses = 2000;
  // Without filters every miss inside a table's range costs a block read.
  CHECK(st.block_reads - before < misses / 10);
  CHECK(st.bloom_filter_skips > misses * 9 / 10);
}

TEST(DB_ConcurrentWritersAndReaders) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 3000;
  constexpr int kReaders = 4;
  auto K = [](int w, int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "w%d-%06d", w, i);
    return std::string(buf);
  };
  auto draft = [&](int w, int i) { return "draft:" + K(w, i); };
  auto final_value = [&](int w, int i) { return "final:" + K(w, i); };

  std::atomic<int> progress[kWriters];
  for (auto& p : progress) p.store(-1);
  std::atomic<int> writers_done{0};
  std::mutex err_mu;
  std::string first_error;
  auto fail = [&](const std::string& msg) {
    std::lock_guard<std::mutex> lk(err_mu);
    if (first_error.empty()) first_error = msg;
  };

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; i++) {
        kv::Status s = db->Put(K(w, i), draft(w, i));
        if (s.ok()) s = db->Put(K(w, i), final_value(w, i));
        if (!s.ok()) {
          fail("writer: " + s.ToString());
          break;
        }
        progress[w].store(i, std::memory_order_release);
      }
      writers_done.fetch_add(1);
    });
  }
  for (int r = 0; r < kReaders; r++) {
    threads.emplace_back([&, r] {
      std::mt19937_64 rng(1000 + r);
      while (writers_done.load() < kWriters) {
        const int w = static_cast<int>(rng() % kWriters);
        const int upto = progress[w].load(std::memory_order_acquire);
        if (upto < 0) continue;
        const int i = static_cast<int>(rng() % (upto + 1));
        std::string v;
        kv::Status s = db->Get(K(w, i), &v);
        if (!s.ok()) {
          fail("reader: Get(" + K(w, i) + ") -> " + s.ToString() + " but write was acknowledged");
          break;
        }
        if (v != draft(w, i) && v != final_value(w, i)) {
          fail("reader: wrong value for " + K(w, i) + ": " + v);
          break;
        }
        if (rng() % 64 == 0) {
          std::vector<std::pair<std::string, std::string>> rows;
          s = db->Scan(K(w, 0), K(w, upto + 1), 40, &rows);
          if (!s.ok()) {
            fail("reader: Scan -> " + s.ToString());
            break;
          }
          for (size_t j = 1; j < rows.size(); j++) {
            if (!(rows[j - 1].first < rows[j].first)) fail("reader: scan out of order");
          }
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK_EQ(first_error, std::string());

  for (int w = 0; w < kWriters; w++) {
    for (int i = 0; i < kPerWriter; i++) CHECK_EQ(GetOr(db.get(), K(w, i)), final_value(w, i));
  }
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  CHECK_EQ(rows.size(), size_t(kWriters * kPerWriter));
}

TEST(DB_ConcurrentWritersToSameKeysEndConsistent) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  constexpr int kThreads = 6;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 4000; i++) {
        const int k = i % 200;
        if (i % 9 == 0) {
          db->Delete(Key(k));
        } else {
          db->Put(Key(k), "t" + std::to_string(t));
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  db->WaitForBackgroundWork();

  // Each surviving value must be one a writer actually wrote, and a scan and
  // point reads must agree.
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  for (const auto& [k, v] : rows) {
    CHECK(v.size() == 2 && v[0] == 't');
    CHECK_EQ(GetOr(db.get(), k), v);
  }
}

TEST(DB_WriteBackpressureLosesNothing) {
  kvtest::TempDir dir("db");
  kv::Options opts = SmallOptions();
  opts.write_buffer_size = 2048;
  opts.level0_compaction_trigger = 2;
  opts.level0_stop_writes_trigger = 4;
  auto db = OpenDB(opts, dir.file("db"));
  for (int i = 0; i < 20000; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  for (int i = 0; i < 20000; i += 97) CHECK_EQ(GetOr(db.get(), Key(i)), Value(i));
  db->WaitForBackgroundWork();
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK_OK(db->Scan("", "", 0, &rows));
  CHECK_EQ(rows.size(), size_t(20000));
}

TEST(DB_LargeValues) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  const std::string big(3 * 1024 * 1024, 'q');  // far larger than the write buffer
  CHECK_OK(db->Put("big", big));
  CHECK_OK(db->Put("small", "s"));
  CHECK_OK(db->Flush());
  CHECK(GetOr(db.get(), "big") == big);
  CHECK_EQ(GetOr(db.get(), "small"), std::string("s"));
}

TEST(DB_SecondOpenOfSameDirectoryIsRefused) {
  kvtest::TempDir dir("db");
  const std::string path = dir.file("db");
  auto db = OpenDB(kv::Options(), path);
  std::unique_ptr<kv::DB> second;
  kv::Status s = kv::DB::Open(kv::Options(), path, &second);
  CHECK(s.IsIOError());
  CHECK(second == nullptr);
  db.reset();
  CHECK_OK(kv::DB::Open(kv::Options(), path, &second));  // lock released on close
}

TEST(DB_CreateIfMissingFalse) {
  kvtest::TempDir dir("db");
  kv::Options opts;
  opts.create_if_missing = false;
  std::unique_ptr<kv::DB> db;
  CHECK(kv::DB::Open(opts, dir.file("nope"), &db).IsInvalidArgument());
}

TEST(DB_SyncWritesMode) {
  kvtest::TempDir dir("db");
  kv::Options opts = SmallOptions();
  opts.sync_writes = true;
  const std::string path = dir.file("db");
  {
    auto db = OpenDB(opts, path);
    for (int i = 0; i < 60; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  }
  auto db = OpenDB(opts, path);
  for (int i = 0; i < 60; i++) CHECK_EQ(GetOr(db.get(), Key(i)), Value(i));
}

TEST(DB_StatsAreConsistent) {
  kvtest::TempDir dir("db");
  auto db = OpenDB(SmallOptions(), dir.file("db"));
  for (int i = 0; i < 100; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  CHECK_OK(db->Delete(Key(0)));
  for (int i = 0; i < 100; i++) GetOr(db.get(), Key(i));
  const kv::Stats st = db->GetStats();
  CHECK_EQ(st.puts, uint64_t(100));
  CHECK_EQ(st.deletes, uint64_t(1));
  CHECK_EQ(st.gets, uint64_t(100));
  CHECK_EQ(st.get_hits, uint64_t(99));
  CHECK(st.wal_bytes > 0);
  CHECK(!db->DebugString().empty());
}
