#pragma once

#include <atomic>
#include <cstdint>

namespace kv {

// Relaxed atomic counters shared by the DB and its tables.
struct Counters {
  std::atomic<uint64_t> puts{0};
  std::atomic<uint64_t> deletes{0};
  std::atomic<uint64_t> gets{0};
  std::atomic<uint64_t> get_hits{0};
  std::atomic<uint64_t> scans{0};
  std::atomic<uint64_t> memtable_hits{0};
  std::atomic<uint64_t> table_lookups{0};
  std::atomic<uint64_t> bloom_filter_skips{0};
  std::atomic<uint64_t> bloom_false_positives{0};
  std::atomic<uint64_t> block_reads{0};
  std::atomic<uint64_t> block_cache_hits{0};
  std::atomic<uint64_t> flushes{0};
  std::atomic<uint64_t> compactions{0};
  std::atomic<uint64_t> compaction_bytes_read{0};
  std::atomic<uint64_t> compaction_bytes_written{0};
  std::atomic<uint64_t> write_stalls{0};
  std::atomic<uint64_t> wal_bytes{0};

  static void Add(std::atomic<uint64_t>& c, uint64_t n = 1) {
    c.fetch_add(n, std::memory_order_relaxed);
  }
  static uint64_t Load(const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
};

}  // namespace kv
