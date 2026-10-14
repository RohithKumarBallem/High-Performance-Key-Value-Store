#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "counters.h"
#include "iterator.h"
#include "kv/options.h"
#include "kv/status.h"
#include "types.h"

// On-disk SSTable layout (all integers little-endian):
//
//   [data block]*   entries: key_len(4) value_len(4) type(1) key value ...
//                   followed by crc32(4) of the block contents
//   [filter block]  bloom filter bytes + crc32(4)   (empty filter if disabled)
//   [index block]   smallest_key_len(4) smallest_key count(4)
//                   { last_key_len(4) last_key offset(8) size(4) }*  + crc32(4)
//   [footer 48 B]   filter_off(8) filter_size(8) index_off(8) index_size(8)
//                   num_entries(8) magic(8)
//
// `size` fields include the trailing CRC.

namespace kv {

constexpr uint64_t kTableMagic = 0x314c4254535356ULL;  // "VSSTBL1"
constexpr size_t kFooterSize = 48;

// Sharded LRU cache of decoded data blocks, keyed by (file number, offset).
class BlockCache {
 public:
  using Block = std::shared_ptr<const std::string>;

  explicit BlockCache(size_t capacity_bytes);

  Block Lookup(uint64_t file_number, uint64_t offset);
  void Insert(uint64_t file_number, uint64_t offset, Block block);

 private:
  static constexpr size_t kNumShards = 16;

  struct Key {
    uint64_t file;
    uint64_t offset;
    bool operator==(const Key& o) const { return file == o.file && offset == o.offset; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return static_cast<size_t>((k.file * 0x9E3779B97F4A7C15ULL) ^ (k.offset * 0xC2B2AE3D27D4EB4FULL));
    }
  };
  struct Entry {
    Key key;
    Block block;
  };
  struct Shard {
    std::mutex mu;
    size_t usage = 0;
    std::list<Entry> lru;  // front = most recently used
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map;
  };

  Shard& ShardFor(const Key& k) { return shards_[KeyHash()(k) % kNumShards]; }

  size_t shard_capacity_;
  std::array<Shard, kNumShards> shards_;
};

// Writes a table from entries added in strictly ascending key order.
class SSTableBuilder {
 public:
  SSTableBuilder(const Options& options, std::string path);
  ~SSTableBuilder();

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  Status Open();
  Status Add(std::string_view key, ValueType type, std::string_view value);
  // Writes filter, index and footer, then fsyncs and closes the file.
  Status Finish();
  // Closes and deletes the partially written file.
  void Abandon();

  uint64_t NumEntries() const { return num_entries_; }
  // Bytes emitted so far (file size once finished).
  uint64_t FileSize() const { return offset_; }
  const std::string& path() const { return path_; }

 private:
  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  Status FlushDataBlock();
  Status Emit(const std::string& bytes);
  Status FlushOutput();

  Options options_;
  std::string path_;
  int fd_ = -1;
  bool finished_ = false;

  std::string block_;      // current data block, not yet sealed
  std::string out_;        // sealed bytes waiting to be written to fd_
  uint64_t offset_ = 0;    // logical file offset of the end of `out_`

  std::string smallest_;
  std::string last_key_;
  uint64_t num_entries_ = 0;
  std::vector<IndexEntry> index_;
  std::vector<uint64_t> key_hashes_;
};

class TableIterator;

// Immutable, open SSTable. Thread-safe: reads use pread() and shared caches.
class SSTable {
 public:
  enum class LookupResult { kNotFound, kFound, kDeleted };

  static Status Open(const Options& options, const std::string& path, uint64_t file_number,
                     BlockCache* cache, Counters* counters, std::shared_ptr<SSTable>* out);
  ~SSTable();

  SSTable(const SSTable&) = delete;
  SSTable& operator=(const SSTable&) = delete;

  Status Get(std::string_view key, std::string* value, LookupResult* result) const;

  // `fill_cache` is false for compaction reads, which would otherwise evict
  // the hot working set.
  std::unique_ptr<TableIterator> NewIterator(bool fill_cache) const;

  uint64_t file_number() const { return file_number_; }
  uint64_t file_size() const { return file_size_; }
  uint64_t num_entries() const { return num_entries_; }
  const std::string& smallest_key() const { return smallest_; }
  const std::string& largest_key() const { return largest_; }
  const std::string& path() const { return path_; }

  bool OverlapsRange(std::string_view lo, std::string_view hi) const {
    return !(largest_ < lo || smallest_ > hi);
  }

 private:
  friend class TableIterator;

  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  SSTable() = default;

  // Index of the first block whose last key is >= key, or num_blocks().
  size_t FindBlock(std::string_view key) const;
  size_t num_blocks() const { return index_.size(); }
  Status ReadBlock(size_t block_idx, bool fill_cache, BlockCache::Block* out) const;

  Options options_;
  std::string path_;
  uint64_t file_number_ = 0;
  uint64_t file_size_ = 0;
  uint64_t num_entries_ = 0;
  int fd_ = -1;
  BlockCache* cache_ = nullptr;
  Counters* counters_ = nullptr;

  std::string smallest_;
  std::string largest_;
  std::string filter_;
  std::vector<IndexEntry> index_;
};

// Iterates one table. The table must outlive the iterator.
class TableIterator final : public KVIterator {
 public:
  TableIterator(const SSTable* table, bool fill_cache) : table_(table), fill_cache_(fill_cache) {}

  void SeekToFirst();
  // Positions at the first entry with key >= target.
  void Seek(std::string_view target);

  bool Valid() const override { return valid_; }
  void Next() override { ParseNext(); }
  std::string_view key() const override { return key_; }
  ValueType type() const override { return type_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  bool LoadBlock(size_t idx);
  void ParseNext();

  const SSTable* table_;
  bool fill_cache_;
  size_t block_idx_ = 0;
  BlockCache::Block block_;
  size_t pos_ = 0;
  bool valid_ = false;
  Status status_;
  std::string_view key_;
  std::string_view value_;
  ValueType type_ = ValueType::kValue;
};

// Iterates a sorted, non-overlapping run of tables (one LSM level) while
// keeping only one table open at a time.
class LevelIterator final : public KVIterator {
 public:
  LevelIterator(std::vector<std::shared_ptr<SSTable>> files, bool fill_cache)
      : files_(std::move(files)), fill_cache_(fill_cache) {}

  void SeekToFirst();
  void Seek(std::string_view target);

  bool Valid() const override { return cur_ && cur_->Valid(); }
  void Next() override;
  std::string_view key() const override { return cur_->key(); }
  ValueType type() const override { return cur_->type(); }
  std::string_view value() const override { return cur_->value(); }
  Status status() const override { return status_; }

 private:
  void OpenFile(size_t idx);   // positions on the first entry of files_[idx]
  void SkipExhausted();        // advances past tables with no remaining entries

  std::vector<std::shared_ptr<SSTable>> files_;
  bool fill_cache_;
  size_t idx_ = 0;
  std::unique_ptr<TableIterator> cur_;
  Status status_;
};

}  // namespace kv
