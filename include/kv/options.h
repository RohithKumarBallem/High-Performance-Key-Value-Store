#pragma once

#include <cstddef>
#include <cstdint>

namespace kv {

// Number of LSM levels. Level 0 holds freshly flushed (possibly overlapping)
// tables; levels 1+ hold non-overlapping, key-sorted tables.
constexpr int kNumLevels = 6;

struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = true;

  // Size at which the active memtable is frozen and flushed to a level-0 table.
  size_t write_buffer_size = 4 << 20;

  // Flush every write to stable storage before acknowledging it (fsync of the
  // write-ahead log, with a full drive-cache flush on macOS).
  //
  // When false, an acknowledged write is in the OS page cache: it survives a
  // process crash or kill -9, but the last few writes can be lost on power
  // loss or kernel panic. Tables and the MANIFEST are fsync()ed either way, so
  // the database always reopens in a consistent state.
  bool sync_writes = false;

  // Approximate size of an SSTable data block (the unit of I/O and caching).
  size_t block_size = 4096;

  // Build bloom filters into new tables and consult them on reads.
  bool use_bloom_filter = true;
  int bloom_bits_per_key = 10;  // ~1% false-positive rate

  // Capacity of the shared LRU block cache. 0 disables it.
  size_t block_cache_capacity = 8 << 20;

  // Compaction output files are split at roughly this size.
  size_t target_file_size = 2 << 20;

  // Number of level-0 files that triggers a level-0 -> level-1 compaction.
  int level0_compaction_trigger = 4;
  // Writers stall while level 0 holds this many files (backpressure).
  int level0_stop_writes_trigger = 12;

  // Max total bytes in level 1; each deeper level is `level_size_multiplier`
  // times larger.
  size_t level1_max_bytes = 10 << 20;
  int level_size_multiplier = 10;
};

}  // namespace kv
