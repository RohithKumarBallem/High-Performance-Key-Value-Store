#include "sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "bloom_filter.h"
#include "coding.h"
#include "crc32.h"
#include "io.h"

namespace kv {

namespace {

constexpr size_t kEntryHeader = 9;   // key_len(4) value_len(4) type(1)
constexpr size_t kOutBufferLimit = 256 * 1024;

struct BlockEntry {
  std::string_view key;
  ValueType type;
  std::string_view value;
};

// Decodes the entry starting at block[*pos] and advances *pos past it.
// Returns false if the bytes don't form a valid entry.
bool ParseBlockEntry(std::string_view block, size_t* pos, BlockEntry* e) {
  if (block.size() - *pos < kEntryHeader) return false;
  const char* p = block.data() + *pos;
  const uint32_t key_len = DecodeFixed32(p);
  const uint32_t value_len = DecodeFixed32(p + 4);
  const uint8_t type = static_cast<uint8_t>(p[8]);
  const size_t body = block.size() - *pos - kEntryHeader;
  if (type > static_cast<uint8_t>(ValueType::kValue) || key_len > body ||
      value_len > body - key_len) {
    return false;
  }
  e->key = std::string_view(p + kEntryHeader, key_len);
  e->type = static_cast<ValueType>(type);
  e->value = std::string_view(p + kEntryHeader + key_len, value_len);
  *pos += kEntryHeader + key_len + value_len;
  return true;
}

void AppendCrc(std::string* s) {
  const uint32_t crc = Crc32(s->data(), s->size());
  PutFixed32(s, crc);
}

// Verifies and strips the trailing CRC of `buf`.
Status VerifyAndStripCrc(std::string* buf, const char* what) {
  if (buf->size() < 4) return Status::Corruption(std::string(what) + " too small");
  const size_t n = buf->size() - 4;
  if (Crc32(buf->data(), n) != DecodeFixed32(buf->data() + n)) {
    return Status::Corruption(std::string(what) + " checksum mismatch");
  }
  buf->resize(n);
  return Status::OK();
}

}  // namespace

// ---------------------------------------------------------------- BlockCache

BlockCache::BlockCache(size_t capacity_bytes) : shard_capacity_(capacity_bytes / kNumShards) {}

BlockCache::Block BlockCache::Lookup(uint64_t file_number, uint64_t offset) {
  if (shard_capacity_ == 0) return nullptr;
  const Key key{file_number, offset};
  Shard& shard = ShardFor(key);
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.map.find(key);
  if (it == shard.map.end()) return nullptr;
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
  return it->second->block;
}

void BlockCache::Insert(uint64_t file_number, uint64_t offset, Block block) {
  if (shard_capacity_ == 0 || block->size() > shard_capacity_) return;
  const Key key{file_number, offset};
  Shard& shard = ShardFor(key);
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.map.find(key);
  if (it != shard.map.end()) {
    shard.usage -= it->second->block->size();
    shard.lru.erase(it->second);
    shard.map.erase(it);
  }
  shard.usage += block->size();
  shard.lru.push_front(Entry{key, std::move(block)});
  shard.map[key] = shard.lru.begin();
  while (shard.usage > shard_capacity_ && !shard.lru.empty()) {
    const Entry& victim = shard.lru.back();
    shard.usage -= victim.block->size();
    shard.map.erase(victim.key);
    shard.lru.pop_back();
  }
}

// ------------------------------------------------------------ SSTableBuilder

SSTableBuilder::SSTableBuilder(const Options& options, std::string path)
    : options_(options), path_(std::move(path)) {}

SSTableBuilder::~SSTableBuilder() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableBuilder::Open() {
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) return IOErrorFromErrno("create " + path_);
  return Status::OK();
}

Status SSTableBuilder::Add(std::string_view key, ValueType type, std::string_view value) {
  if (num_entries_ > 0 && key <= last_key_) {
    return Status::InvalidArgument("SSTableBuilder: keys must be added in ascending order");
  }
  if (num_entries_ == 0) smallest_.assign(key);
  last_key_.assign(key);
  num_entries_++;
  if (options_.use_bloom_filter) key_hashes_.push_back(BloomFilter::HashKey(key));

  PutFixed32(&block_, static_cast<uint32_t>(key.size()));
  PutFixed32(&block_, static_cast<uint32_t>(value.size()));
  block_.push_back(static_cast<char>(type));
  block_.append(key.data(), key.size());
  block_.append(value.data(), value.size());

  if (block_.size() >= options_.block_size) return FlushDataBlock();
  return Status::OK();
}

Status SSTableBuilder::FlushDataBlock() {
  if (block_.empty()) return Status::OK();
  AppendCrc(&block_);
  index_.push_back(IndexEntry{last_key_, offset_, static_cast<uint32_t>(block_.size())});
  Status s = Emit(block_);
  block_.clear();
  return s;
}

Status SSTableBuilder::Emit(const std::string& bytes) {
  out_.append(bytes);
  offset_ += bytes.size();
  return out_.size() >= kOutBufferLimit ? FlushOutput() : Status::OK();
}

Status SSTableBuilder::FlushOutput() {
  Status s = WriteFully(fd_, out_.data(), out_.size());
  out_.clear();
  return s;
}

Status SSTableBuilder::Finish() {
  Status s = FlushDataBlock();
  if (!s.ok()) return s;

  std::string filter;
  if (options_.use_bloom_filter) {
    filter = BloomFilter::Build(key_hashes_, options_.bloom_bits_per_key);
  }
  AppendCrc(&filter);
  const uint64_t filter_off = offset_;
  const uint64_t filter_size = filter.size();
  s = Emit(filter);
  if (!s.ok()) return s;

  std::string index;
  PutFixed32(&index, static_cast<uint32_t>(smallest_.size()));
  index.append(smallest_);
  PutFixed32(&index, static_cast<uint32_t>(index_.size()));
  for (const IndexEntry& e : index_) {
    PutFixed32(&index, static_cast<uint32_t>(e.last_key.size()));
    index.append(e.last_key);
    PutFixed64(&index, e.offset);
    PutFixed32(&index, e.size);
  }
  AppendCrc(&index);
  const uint64_t index_off = offset_;
  const uint64_t index_size = index.size();
  s = Emit(index);
  if (!s.ok()) return s;

  std::string footer;
  PutFixed64(&footer, filter_off);
  PutFixed64(&footer, filter_size);
  PutFixed64(&footer, index_off);
  PutFixed64(&footer, index_size);
  PutFixed64(&footer, num_entries_);
  PutFixed64(&footer, kTableMagic);
  s = Emit(footer);
  if (!s.ok()) return s;

  s = FlushOutput();
  if (s.ok()) s = SyncFd(fd_, options_.sync_writes);
  ::close(fd_);
  fd_ = -1;
  finished_ = s.ok();
  return s;
}

void SSTableBuilder::Abandon() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  RemoveFileQuietly(path_);
}

// ------------------------------------------------------------------ SSTable

Status SSTable::Open(const Options& options, const std::string& path, uint64_t file_number,
                     BlockCache* cache, Counters* counters, std::shared_ptr<SSTable>* out) {
  std::shared_ptr<SSTable> t(new SSTable());
  t->options_ = options;
  t->path_ = path;
  t->file_number_ = file_number;
  t->cache_ = cache;
  t->counters_ = counters;

  t->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (t->fd_ < 0) return IOErrorFromErrno("open " + path);
  struct stat st;
  if (::fstat(t->fd_, &st) != 0) return IOErrorFromErrno("stat " + path);
  t->file_size_ = static_cast<uint64_t>(st.st_size);
  if (t->file_size_ < kFooterSize) return Status::Corruption(path + ": file too small");

  char footer[kFooterSize];
  Status s = PReadFully(t->fd_, footer, kFooterSize, t->file_size_ - kFooterSize);
  if (!s.ok()) return s;
  const uint64_t filter_off = DecodeFixed64(footer);
  const uint64_t filter_size = DecodeFixed64(footer + 8);
  const uint64_t index_off = DecodeFixed64(footer + 16);
  const uint64_t index_size = DecodeFixed64(footer + 24);
  t->num_entries_ = DecodeFixed64(footer + 32);
  if (DecodeFixed64(footer + 40) != kTableMagic) {
    return Status::Corruption(path + ": bad magic number");
  }
  const uint64_t body = t->file_size_ - kFooterSize;
  if (filter_off > body || filter_size > body - filter_off || index_off > body ||
      index_size > body - index_off) {
    return Status::Corruption(path + ": footer offsets out of range");
  }

  std::string filter(filter_size, '\0');
  s = PReadFully(t->fd_, filter.data(), filter.size(), filter_off);
  if (s.ok()) s = VerifyAndStripCrc(&filter, "filter block");
  if (!s.ok()) return Status::Corruption(path + ": " + s.ToString());
  if (options.use_bloom_filter) t->filter_ = std::move(filter);

  std::string index(index_size, '\0');
  s = PReadFully(t->fd_, index.data(), index.size(), index_off);
  if (s.ok()) s = VerifyAndStripCrc(&index, "index block");
  if (!s.ok()) return Status::Corruption(path + ": " + s.ToString());

  // Parse the index. Every length is bounds-checked against what remains.
  size_t pos = 0;
  auto read_u32 = [&](uint32_t* v) {
    if (index.size() - pos < 4) return false;
    *v = DecodeFixed32(index.data() + pos);
    pos += 4;
    return true;
  };
  auto read_str = [&](std::string* str) {
    uint32_t len;
    if (!read_u32(&len) || index.size() - pos < len) return false;
    str->assign(index.data() + pos, len);
    pos += len;
    return true;
  };
  uint32_t count;
  if (!read_str(&t->smallest_) || !read_u32(&count)) {
    return Status::Corruption(path + ": malformed index block");
  }
  t->index_.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    IndexEntry e;
    if (!read_str(&e.last_key) || index.size() - pos < 12) {
      return Status::Corruption(path + ": malformed index block");
    }
    e.offset = DecodeFixed64(index.data() + pos);
    e.size = DecodeFixed32(index.data() + pos + 8);
    pos += 12;
    if (e.size < 4 || e.offset > filter_off || e.size > filter_off - e.offset) {
      return Status::Corruption(path + ": data block out of range");
    }
    t->index_.push_back(std::move(e));
  }
  if (!t->index_.empty()) t->largest_ = t->index_.back().last_key;

  *out = std::move(t);
  return Status::OK();
}

SSTable::~SSTable() {
  if (fd_ >= 0) ::close(fd_);
}

size_t SSTable::FindBlock(std::string_view key) const {
  size_t lo = 0, hi = index_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (std::string_view(index_[mid].last_key) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

Status SSTable::ReadBlock(size_t block_idx, bool fill_cache, BlockCache::Block* out) const {
  const IndexEntry& ie = index_[block_idx];
  if (cache_ != nullptr) {
    if (BlockCache::Block hit = cache_->Lookup(file_number_, ie.offset)) {
      Counters::Add(counters_->block_cache_hits);
      *out = std::move(hit);
      return Status::OK();
    }
  }

  std::string buf(ie.size, '\0');
  Status s = PReadFully(fd_, buf.data(), buf.size(), ie.offset);
  if (!s.ok()) return s;
  Counters::Add(counters_->block_reads);
  s = VerifyAndStripCrc(&buf, "data block");
  if (!s.ok()) return Status::Corruption(path_ + ": " + s.ToString());

  auto block = std::make_shared<const std::string>(std::move(buf));
  if (cache_ != nullptr && fill_cache) cache_->Insert(file_number_, ie.offset, block);
  *out = std::move(block);
  return Status::OK();
}

Status SSTable::Get(std::string_view key, std::string* value, LookupResult* result) const {
  *result = LookupResult::kNotFound;
  if (index_.empty() || key < smallest_ || key > largest_) return Status::OK();

  Counters::Add(counters_->table_lookups);
  const bool filtered = !filter_.empty();
  if (filtered && !BloomFilter::MayContain(filter_, key)) {
    Counters::Add(counters_->bloom_filter_skips);
    return Status::OK();
  }

  const size_t idx = FindBlock(key);
  if (idx == index_.size()) return Status::OK();

  BlockCache::Block block;
  Status s = ReadBlock(idx, /*fill_cache=*/true, &block);
  if (!s.ok()) return s;

  size_t pos = 0;
  BlockEntry e;
  while (pos < block->size()) {
    if (!ParseBlockEntry(*block, &pos, &e)) {
      return Status::Corruption(path_ + ": malformed entry in data block");
    }
    if (e.key == key) {
      if (e.type == ValueType::kValue) {
        value->assign(e.value.data(), e.value.size());
        *result = LookupResult::kFound;
      } else {
        *result = LookupResult::kDeleted;
      }
      return Status::OK();
    }
    if (e.key > key) break;
  }
  if (filtered) Counters::Add(counters_->bloom_false_positives);
  return Status::OK();
}

std::unique_ptr<TableIterator> SSTable::NewIterator(bool fill_cache) const {
  return std::make_unique<TableIterator>(this, fill_cache);
}

// ------------------------------------------------------------ TableIterator

bool TableIterator::LoadBlock(size_t idx) {
  Status s = table_->ReadBlock(idx, fill_cache_, &block_);
  if (!s.ok()) {
    status_ = s;
    valid_ = false;
    return false;
  }
  block_idx_ = idx;
  pos_ = 0;
  return true;
}

void TableIterator::ParseNext() {
  while (true) {
    if (block_ && pos_ < block_->size()) {
      BlockEntry e;
      if (!ParseBlockEntry(*block_, &pos_, &e)) {
        status_ = Status::Corruption(table_->path() + ": malformed entry in data block");
        valid_ = false;
        return;
      }
      key_ = e.key;
      type_ = e.type;
      value_ = e.value;
      valid_ = true;
      return;
    }
    const size_t next = block_ ? block_idx_ + 1 : 0;
    if (next >= table_->num_blocks()) {
      valid_ = false;
      return;
    }
    if (!LoadBlock(next)) return;
  }
}

void TableIterator::SeekToFirst() {
  block_.reset();
  status_ = Status::OK();
  ParseNext();
}

void TableIterator::Seek(std::string_view target) {
  block_.reset();
  status_ = Status::OK();
  const size_t idx = table_->FindBlock(target);
  if (idx >= table_->num_blocks()) {
    valid_ = false;
    return;
  }
  if (!LoadBlock(idx)) return;
  for (ParseNext(); valid_ && key_ < target; ParseNext()) {
  }
}

// ------------------------------------------------------------- LevelIterator

void LevelIterator::OpenFile(size_t idx) {
  idx_ = idx;
  if (idx_ >= files_.size()) {
    cur_.reset();
    return;
  }
  cur_ = files_[idx_]->NewIterator(fill_cache_);
}

void LevelIterator::SkipExhausted() {
  while (cur_ && !cur_->Valid()) {
    if (!cur_->status().ok()) {
      status_ = cur_->status();
      cur_.reset();
      return;
    }
    OpenFile(idx_ + 1);
    if (cur_) cur_->SeekToFirst();
  }
}

void LevelIterator::SeekToFirst() {
  status_ = Status::OK();
  OpenFile(0);
  if (cur_) cur_->SeekToFirst();
  SkipExhausted();
}

void LevelIterator::Seek(std::string_view target) {
  status_ = Status::OK();
  // First file whose largest key is >= target.
  size_t lo = 0, hi = files_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (std::string_view(files_[mid]->largest_key()) < target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  OpenFile(lo);
  if (cur_) cur_->Seek(target);
  SkipExhausted();
}

void LevelIterator::Next() {
  cur_->Next();
  SkipExhausted();
}

}  // namespace kv
