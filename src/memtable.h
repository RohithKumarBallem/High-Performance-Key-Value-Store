#pragma once

#include <atomic>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "types.h"

namespace kv {

// In-memory sorted write buffer. Thread-safe: any number of concurrent readers,
// writers are exclusive. Once rotated out it is only read.
class MemTable {
 public:
  struct Item {
    std::string key;
    ValueType type;
    std::string value;
  };

  void Put(std::string_view key, std::string_view value) { Insert(key, ValueType::kValue, value); }
  void Delete(std::string_view key) { Insert(key, ValueType::kDeletion, {}); }

  // True if the table holds an entry (value or tombstone) for `key`; `*type`
  // says which, and `*value` is filled for values.
  bool Get(std::string_view key, std::string* value, ValueType* type) const;

  size_t ApproximateMemoryUsage() const { return usage_.load(std::memory_order_relaxed); }
  bool Empty() const;

  // Calls fn(key, type, value) for every entry in key order.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    for (const auto& [key, entry] : map_) fn(std::string_view(key), entry.type, std::string_view(entry.value));
  }

  // Copies up to `max_items` entries with key > `after` (or >= when
  // `inclusive`) in key order. Used by chunked iteration so scans never hold
  // the lock across user code.
  void ReadChunk(const std::string& after, bool inclusive, size_t max_items,
                 std::vector<Item>* out) const;

 private:
  struct Entry {
    ValueType type;
    std::string value;
  };

  // Rough per-entry bookkeeping cost of a std::map node.
  static constexpr size_t kEntryOverhead = 48;

  void Insert(std::string_view key, ValueType type, std::string_view value);

  mutable std::shared_mutex mu_;
  std::map<std::string, Entry, std::less<>> map_;
  std::atomic<size_t> usage_{0};  // modified only under the exclusive lock
};

}  // namespace kv
