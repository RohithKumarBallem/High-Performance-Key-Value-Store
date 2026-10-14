#include "db_impl.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "io.h"

namespace kv {

namespace fs = std::filesystem;

namespace {

constexpr char kManifestHeader[] = "KVSTORE-MANIFEST 1";

Options Sanitize(Options o) {
  o.write_buffer_size = std::max<size_t>(o.write_buffer_size, 1);
  o.block_size = std::max<size_t>(o.block_size, 64);
  o.target_file_size = std::max<size_t>(o.target_file_size, 1);
  o.level1_max_bytes = std::max<size_t>(o.level1_max_bytes, 1);
  o.level_size_multiplier = std::max(o.level_size_multiplier, 2);
  o.level0_compaction_trigger = std::max(o.level0_compaction_trigger, 1);
  // A stop trigger at or below the compaction trigger would stall writers
  // while the compaction that could unblock them never starts.
  o.level0_stop_writes_trigger =
      std::max(o.level0_stop_writes_trigger, o.level0_compaction_trigger + 1);
  return o;
}

std::string NumberedName(uint64_t number, const char* ext) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%06" PRIu64 ".%s", number, ext);
  return buf;
}

// Splits "000012.sst" into (12, "sst").
bool ParseFileName(const std::string& name, uint64_t* number, std::string* ext) {
  const size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot > 18) return false;
  uint64_t n = 0;
  for (size_t i = 0; i < dot; i++) {
    if (name[i] < '0' || name[i] > '9') return false;
    n = n * 10 + static_cast<uint64_t>(name[i] - '0');
  }
  *number = n;
  *ext = name.substr(dot + 1);
  return true;
}

void SortLevels(Version* v) {
  std::sort(v->levels[0].begin(), v->levels[0].end(),
            [](const auto& a, const auto& b) { return a->file_number() > b->file_number(); });
  for (int level = 1; level < kNumLevels; level++) {
    std::sort(v->levels[level].begin(), v->levels[level].end(),
              [](const auto& a, const auto& b) { return a->smallest_key() < b->smallest_key(); });
  }
}

}  // namespace

// ------------------------------------------------------------------ DB::Open

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Init();
  if (!s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

DBImpl::DBImpl(const Options& options, std::string path)
    : options_(Sanitize(options)),
      path_(std::move(path)),
      cache_(options_.block_cache_capacity) {}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  room_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  wal_.reset();
  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
  }
}

Status DBImpl::Init() {
  Status s = Recover();
  if (!s.ok()) return s;
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

// ------------------------------------------------------------------ helpers

uint64_t DBImpl::NewFileNumber() {
  std::lock_guard<std::mutex> lk(state_mu_);
  return next_file_number_++;
}

std::string DBImpl::TablePath(uint64_t number) const {
  return path_ + "/" + NumberedName(number, "sst");
}
std::string DBImpl::LogPath(uint64_t number) const {
  return path_ + "/" + NumberedName(number, "log");
}
std::string DBImpl::ManifestPath() const { return path_ + "/MANIFEST"; }

uint64_t DBImpl::MaxBytesForLevel(int level) const {
  uint64_t max = options_.level1_max_bytes;
  for (int i = 1; i < level; i++) max *= static_cast<uint64_t>(options_.level_size_multiplier);
  return max;
}

// ----------------------------------------------------------------- manifest

Status DBImpl::WriteManifest(const Version& version, uint64_t log_number) {
  uint64_t next_file;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    next_file = next_file_number_;
  }
  std::string out;
  out += kManifestHeader;
  out += "\nlog " + std::to_string(log_number);
  out += "\nnext " + std::to_string(next_file) + "\n";
  for (int level = 0; level < kNumLevels; level++) {
    for (const auto& f : version.levels[level]) {
      out += "file " + std::to_string(level) + " " + std::to_string(f->file_number()) + "\n";
    }
  }
  out += "END\n";
  return WriteFileAtomic(ManifestPath(), out, options_.sync_writes);
}

Status DBImpl::LoadManifest(uint64_t* log_number, uint64_t* next_file,
                            std::vector<std::pair<int, uint64_t>>* files, bool* found) {
  *found = false;
  std::error_code ec;
  if (!fs::exists(ManifestPath(), ec)) return Status::OK();

  std::string data;
  Status s = ReadFileToString(ManifestPath(), &data);
  if (!s.ok()) return s;

  std::istringstream in(data);
  std::string line;
  if (!std::getline(in, line) || line != kManifestHeader) {
    return Status::Corruption("MANIFEST: bad header");
  }
  bool saw_end = false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "log") {
      ls >> *log_number;
    } else if (tag == "next") {
      ls >> *next_file;
    } else if (tag == "file") {
      int level;
      uint64_t number;
      ls >> level >> number;
      if (ls.fail() || level < 0 || level >= kNumLevels) {
        return Status::Corruption("MANIFEST: bad file entry: " + line);
      }
      files->emplace_back(level, number);
    } else if (tag == "END") {
      saw_end = true;
    } else {
      return Status::Corruption("MANIFEST: unrecognised line: " + line);
    }
    if (ls.fail() && tag != "END") return Status::Corruption("MANIFEST: malformed line: " + line);
  }
  if (!saw_end) return Status::Corruption("MANIFEST: truncated (no END marker)");
  *found = true;
  return Status::OK();
}

// ----------------------------------------------------------------- recovery

Status DBImpl::Recover() {
  std::error_code ec;
  if (!fs::exists(path_, ec)) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(path_ + ": does not exist (create_if_missing is false)");
    }
    fs::create_directories(path_, ec);
    if (ec) return Status::IOError("create " + path_ + ": " + ec.message());
  }

  // One DB per directory: flock conflicts across processes and across
  // separate open() calls within one process.
  lock_fd_ = ::open((path_ + "/LOCK").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0) return IOErrorFromErrno("open LOCK");
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    return Status::IOError(path_ + ": database is already open");
  }

  uint64_t log_number = 0;
  uint64_t next_file = 1;
  std::vector<std::pair<int, uint64_t>> listed;
  bool found = false;
  Status s = LoadManifest(&log_number, &next_file, &listed, &found);
  if (!s.ok()) return s;
  if (!found && !options_.create_if_missing) {
    return Status::InvalidArgument(path_ + ": no database found (create_if_missing is false)");
  }

  auto version = std::make_shared<Version>();
  std::set<uint64_t> live;
  for (const auto& [level, number] : listed) {
    std::shared_ptr<SSTable> table;
    s = SSTable::Open(options_, TablePath(number), number, &cache_, &counters_, &table);
    if (!s.ok()) return s;
    version->levels[level].push_back(std::move(table));
    live.insert(number);
  }
  SortLevels(version.get());

  // Anything on disk the manifest doesn't reference is debris from a crash
  // between writing a file and committing it: unreferenced tables, logs whose
  // contents were already flushed, a half-written manifest.
  std::vector<uint64_t> logs;
  uint64_t max_seen = 0;
  for (const auto& entry : fs::directory_iterator(path_, ec)) {
    const std::string name = entry.path().filename().string();
    if (name == "MANIFEST.tmp") {
      RemoveFileQuietly(entry.path().string());
      continue;
    }
    uint64_t number;
    std::string ext;
    if (!ParseFileName(name, &number, &ext)) continue;
    max_seen = std::max(max_seen, number);
    if (ext == "sst") {
      if (live.count(number) == 0) RemoveFileQuietly(entry.path().string());
    } else if (ext == "log") {
      if (number >= log_number) {
        logs.push_back(number);
      } else {
        RemoveFileQuietly(entry.path().string());
      }
    }
  }
  if (ec) return Status::IOError("list " + path_ + ": " + ec.message());
  std::sort(logs.begin(), logs.end());
  next_file_number_ = std::max(next_file, max_seen + 1);

  // Replay surviving logs, oldest first, so later writes win.
  auto recovered = std::make_shared<MemTable>();
  for (uint64_t number : logs) {
    WalReplayInfo info;
    s = ReplayWal(
        LogPath(number),
        [&](ValueType type, std::string_view key, std::string_view value) {
          if (type == ValueType::kValue) {
            recovered->Put(key, value);
          } else {
            recovered->Delete(key);
          }
        },
        &info);
    if (!s.ok()) return s;
  }

  // Persist the recovered writes as a level-0 table before dropping their logs.
  if (!recovered->Empty()) {
    const uint64_t number = next_file_number_++;
    std::shared_ptr<SSTable> table;
    s = BuildTable(*recovered, number, &table);
    if (!s.ok()) return s;
    if (table) {
      auto nv = std::make_shared<Version>(*version);
      nv->levels[0].insert(nv->levels[0].begin(), std::move(table));
      version = std::move(nv);
      Counters::Add(counters_.flushes);
    }
  }

  const uint64_t new_log = next_file_number_++;
  s = WalWriter::Create(LogPath(new_log), options_.sync_writes, &wal_);
  if (!s.ok()) return s;

  mem_ = std::make_shared<MemTable>();
  mem_log_number_ = new_log;
  version_ = version;
  s = WriteManifest(*version_, new_log);
  if (!s.ok()) return s;
  manifest_log_number_ = new_log;

  for (uint64_t number : logs) RemoveFileQuietly(LogPath(number));
  return Status::OK();
}

Status DBImpl::BuildTable(const MemTable& mem, uint64_t file_number,
                          std::shared_ptr<SSTable>* table) {
  table->reset();
  SSTableBuilder builder(options_, TablePath(file_number));
  Status s = builder.Open();
  if (!s.ok()) return s;
  mem.ForEach([&](std::string_view key, ValueType type, std::string_view value) {
    if (s.ok()) s = builder.Add(key, type, value);
  });
  if (s.ok() && builder.NumEntries() == 0) {
    builder.Abandon();
    return Status::OK();
  }
  if (s.ok()) s = builder.Finish();
  if (!s.ok()) {
    builder.Abandon();
    return s;
  }
  s = SSTable::Open(options_, TablePath(file_number), file_number, &cache_, &counters_, table);
  if (!s.ok()) RemoveFileQuietly(TablePath(file_number));
  return s;
}

// --------------------------------------------------------------- write path

Status DBImpl::Put(std::string_view key, std::string_view value) {
  return Write(ValueType::kValue, key, value);
}

Status DBImpl::Delete(std::string_view key) { return Write(ValueType::kDeletion, key, {}); }

Status DBImpl::Write(ValueType type, std::string_view key, std::string_view value) {
  if (key.empty() || key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key must be between 1 and 65536 bytes");
  }
  if (value.size() > kMaxValueSize) {
    return Status::InvalidArgument("value exceeds 64 MiB");
  }

  std::lock_guard<std::mutex> write_lock(write_mu_);
  Status s = MakeRoomForWrite();
  if (!s.ok()) return s;

  // Log first: once the WAL append returns, the write survives a process crash.
  s = wal_->Append(type, key, value);
  if (!s.ok()) return s;
  Counters::Add(counters_.wal_bytes, 17 + key.size() + value.size());

  // mem_ only changes under write_mu_, which we hold.
  if (type == ValueType::kValue) {
    mem_->Put(key, value);
    Counters::Add(counters_.puts);
  } else {
    mem_->Delete(key);
    Counters::Add(counters_.deletes);
  }
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite() {
  std::unique_lock<std::mutex> lk(state_mu_);
  bool stalled = false;
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (shutting_down_) return Status::IOError("database is closing");

    if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) return Status::OK();

    // Need a fresh memtable. Wait if the previous one is still being flushed,
    // or level 0 is so deep that adding another table would hurt reads.
    const bool l0_full =
        static_cast<int>(version_->levels[0].size()) >= options_.level0_stop_writes_trigger;
    if (imm_ != nullptr || l0_full) {
      if (!stalled) {
        stalled = true;
        Counters::Add(counters_.write_stalls);
      }
      bg_cv_.notify_one();
      room_cv_.wait(lk);
      continue;
    }
    return RotateMemTable(lk);
  }
}

Status DBImpl::RotateMemTable(std::unique_lock<std::mutex>& lk) {
  const uint64_t number = next_file_number_++;
  lk.unlock();
  std::unique_ptr<WalWriter> wal;
  Status s = WalWriter::Create(LogPath(number), options_.sync_writes, &wal);
  lk.lock();
  if (!s.ok()) return s;

  // The old log stays on disk until the frozen memtable is safely in a table.
  wal_ = std::move(wal);
  imm_ = std::move(mem_);
  imm_log_number_ = mem_log_number_;
  mem_ = std::make_shared<MemTable>();
  mem_log_number_ = number;
  bg_cv_.notify_one();
  return Status::OK();
}

Status DBImpl::Flush() {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::unique_lock<std::mutex> lk(state_mu_);
  auto settled = [this] { return imm_ == nullptr || !bg_error_.ok() || shutting_down_; };

  room_cv_.wait(lk, settled);
  if (!bg_error_.ok()) return bg_error_;
  if (!mem_->Empty()) {
    Status s = RotateMemTable(lk);
    if (!s.ok()) return s;
    room_cv_.wait(lk, settled);
  }
  return bg_error_;
}

void DBImpl::WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lk(state_mu_);
  room_cv_.wait(lk, [this] {
    return shutting_down_ || !bg_error_.ok() ||
           (imm_ == nullptr && !bg_busy_ && !NeedsCompaction(*version_));
  });
}

// --------------------------------------------------------------- read path

Status DBImpl::Get(std::string_view key, std::string* value) {
  if (key.empty() || key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key must be between 1 and 65536 bytes");
  }
  Counters::Add(counters_.gets);

  // Pin a consistent view: the memtables and the table set as of one instant.
  std::shared_ptr<MemTable> mem, imm;
  VersionPtr version;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    mem = mem_;
    imm = imm_;
    version = version_;
  }

  auto resolve = [&](bool is_value) {
    if (!is_value) return Status::NotFound();
    Counters::Add(counters_.get_hits);
    return Status::OK();
  };

  ValueType type;
  if (mem->Get(key, value, &type)) {
    Counters::Add(counters_.memtable_hits);
    return resolve(type == ValueType::kValue);
  }
  if (imm && imm->Get(key, value, &type)) {
    Counters::Add(counters_.memtable_hits);
    return resolve(type == ValueType::kValue);
  }

  SSTable::LookupResult result;
  // Level 0 tables may overlap, so probe all of them newest-first.
  for (const auto& table : version->levels[0]) {
    Status s = table->Get(key, value, &result);
    if (!s.ok()) return s;
    if (result != SSTable::LookupResult::kNotFound) {
      return resolve(result == SSTable::LookupResult::kFound);
    }
  }
  // Deeper levels don't overlap: at most one candidate table per level.
  for (int level = 1; level < kNumLevels; level++) {
    const auto& files = version->levels[level];
    auto it = std::lower_bound(files.begin(), files.end(), key,
                               [](const std::shared_ptr<SSTable>& f, std::string_view k) {
                                 return std::string_view(f->largest_key()) < k;
                               });
    if (it == files.end()) continue;
    Status s = (*it)->Get(key, value, &result);
    if (!s.ok()) return s;
    if (result != SSTable::LookupResult::kNotFound) {
      return resolve(result == SSTable::LookupResult::kFound);
    }
  }
  return Status::NotFound();
}

Status DBImpl::Scan(std::string_view start, std::string_view end, size_t limit,
                    std::vector<std::pair<std::string, std::string>>* out) {
  Counters::Add(counters_.scans);

  std::shared_ptr<MemTable> mem, imm;
  VersionPtr version;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    mem = mem_;
    imm = imm_;
    version = version_;
  }

  // Newest source first, so MergingIterator resolves duplicates correctly.
  std::vector<std::unique_ptr<KVIterator>> children;
  children.push_back(std::make_unique<MemTableIterator>(mem, std::string(start)));
  if (imm) children.push_back(std::make_unique<MemTableIterator>(imm, std::string(start)));
  for (const auto& table : version->levels[0]) {
    if (table->largest_key() < start) continue;
    auto it = table->NewIterator(/*fill_cache=*/false);
    it->Seek(start);
    children.push_back(std::move(it));
  }
  for (int level = 1; level < kNumLevels; level++) {
    if (version->levels[level].empty()) continue;
    auto it = std::make_unique<LevelIterator>(version->levels[level], /*fill_cache=*/false);
    it->Seek(start);
    children.push_back(std::move(it));
  }

  MergingIterator merged(std::move(children));
  for (; merged.Valid(); merged.Next()) {
    if (!end.empty() && merged.key() >= end) break;
    if (merged.type() == ValueType::kDeletion) continue;
    if (limit != 0 && out->size() >= limit) break;
    out->emplace_back(std::string(merged.key()), std::string(merged.value()));
  }
  return merged.status();
}

// ---------------------------------------------------------- background work

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lk(state_mu_);
  while (true) {
    bg_cv_.wait(lk, [this] {
      return shutting_down_ ||
             (bg_error_.ok() && (imm_ != nullptr || NeedsCompaction(*version_)));
    });
    if (shutting_down_) break;

    bg_busy_ = true;
    // Flushing takes priority: writers are blocked behind the immutable memtable.
    Status s = imm_ != nullptr ? FlushImmutable(lk) : RunCompaction(lk);
    bg_busy_ = false;
    if (!s.ok()) bg_error_ = s;
    room_cv_.notify_all();
  }
}

Status DBImpl::FlushImmutable(std::unique_lock<std::mutex>& lk) {
  std::shared_ptr<MemTable> imm = imm_;
  VersionPtr base = version_;
  // With imm_ set, writers can't rotate again, so this log number is stable
  // and everything older is covered by the table we are about to write.
  const uint64_t live_log = mem_log_number_;
  const uint64_t flushed_log = imm_log_number_;
  lk.unlock();

  const uint64_t number = NewFileNumber();
  std::shared_ptr<SSTable> table;
  Status s = BuildTable(*imm, number, &table);
  VersionPtr installed;
  if (s.ok()) {
    std::vector<std::pair<int, std::shared_ptr<SSTable>>> added;
    if (table) added.emplace_back(0, table);
    installed = ApplyEdit(*base, {}, added);
    s = WriteManifest(*installed, live_log);
    if (!s.ok() && table) RemoveFileQuietly(TablePath(number));
  }

  lk.lock();
  if (s.ok()) {
    version_ = installed;
    manifest_log_number_ = live_log;
    imm_.reset();
    Counters::Add(counters_.flushes);
  }
  lk.unlock();
  if (s.ok()) RemoveFileQuietly(LogPath(flushed_log));
  lk.lock();
  return s;
}

bool DBImpl::NeedsCompaction(const Version& v) const {
  if (static_cast<int>(v.levels[0].size()) >= options_.level0_compaction_trigger) return true;
  for (int level = 1; level < kNumLevels - 1; level++) {
    if (!v.levels[level].empty() && v.LevelBytes(level) >= MaxBytesForLevel(level)) return true;
  }
  return false;
}

bool DBImpl::PickCompaction(const Version& v, Compaction* c) {
  // Choose the level that is furthest over its budget.
  int level = -1;
  double best = 0;
  if (static_cast<int>(v.levels[0].size()) >= options_.level0_compaction_trigger) {
    level = 0;
    best = static_cast<double>(v.levels[0].size()) / options_.level0_compaction_trigger;
  }
  for (int l = 1; l < kNumLevels - 1; l++) {
    if (v.levels[l].empty()) continue;
    const uint64_t bytes = v.LevelBytes(l);
    if (bytes < MaxBytesForLevel(l)) continue;
    const double score = static_cast<double>(bytes) / static_cast<double>(MaxBytesForLevel(l));
    if (score > best) {
      level = l;
      best = score;
    }
  }
  if (level < 0) return false;

  c->level = level;
  if (level == 0) {
    c->inputs[0] = v.levels[0];  // all of level 0: its tables overlap each other
  } else {
    // Round-robin through the key space so every file gets compacted in turn.
    const auto& files = v.levels[level];
    size_t pick = 0;
    for (size_t i = 0; i < files.size(); i++) {
      if (files[i]->smallest_key() > compact_pointer_[level]) {
        pick = i;
        break;
      }
    }
    c->inputs[0] = {files[pick]};
    compact_pointer_[level] = files[pick]->largest_key();
  }

  std::string lo = c->inputs[0].front()->smallest_key();
  std::string hi = c->inputs[0].front()->largest_key();
  for (const auto& f : c->inputs[0]) {
    lo = std::min(lo, f->smallest_key());
    hi = std::max(hi, f->largest_key());
  }
  for (const auto& f : v.levels[level + 1]) {
    if (f->OverlapsRange(lo, hi)) c->inputs[1].push_back(f);
  }
  for (const auto& f : c->inputs[1]) {
    lo = std::min(lo, f->smallest_key());
    hi = std::max(hi, f->largest_key());
  }

  // A tombstone can be discarded only if no deeper level could still hold an
  // older value it needs to shadow.
  c->drop_tombstones = true;
  for (int l = level + 2; l < kNumLevels && c->drop_tombstones; l++) {
    for (const auto& f : v.levels[l]) {
      if (f->OverlapsRange(lo, hi)) {
        c->drop_tombstones = false;
        break;
      }
    }
  }
  return true;
}

Status DBImpl::RunCompaction(std::unique_lock<std::mutex>& lk) {
  VersionPtr base = version_;
  Compaction c;
  if (!PickCompaction(*base, &c)) return Status::OK();
  lk.unlock();

  std::vector<std::shared_ptr<SSTable>> outputs;
  Status s = DoCompaction(c, &outputs);

  VersionPtr installed;
  if (s.ok()) {
    std::set<uint64_t> removed;
    for (const auto& side : c.inputs) {
      for (const auto& f : side) removed.insert(f->file_number());
    }
    std::vector<std::pair<int, std::shared_ptr<SSTable>>> added;
    for (const auto& t : outputs) added.emplace_back(c.level + 1, t);
    installed = ApplyEdit(*base, removed, added);
    s = WriteManifest(*installed, manifest_log_number_);
    if (!s.ok()) {
      for (const auto& t : outputs) RemoveFileQuietly(t->path());
    }
  }

  lk.lock();
  if (s.ok()) {
    version_ = installed;
    Counters::Add(counters_.compactions);
  }
  lk.unlock();
  if (s.ok()) {
    // Readers holding the old version keep the data alive through open fds.
    for (const auto& side : c.inputs) {
      for (const auto& f : side) RemoveFileQuietly(f->path());
    }
  }
  lk.lock();
  return s;
}

Status DBImpl::DoCompaction(const Compaction& c, std::vector<std::shared_ptr<SSTable>>* outputs) {
  // Source order is recency order: level L (newer) before level L+1 (older).
  std::vector<std::unique_ptr<KVIterator>> children;
  if (c.level == 0) {
    for (const auto& f : c.inputs[0]) {  // newest first
      auto it = f->NewIterator(/*fill_cache=*/false);
      it->SeekToFirst();
      children.push_back(std::move(it));
    }
  } else {
    auto it = std::make_unique<LevelIterator>(c.inputs[0], /*fill_cache=*/false);
    it->SeekToFirst();
    children.push_back(std::move(it));
  }
  if (!c.inputs[1].empty()) {
    auto it = std::make_unique<LevelIterator>(c.inputs[1], /*fill_cache=*/false);
    it->SeekToFirst();
    children.push_back(std::move(it));
  }
  uint64_t bytes_read = 0;
  for (const auto& side : c.inputs) {
    for (const auto& f : side) bytes_read += f->file_size();
  }

  MergingIterator merged(std::move(children));
  std::unique_ptr<SSTableBuilder> builder;
  uint64_t builder_number = 0;
  uint64_t bytes_written = 0;

  auto finish_output = [&]() -> Status {
    if (!builder) return Status::OK();
    Status fs_ = builder->Finish();
    if (fs_.ok()) {
      std::shared_ptr<SSTable> table;
      fs_ = SSTable::Open(options_, builder->path(), builder_number, &cache_, &counters_, &table);
      if (fs_.ok()) {
        bytes_written += table->file_size();
        outputs->push_back(std::move(table));
      }
    }
    if (!fs_.ok()) builder->Abandon();
    builder.reset();
    return fs_;
  };

  Status s;
  for (; s.ok() && merged.Valid(); merged.Next()) {
    if (c.drop_tombstones && merged.type() == ValueType::kDeletion) continue;
    if (!builder) {
      builder_number = NewFileNumber();
      builder = std::make_unique<SSTableBuilder>(options_, TablePath(builder_number));
      s = builder->Open();
      if (!s.ok()) break;
    }
    s = builder->Add(merged.key(), merged.type(), merged.value());
    if (s.ok() && builder->FileSize() >= options_.target_file_size) s = finish_output();
  }
  if (s.ok()) s = merged.status();
  if (s.ok()) s = finish_output();

  if (!s.ok()) {
    if (builder) builder->Abandon();
    for (const auto& t : *outputs) RemoveFileQuietly(t->path());
    outputs->clear();
    return s;
  }
  Counters::Add(counters_.compaction_bytes_read, bytes_read);
  Counters::Add(counters_.compaction_bytes_written, bytes_written);
  return Status::OK();
}

DBImpl::VersionPtr DBImpl::ApplyEdit(
    const Version& base, const std::set<uint64_t>& removed,
    const std::vector<std::pair<int, std::shared_ptr<SSTable>>>& added) const {
  auto v = std::make_shared<Version>();
  for (int level = 0; level < kNumLevels; level++) {
    for (const auto& f : base.levels[level]) {
      if (removed.count(f->file_number()) == 0) v->levels[level].push_back(f);
    }
  }
  for (const auto& [level, table] : added) v->levels[level].push_back(table);
  SortLevels(v.get());
  return v;
}

// -------------------------------------------------------------- diagnostics

Stats DBImpl::GetStats() const {
  Stats s;
  s.puts = Counters::Load(counters_.puts);
  s.deletes = Counters::Load(counters_.deletes);
  s.gets = Counters::Load(counters_.gets);
  s.get_hits = Counters::Load(counters_.get_hits);
  s.scans = Counters::Load(counters_.scans);
  s.memtable_hits = Counters::Load(counters_.memtable_hits);
  s.table_lookups = Counters::Load(counters_.table_lookups);
  s.bloom_filter_skips = Counters::Load(counters_.bloom_filter_skips);
  s.bloom_false_positives = Counters::Load(counters_.bloom_false_positives);
  s.block_reads = Counters::Load(counters_.block_reads);
  s.block_cache_hits = Counters::Load(counters_.block_cache_hits);
  s.flushes = Counters::Load(counters_.flushes);
  s.compactions = Counters::Load(counters_.compactions);
  s.compaction_bytes_read = Counters::Load(counters_.compaction_bytes_read);
  s.compaction_bytes_written = Counters::Load(counters_.compaction_bytes_written);
  s.write_stalls = Counters::Load(counters_.write_stalls);
  s.wal_bytes = Counters::Load(counters_.wal_bytes);

  VersionPtr version;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    version = version_;
  }
  for (int level = 0; level < kNumLevels; level++) {
    s.level_files[level] = version->levels[level].size();
    s.level_bytes[level] = version->LevelBytes(level);
  }
  return s;
}

std::string DBImpl::DebugString() const {
  const Stats s = GetStats();
  std::string out;
  char buf[128];
  for (int level = 0; level < kNumLevels; level++) {
    if (s.level_files[level] == 0) continue;
    std::snprintf(buf, sizeof(buf), "L%d: %" PRIu64 " files, %" PRIu64 " bytes\n", level,
                  s.level_files[level], s.level_bytes[level]);
    out += buf;
  }
  if (out.empty()) out = "(no tables; all data is in the memtable/WAL)\n";
  return out;
}

}  // namespace kv
