#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>

#include "coding.h"
#include "crc32.h"
#include "io.h"

namespace kv {

namespace {
constexpr size_t kHeaderSize = 8;       // crc + payload_len
constexpr size_t kPayloadFixed = 9;     // type + key_len + value_len
}  // namespace

Status WalWriter::Create(const std::string& path, bool sync_each_write,
                         std::unique_ptr<WalWriter>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return IOErrorFromErrno("create " + path);
  if (sync_each_write) {
    // Make the new file's directory entry durable before we acknowledge writes into it.
    Status s = SyncDir(std::filesystem::path(path).parent_path().string());
    if (!s.ok()) {
      ::close(fd);
      return s;
    }
  }
  out->reset(new WalWriter(fd, sync_each_write));
  return Status::OK();
}

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status WalWriter::Append(ValueType type, std::string_view key, std::string_view value) {
  if (!sticky_error_.ok()) return sticky_error_;

  const uint32_t payload_len =
      static_cast<uint32_t>(kPayloadFixed + key.size() + value.size());
  scratch_.clear();
  scratch_.reserve(kHeaderSize + payload_len);
  PutFixed32(&scratch_, 0);  // CRC placeholder
  PutFixed32(&scratch_, payload_len);
  scratch_.push_back(static_cast<char>(type));
  PutFixed32(&scratch_, static_cast<uint32_t>(key.size()));
  PutFixed32(&scratch_, static_cast<uint32_t>(value.size()));
  scratch_.append(key.data(), key.size());
  scratch_.append(value.data(), value.size());

  const uint32_t crc = Crc32(scratch_.data() + 4, scratch_.size() - 4);
  for (int i = 0; i < 4; i++) scratch_[i] = static_cast<char>((crc >> (8 * i)) & 0xff);

  Status s = WriteFully(fd_, scratch_.data(), scratch_.size());
  if (s.ok() && sync_) s = SyncFd(fd_, /*full=*/true);
  if (!s.ok()) {
    sticky_error_ = s;
    return s;
  }
  bytes_written_ += scratch_.size();
  return Status::OK();
}

Status ReplayWal(const std::string& path, const WalApplyFn& apply, WalReplayInfo* info) {
  std::string data;
  Status s = ReadFileToString(path, &data);
  if (!s.ok()) return s;

  *info = WalReplayInfo{};
  info->total_bytes = data.size();

  size_t pos = 0;
  while (data.size() - pos >= kHeaderSize) {
    const char* rec = data.data() + pos;
    const uint32_t stored_crc = DecodeFixed32(rec);
    const uint32_t payload_len = DecodeFixed32(rec + 4);
    if (payload_len < kPayloadFixed || payload_len > data.size() - pos - kHeaderSize) break;

    if (Crc32(rec + 4, 4 + payload_len) != stored_crc) break;

    const char* payload = rec + kHeaderSize;
    const uint8_t type = static_cast<uint8_t>(payload[0]);
    const uint32_t key_len = DecodeFixed32(payload + 1);
    const uint32_t value_len = DecodeFixed32(payload + 5);
    if (type > static_cast<uint8_t>(ValueType::kValue)) break;
    if (key_len > payload_len - kPayloadFixed ||
        value_len != payload_len - kPayloadFixed - key_len) {
      break;
    }

    apply(static_cast<ValueType>(type), std::string_view(payload + kPayloadFixed, key_len),
          std::string_view(payload + kPayloadFixed + key_len, value_len));
    pos += kHeaderSize + payload_len;
    info->records++;
  }
  info->valid_bytes = pos;
  return Status::OK();
}

}  // namespace kv
