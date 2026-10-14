// Crash-recovery tests. A "crash" is simulated two ways:
//   * snapshot: copy the directory of a live DB (the OS page cache makes the
//     WAL visible), then open the copy, as if the process had died there;
//   * kill: fork a child that writes continuously and SIGKILL it mid-flight.

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "io.h"
#include "kv/db.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

kv::Options SmallOptions() {
  kv::Options o;
  o.write_buffer_size = 16 * 1024;
  o.target_file_size = 8 * 1024;
  o.block_size = 512;
  o.level1_max_bytes = 64 * 1024;
  o.level0_compaction_trigger = 3;
  o.level0_stop_writes_trigger = 8;
  return o;
}

std::string Key(int i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}
std::string Value(int i) { return "value-" + std::to_string(i) + std::string(60, 'v'); }

std::unique_ptr<kv::DB> OpenDB(const kv::Options& o, const std::string& path) {
  std::unique_ptr<kv::DB> db;
  CHECK_OK(kv::DB::Open(o, path, &db));
  return db;
}

std::string GetOr(kv::DB* db, const std::string& key) {
  std::string v;
  kv::Status s = db->Get(key, &v);
  if (s.IsNotFound()) return "<missing>";
  if (!s.ok()) KV_FAIL("Get failed: " + s.ToString());
  return v;
}

// Copies a quiescent DB directory (minus the lock file) to `dst`.
void Snapshot(const std::string& src, const std::string& dst) {
  fs::create_directories(dst);
  for (const auto& e : fs::directory_iterator(src)) {
    if (e.path().filename() == "LOCK") continue;
    fs::copy_file(e.path(), fs::path(dst) / e.path().filename());
  }
}

std::vector<std::string> FilesWithExtension(const std::string& dir, const std::string& ext) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ext) out.push_back(e.path().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(Recovery_ReplaysWalAfterUncleanShutdown) {
  kvtest::TempDir dir("rec");
  auto db = OpenDB(kv::Options(), dir.file("live"));  // 4 MiB buffer: nothing flushes
  for (int i = 0; i < 500; i++) CHECK_OK(db->Put(Key(i), Value(i)));
  CHECK_OK(db->Delete(Key(10)));
  CHECK_OK(db->Put(Key(20), "overwritten"));
  CHECK_EQ(db->GetStats().flushes, uint64_t(0));

  Snapshot(dir.file("live"), dir.file("crashed"));
  auto recovered = OpenDB(kv::Options(), dir.file("crashed"));
  for (int i = 0; i < 500; i++) {
    std::string want = Value(i);
    if (i == 10) want = "<missing>";
    if (i == 20) want = "overwritten";
    CHECK_EQ(GetOr(recovered.get(), Key(i)), want);
  }
}

TEST(Recovery_TornWalTailLosesOnlyTheLastRecord) {
  kvtest::TempDir dir("rec");
  auto db = OpenDB(kv::Options(), dir.file("live"));
  for (int i = 0; i < 200; i++) CHECK_OK(db->Put(Key(i), Value(i)));

  for (uintmax_t cut : {uintmax_t(1), uintmax_t(7), uintmax_t(30)}) {
    const std::string copy = dir.file("crash-" + std::to_string(cut));
    Snapshot(dir.file("live"), copy);
    auto logs = FilesWithExtension(copy, ".log");
    CHECK(!logs.empty());
    fs::resize_file(logs.back(), fs::file_size(logs.back()) - cut);

    auto recovered = OpenDB(kv::Options(), copy);
    for (int i = 0; i < 199; i++) CHECK_EQ(GetOr(recovered.get(), Key(i)), Value(i));
    CHECK_EQ(GetOr(recovered.get(), Key(199)), std::string("<missing>"));
    // The recovered DB must be fully usable.
    CHECK_OK(recovered->Put(Key(199), "again"));
    CHECK_EQ(GetOr(recovered.get(), Key(199)), std::string("again"));
  }
}

TEST(Recovery_StaleLogOfAlreadyFlushedDataIsNotReplayed) {
  kvtest::TempDir dir("rec");
  auto db = OpenDB(kv::Options(), dir.file("live"));
  CHECK_OK(db->Put("k", "old"));

  // Remember the log that holds Put(k, old)...
  const std::string stale_snapshot = dir.file("stale");
  Snapshot(dir.file("live"), stale_snapshot);
  const auto stale_logs = FilesWithExtension(stale_snapshot, ".log");
  CHECK_EQ(stale_logs.size(), size_t(1));

  // ...then supersede and flush it, which retires that log.
  CHECK_OK(db->Flush());
  CHECK_OK(db->Put("k", "new"));
  CHECK_OK(db->Flush());
  db->WaitForBackgroundWork();

  // Crash image: current files, plus the retired log reappearing (as if the
  // crash hit after the MANIFEST update but before the unlink).
  const std::string crashed = dir.file("crashed");
  Snapshot(dir.file("live"), crashed);
  fs::copy_file(stale_logs.front(), fs::path(crashed) / fs::path(stale_logs.front()).filename(),
                fs::copy_options::overwrite_existing);

  auto recovered = OpenDB(kv::Options(), crashed);
  CHECK_EQ(GetOr(recovered.get(), "k"), std::string("new"));
}

TEST(Recovery_DiscardsOrphanTablesAndTempManifest) {
  kvtest::TempDir dir("rec");
  {
    auto db = OpenDB(SmallOptions(), dir.file("db"));
    for (int i = 0; i < 3000; i++) CHECK_OK(db->Put(Key(i), Value(i)));
    db->WaitForBackgroundWork();
  }
  // A crash between writing a table and committing it leaves such a file
  // behind; so does a crash mid manifest write.
  const std::string orphan = dir.file("db") + "/999999.sst";
  { std::ofstream(orphan) << "half-written garbage"; }
  { std::ofstream(dir.file("db") + "/MANIFEST.tmp") << "partial"; }

  auto db = OpenDB(SmallOptions(), dir.file("db"));
  CHECK(!fs::exists(orphan));
  CHECK(!fs::exists(dir.file("db") + "/MANIFEST.tmp"));
  for (int i = 0; i < 3000; i += 13) CHECK_EQ(GetOr(db.get(), Key(i)), Value(i));
}

TEST(Recovery_CorruptManifestIsReportedNotIgnored) {
  kvtest::TempDir dir("rec");
  {
    auto db = OpenDB(kv::Options(), dir.file("db"));
    CHECK_OK(db->Put("a", "b"));
  }
  CHECK_OK(kv::WriteFileAtomic(dir.file("db") + "/MANIFEST", "KVSTORE-MANIFEST 1\nlog 3\n"));
  std::unique_ptr<kv::DB> db;
  CHECK(kv::DB::Open(kv::Options(), dir.file("db"), &db).IsCorruption());

  CHECK_OK(kv::WriteFileAtomic(dir.file("db") + "/MANIFEST", "garbage"));
  CHECK(kv::DB::Open(kv::Options(), dir.file("db"), &db).IsCorruption());
}

TEST(Recovery_MissingTableFileIsReportedNotSilentlyDropped) {
  kvtest::TempDir dir("rec");
  {
    auto db = OpenDB(SmallOptions(), dir.file("db"));
    for (int i = 0; i < 1000; i++) CHECK_OK(db->Put(Key(i), Value(i)));
    CHECK_OK(db->Flush());
    db->WaitForBackgroundWork();
  }
  const auto tables = FilesWithExtension(dir.file("db"), ".sst");
  CHECK(!tables.empty());
  fs::remove(tables.front());
  std::unique_ptr<kv::DB> db;
  CHECK(!kv::DB::Open(SmallOptions(), dir.file("db"), &db).ok());
}

TEST(Recovery_CorruptTableSurfacesCorruptionOnRead) {
  kvtest::TempDir dir("rec");
  {
    auto db = OpenDB(SmallOptions(), dir.file("db"));
    for (int i = 0; i < 400; i++) CHECK_OK(db->Put(Key(i), Value(i)));
    CHECK_OK(db->Flush());
    db->WaitForBackgroundWork();
  }
  for (const auto& table : FilesWithExtension(dir.file("db"), ".sst")) {
    std::string bytes;
    CHECK_OK(kv::ReadFileToString(table, &bytes));
    bytes[10] ^= 0x7f;
    CHECK_OK(kv::WriteFileAtomic(table, bytes));
  }
  kv::Options opts = SmallOptions();
  opts.block_cache_capacity = 0;
  auto db = OpenDB(opts, dir.file("db"));
  std::string v;
  kv::Status s = db->Get(Key(0), &v);
  CHECK(s.IsCorruption());  // never returns garbage as if it were data
}

TEST(Crash_SigkillDuringWritesLosesNoAcknowledgedWrite) {
  kvtest::TempDir dir("kill");
  const std::string path = dir.file("db");
  const kv::Options opts = SmallOptions();

  // Child publishes how many writes it has had acknowledged here.
  auto* acked = static_cast<std::atomic<uint32_t>*>(
      mmap(nullptr, sizeof(std::atomic<uint32_t>), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  CHECK(acked != MAP_FAILED);
  new (acked) std::atomic<uint32_t>(0);

  std::mt19937 rng(99);
  uint32_t durable = 0;  // keys [0, durable) must survive every restart
  uint32_t total_acked = 0;
  for (int round = 0; round < 6; round++) {
    acked->store(durable);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      std::unique_ptr<kv::DB> db;
      if (!kv::DB::Open(opts, path, &db).ok()) _exit(2);
      for (uint32_t i = durable; i < durable + 5000000; i++) {
        // Mix in overwrites and deletes so compactions have real work.
        if (i % 11 == 10 && !db->Delete(Key(static_cast<int>(i - 7))).ok()) _exit(3);
        if (!db->Put(Key(static_cast<int>(i)), Value(static_cast<int>(i))).ok()) _exit(3);
        acked->store(i + 1, std::memory_order_release);
      }
      _exit(0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40 + rng() % 120));
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));  // died from our kill, not from a failure path

    const uint32_t now_acked = acked->load();
    CHECK(now_acked > durable);  // made progress each round
    total_acked = now_acked;

    // Iteration j+7 deletes key j when (j+7) % 11 == 10. Iterations
    // [0, total_acked) completed; iteration `total_acked` may have been cut
    // off after its Delete, so that one deletion is allowed either way.
    auto db = OpenDB(opts, path);
    for (uint32_t j = 0; j < total_acked; j++) {
      const bool delete_iteration = (j + 7) % 11 == 10;
      const std::string got = GetOr(db.get(), Key(static_cast<int>(j)));
      const std::string value = Value(static_cast<int>(j));
      bool ok;
      if (delete_iteration && j + 7 < total_acked) {
        ok = got == "<missing>";
      } else if (delete_iteration && j + 7 == total_acked) {
        ok = got == "<missing>" || got == value;
      } else {
        ok = got == value;
      }
      if (!ok) {
        KV_FAIL("round " + std::to_string(round) + ": acknowledged write " + std::to_string(j) +
                " was lost or corrupted");
      }
    }
    durable = total_acked;
  }
  munmap(acked, sizeof(std::atomic<uint32_t>));
  CHECK(total_acked > 1000);
}
