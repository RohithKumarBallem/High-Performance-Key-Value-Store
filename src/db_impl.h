#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "counters.h"
#include "kv/db.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace kv {

// Immutable snapshot of which tables live in which level. Level 0 is ordered
// newest-first (tables may overlap); deeper levels are sorted by key range and
// never overlap. Readers pin a Version with a shared_ptr so compaction can
// retire tables underneath them.
struct Version {
  std::vector<std::shared_ptr<SSTable>> levels[kNumLevels];

  uint64_t LevelBytes(int level) const {
    uint64_t total = 0;
    for (const auto& f : levels[level]) total += f->file_size();
    return total;
  }
};

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string path);
  ~DBImpl() override;

  // Opens the directory, replays the WAL, and starts the background thread.
  Status Init();

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Scan(std::string_view start, std::string_view end, size_t limit,
              std::vector<std::pair<std::string, std::string>>* out) override;
  Status Flush() override;
  void WaitForBackgroundWork() override;
  Stats GetStats() const override;
  std::string DebugString() const override;

 private:
  using VersionPtr = std::shared_ptr<const Version>;

  struct Compaction {
    int level = 0;  // inputs[0] come from `level`, inputs[1] from `level + 1`
    std::vector<std::shared_ptr<SSTable>> inputs[2];
    bool drop_tombstones = false;
  };

  // --- recovery -----------------------------------------------------------
  Status Recover();
  Status LoadManifest(uint64_t* log_number, uint64_t* next_file,
                      std::vector<std::pair<int, uint64_t>>* files, bool* found);
  Status WriteManifest(const Version& version, uint64_t log_number);

  // --- write path ---------------------------------------------------------
  Status Write(ValueType type, std::string_view key, std::string_view value);
  Status MakeRoomForWrite();
  // Freezes the active memtable and starts a fresh one with a new WAL.
  // Caller holds write_mu_ and `lk` on state_mu_, and imm_ must be empty.
  Status RotateMemTable(std::unique_lock<std::mutex>& lk);

  // --- background work ----------------------------------------------------
  void BackgroundLoop();
  Status FlushImmutable(std::unique_lock<std::mutex>& lk);
  Status RunCompaction(std::unique_lock<std::mutex>& lk);
  bool PickCompaction(const Version& v, Compaction* c);
  bool NeedsCompaction(const Version& v) const;
  Status DoCompaction(const Compaction& c, std::vector<std::shared_ptr<SSTable>>* outputs);
  Status BuildTable(const MemTable& mem, uint64_t file_number, std::shared_ptr<SSTable>* table);
  VersionPtr ApplyEdit(const Version& base, const std::set<uint64_t>& removed,
                       const std::vector<std::pair<int, std::shared_ptr<SSTable>>>& added) const;

  // --- helpers ------------------------------------------------------------
  uint64_t NewFileNumber();
  std::string TablePath(uint64_t number) const;
  std::string LogPath(uint64_t number) const;
  std::string ManifestPath() const;
  uint64_t MaxBytesForLevel(int level) const;

  const Options options_;
  const std::string path_;
  Counters counters_;
  BlockCache cache_;
  int lock_fd_ = -1;

  // Serializes writers; guards wal_. Always acquired before state_mu_.
  std::mutex write_mu_;
  std::unique_ptr<WalWriter> wal_;

  // Guards everything below. Held only for short pointer swaps and waits,
  // never across I/O.
  mutable std::mutex state_mu_;
  std::condition_variable bg_cv_;    // wakes the background thread
  std::condition_variable room_cv_;  // wakes writers/waiters when bg work finishes
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  VersionPtr version_;
  uint64_t mem_log_number_ = 0;
  uint64_t imm_log_number_ = 0;
  uint64_t next_file_number_ = 1;
  bool shutting_down_ = false;
  bool bg_busy_ = false;
  Status bg_error_;

  // Touched only by the background thread (and Recover, before it starts).
  // The `log` value most recently written to the MANIFEST: logs numbered below
  // it are fully flushed. Compactions re-write it unchanged.
  uint64_t manifest_log_number_ = 0;
  std::string compact_pointer_[kNumLevels];

  std::thread bg_thread_;
};

}  // namespace kv
