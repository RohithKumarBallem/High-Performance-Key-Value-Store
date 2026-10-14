#pragma once

#include <cstdint>

#include "kv/options.h"

namespace kv {

// Point-in-time snapshot of operation counters.
struct Stats {
  uint64_t puts = 0;
  uint64_t deletes = 0;
  uint64_t gets = 0;
  uint64_t get_hits = 0;
  uint64_t scans = 0;

  uint64_t memtable_hits = 0;        // Gets answered by the memtables
  uint64_t table_lookups = 0;        // SSTable lookups that passed the key-range check
  uint64_t bloom_filter_skips = 0;   // ... of which the bloom filter rejected outright
  uint64_t bloom_false_positives = 0;  // filter said "maybe" but the key was absent
  uint64_t block_reads = 0;          // data blocks read from disk
  uint64_t block_cache_hits = 0;

  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t write_stalls = 0;         // times a writer had to wait for background work
  uint64_t wal_bytes = 0;

  uint64_t level_files[kNumLevels] = {};
  uint64_t level_bytes[kNumLevels] = {};
};

}  // namespace kv
