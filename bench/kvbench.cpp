// Benchmark for the kvstore.
//
// Loads a database, then measures random-read latency twice over the same
// on-disk data -- once with bloom filters enabled and once with them ignored --
// so the filters' effect on read latency is an A/B measurement, not a claim.
//
//   kvbench [--num=N] [--value-size=B] [--threads=T] [--reads=N]
//           [--sync-ops=N] [--db=DIR] [--keep]

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kv/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  size_t num_keys = 200000;
  size_t value_size = 100;
  int threads = 4;
  size_t reads = 400000;     // per read phase
  size_t sync_ops = 2000;    // writes in the fsync-per-write phase
  std::string db_dir;
  bool keep = false;
};

std::string KeyFor(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%012llu", static_cast<unsigned long long>(i));
  return buf;
}

// Sorts between KeyFor(i) and KeyFor(i + 1): inside the key range, never present.
std::string MissingKeyFor(uint64_t i) { return KeyFor(i) + "x"; }

// Cheap deterministic filler (xorshift64*), so generating a value costs far less
// than the write being measured.
std::string ValueFor(uint64_t i, size_t size) {
  std::string v(size, 'a');
  uint64_t x = i * 0x9E3779B97F4A7C15ULL + 1;
  for (size_t j = 0; j < size; j++) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    v[j] = static_cast<char>('a' + ((x * 0x2545F4914F6CDD1DULL) >> 59) % 26);
  }
  return v;
}

double Seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

void Die(const std::string& what, const kv::Status& s) {
  std::fprintf(stderr, "%s: %s\n", what.c_str(), s.ToString().c_str());
  std::exit(1);
}

std::unique_ptr<kv::DB> Open(const kv::Options& o, const std::string& dir) {
  std::unique_ptr<kv::DB> db;
  kv::Status s = kv::DB::Open(o, dir, &db);
  if (!s.ok()) Die("open", s);
  return db;
}

struct ReadResult {
  double ops_per_sec = 0;
  double mean_us = 0;
  double p50_us = 0;
  double p99_us = 0;
  uint64_t hits = 0;
};

enum class ReadMix { kHit, kMiss, kMixed };

// Each thread issues `total / threads` reads with the same RNG seeds every
// time, so two runs see an identical key sequence.
ReadResult RunReads(kv::DB* db, const Config& cfg, ReadMix mix, size_t total) {
  const int threads = cfg.threads;
  std::vector<std::vector<uint32_t>> lat(threads);
  std::vector<uint64_t> hits(threads, 0);
  std::vector<std::thread> pool;

  const auto t0 = Clock::now();
  for (int t = 0; t < threads; t++) {
    pool.emplace_back([&, t] {
      std::mt19937_64 rng(0xC0FFEE + t);
      const size_t n = total / threads;
      lat[t].reserve(n);
      std::string value;
      for (size_t i = 0; i < n; i++) {
        const uint64_t id = rng() % cfg.num_keys;
        bool want_miss = mix == ReadMix::kMiss || (mix == ReadMix::kMixed && (rng() & 1));
        const std::string key = want_miss ? MissingKeyFor(id) : KeyFor(id);
        const auto a = Clock::now();
        kv::Status s = db->Get(key, &value);
        const auto b = Clock::now();
        if (s.ok()) {
          hits[t]++;
        } else if (!s.IsNotFound()) {
          Die("get", s);
        }
        lat[t].push_back(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
      }
    });
  }
  for (auto& th : pool) th.join();
  const double elapsed = Seconds(t0, Clock::now());

  std::vector<uint32_t> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end());

  ReadResult r;
  r.ops_per_sec = static_cast<double>(all.size()) / elapsed;
  r.mean_us = std::accumulate(all.begin(), all.end(), 0.0) / static_cast<double>(all.size()) / 1000.0;
  r.p50_us = all[all.size() / 2] / 1000.0;
  r.p99_us = all[all.size() * 99 / 100] / 1000.0;
  r.hits = std::accumulate(hits.begin(), hits.end(), uint64_t{0});
  return r;
}

const char* MixName(ReadMix m) {
  switch (m) {
    case ReadMix::kHit: return "read-hit (existing keys)";
    case ReadMix::kMiss: return "read-miss (absent keys)";
    case ReadMix::kMixed: return "read-mixed (50% absent)";
  }
  return "";
}

void PrintLevels(const kv::Stats& st) {
  std::printf("  layout:");
  for (int l = 0; l < kv::kNumLevels; l++) {
    if (st.level_files[l]) {
      std::printf(" L%d=%llu files/%.1f MiB", l, (unsigned long long)st.level_files[l],
                  static_cast<double>(st.level_bytes[l]) / (1 << 20));
    }
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    auto val = [&](const char* name) -> const char* {
      const size_t n = std::strlen(name);
      return std::strncmp(a, name, n) == 0 ? a + n : nullptr;
    };
    if (auto v = val("--num=")) cfg.num_keys = std::strtoull(v, nullptr, 10);
    else if (auto v2 = val("--value-size=")) cfg.value_size = std::strtoull(v2, nullptr, 10);
    else if (auto v3 = val("--threads=")) cfg.threads = std::atoi(v3);
    else if (auto v4 = val("--reads=")) cfg.reads = std::strtoull(v4, nullptr, 10);
    else if (auto v5 = val("--sync-ops=")) cfg.sync_ops = std::strtoull(v5, nullptr, 10);
    else if (auto v6 = val("--db=")) cfg.db_dir = v6;
    else if (std::strcmp(a, "--keep") == 0) cfg.keep = true;
    else {
      std::fprintf(stderr, "unknown flag: %s\n", a);
      return 2;
    }
  }
  cfg.threads = std::max(cfg.threads, 1);
  if (cfg.db_dir.empty()) {
    cfg.db_dir = (std::filesystem::temp_directory_path() /
                  ("kvbench-" + std::to_string(::getpid()))).string();
  }
  std::filesystem::remove_all(cfg.db_dir);

  std::printf("kvbench: %zu keys, %zu-byte values, %d threads, %zu reads/phase\n", cfg.num_keys,
              cfg.value_size, cfg.threads, cfg.reads);
  std::printf("db: %s\n\n", cfg.db_dir.c_str());

  kv::Options opts;  // defaults: 4 MiB write buffer, 10 bits/key bloom, 8 MiB block cache

  // ---- writes (no fsync): concurrent, random key order ---------------------
  {
    auto db = Open(opts, cfg.db_dir);
    std::vector<uint64_t> order(cfg.num_keys);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937_64(42));

    const auto t0 = Clock::now();
    std::vector<std::thread> pool;
    const size_t per = (cfg.num_keys + cfg.threads - 1) / cfg.threads;
    for (int t = 0; t < cfg.threads; t++) {
      pool.emplace_back([&, t] {
        const size_t lo = t * per, hi = std::min(cfg.num_keys, lo + per);
        for (size_t i = lo; i < hi; i++) {
          kv::Status s = db->Put(KeyFor(order[i]), ValueFor(order[i], cfg.value_size));
          if (!s.ok()) Die("put", s);
        }
      });
    }
    for (auto& th : pool) th.join();
    const double write_secs = Seconds(t0, Clock::now());
    db->WaitForBackgroundWork();
    const double drained_secs = Seconds(t0, Clock::now());

    const kv::Stats st = db->GetStats();
    std::printf("fill-random (no fsync)      %10.0f ops/s   (%.2fs; %.2fs incl. background drain)\n",
                static_cast<double>(cfg.num_keys) / write_secs, write_secs, drained_secs);
    std::printf("  flushes=%llu compactions=%llu write_stalls=%llu write-amp=%.1fx\n",
                (unsigned long long)st.flushes, (unsigned long long)st.compactions,
                (unsigned long long)st.write_stalls,
                static_cast<double>(st.compaction_bytes_written + st.wal_bytes) /
                    std::max<double>(1, static_cast<double>(st.wal_bytes)));
    PrintLevels(st);
  }

  // ---- reads: same data, bloom on vs. off ----------------------------------
  struct Row {
    ReadMix mix;
    ReadResult on, off;
  };
  std::vector<Row> rows;
  for (ReadMix mix : {ReadMix::kHit, ReadMix::kMiss, ReadMix::kMixed}) rows.push_back({mix, {}, {}});

  for (bool bloom : {true, false}) {
    kv::Options o = opts;
    o.use_bloom_filter = bloom;
    auto db = Open(o, cfg.db_dir);
    db->WaitForBackgroundWork();
    // Warm the cache identically for both configurations.
    RunReads(db.get(), cfg, ReadMix::kMixed, cfg.reads / 4);
    for (auto& row : rows) {
      (bloom ? row.on : row.off) = RunReads(db.get(), cfg, row.mix, cfg.reads);
    }
  }

  std::printf("\n%-27s %-6s %12s %10s %9s %9s\n", "read phase", "bloom", "ops/s", "mean us",
              "p50 us", "p99 us");
  for (const auto& row : rows) {
    for (bool bloom : {true, false}) {
      const ReadResult& r = bloom ? row.on : row.off;
      std::printf("%-27s %-6s %12.0f %10.2f %9.2f %9.2f\n", bloom ? MixName(row.mix) : "",
                  bloom ? "on" : "off", r.ops_per_sec, r.mean_us, r.p50_us, r.p99_us);
    }
    std::printf("%-27s %-6s %11.1f%% lower mean latency with bloom filters\n", "", "",
                100.0 * (1.0 - row.on.mean_us / row.off.mean_us));
  }

  // ---- durable writes: fsync on every write --------------------------------
  {
    std::filesystem::remove_all(cfg.db_dir + "-sync");
    kv::Options o = opts;
    o.sync_writes = true;
    auto db = Open(o, cfg.db_dir + "-sync");
    const auto t0 = Clock::now();
    for (size_t i = 0; i < cfg.sync_ops; i++) {
      kv::Status s = db->Put(KeyFor(i), ValueFor(i, cfg.value_size));
      if (!s.ok()) Die("put", s);
    }
    const double secs = Seconds(t0, Clock::now());
    std::printf("\nfill-sequential (fsync/write) %9.0f ops/s   (%zu writes, 1 thread)\n",
                static_cast<double>(cfg.sync_ops) / secs, cfg.sync_ops);
    db.reset();
    std::filesystem::remove_all(cfg.db_dir + "-sync");
  }

  if (!cfg.keep) std::filesystem::remove_all(cfg.db_dir);
  return 0;
}
