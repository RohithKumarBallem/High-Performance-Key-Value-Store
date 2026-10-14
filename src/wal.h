#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "kv/status.h"
#include "types.h"

namespace kv {

// Write-ahead log: every mutation is appended here before it is applied to the
// memtable, so it can be replayed after a crash.
//
// Record layout:
//   crc32 (4) | payload_len (4) | type (1) | key_len (4) | value_len (4) | key | value
// The CRC covers everything after the CRC field. Each record is handed to the
// kernel in a single write(), so a process crash can only tear the last record.
//
// Not thread-safe; the DB serializes writers.
class WalWriter {
 public:
  static Status Create(const std::string& path, bool sync_each_write,
                       std::unique_ptr<WalWriter>* out);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Append(ValueType type, std::string_view key, std::string_view value);
  uint64_t bytes_written() const { return bytes_written_; }

 private:
  WalWriter(int fd, bool sync) : fd_(fd), sync_(sync) {}

  int fd_;
  bool sync_;
  Status sticky_error_;  // a failed append may leave a torn record; refuse further writes
  uint64_t bytes_written_ = 0;
  std::string scratch_;
};

struct WalReplayInfo {
  uint64_t records = 0;
  uint64_t valid_bytes = 0;   // length of the intact prefix
  uint64_t total_bytes = 0;
  bool torn_tail() const { return valid_bytes < total_bytes; }
};

using WalApplyFn = std::function<void(ValueType, std::string_view key, std::string_view value)>;

// Replays the intact prefix of the log at `path`. A truncated or corrupt
// record ends replay (that is the expected result of a crash mid-append), and
// is reported through `info` rather than as an error.
Status ReplayWal(const std::string& path, const WalApplyFn& apply, WalReplayInfo* info);

}  // namespace kv
