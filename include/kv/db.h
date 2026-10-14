#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kv/options.h"
#include "kv/stats.h"
#include "kv/status.h"

namespace kv {

// A persistent, ordered key-value store backed by a log-structured merge tree.
//
// All methods are thread-safe. A database directory can be open by only one
// DB instance (in any process) at a time.
class DB {
 public:
  // Opens (and, per `options`, creates) the database at `path`, replaying the
  // write-ahead log to recover any writes that had not reached an SSTable.
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<DB>* db);

  virtual ~DB() = default;

  // Keys must be 1..65536 bytes; values at most 64 MiB.
  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;

  // Returns NotFound if the key is absent or deleted.
  virtual Status Get(std::string_view key, std::string* value) = 0;

  // Appends the live entries with start <= key < end, in key order, to `out`.
  // An empty `end` means "no upper bound"; `limit` of 0 means "no limit".
  // Tables are read from a consistent snapshot; entries in the active
  // memtable are observed as they are at the time they are visited.
  virtual Status Scan(std::string_view start, std::string_view end, size_t limit,
                      std::vector<std::pair<std::string, std::string>>* out) = 0;

  // Forces the active memtable to disk and waits until it is an SSTable.
  virtual Status Flush() = 0;

  // Blocks until no flush or compaction is pending or running.
  virtual void WaitForBackgroundWork() = 0;

  virtual Stats GetStats() const = 0;

  // Human-readable summary of the level layout.
  virtual std::string DebugString() const = 0;
};

}  // namespace kv
