#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "io.h"
#include "test_framework.h"
#include "wal.h"

namespace {

using Record = std::tuple<kv::ValueType, std::string, std::string>;

std::vector<Record> Replay(const std::string& path, kv::WalReplayInfo* info) {
  std::vector<Record> out;
  CHECK_OK(kv::ReplayWal(
      path,
      [&](kv::ValueType t, std::string_view k, std::string_view v) {
        out.emplace_back(t, std::string(k), std::string(v));
      },
      info));
  return out;
}

void WriteRecords(const std::string& path, int n) {
  std::unique_ptr<kv::WalWriter> w;
  CHECK_OK(kv::WalWriter::Create(path, false, &w));
  for (int i = 0; i < n; i++) {
    if (i % 5 == 4) {
      CHECK_OK(w->Append(kv::ValueType::kDeletion, "k" + std::to_string(i), ""));
    } else {
      CHECK_OK(w->Append(kv::ValueType::kValue, "k" + std::to_string(i), "value-" + std::to_string(i)));
    }
  }
}

}  // namespace

TEST(Wal_RoundTrip) {
  kvtest::TempDir dir("wal");
  WriteRecords(dir.file("a.log"), 100);

  kv::WalReplayInfo info;
  auto recs = Replay(dir.file("a.log"), &info);
  CHECK_EQ(recs.size(), size_t(100));
  CHECK(!info.torn_tail());
  CHECK_EQ(info.records, uint64_t(100));
  CHECK(std::get<0>(recs[4]) == kv::ValueType::kDeletion);
  CHECK_EQ(std::get<1>(recs[7]), std::string("k7"));
  CHECK_EQ(std::get<2>(recs[7]), std::string("value-7"));
}

TEST(Wal_TornTailKeepsIntactPrefix) {
  kvtest::TempDir dir("wal");
  const std::string path = dir.file("a.log");
  WriteRecords(path, 50);
  const auto full = std::filesystem::file_size(path);

  // Chop the file mid-record, as a crash during write() would.
  for (uintmax_t cut : {uintmax_t(1), uintmax_t(5), uintmax_t(9), uintmax_t(19)}) {
    std::filesystem::resize_file(path, full - cut);
    kv::WalReplayInfo info;
    auto recs = Replay(path, &info);
    CHECK_EQ(recs.size(), size_t(49));
    CHECK(info.torn_tail());
    CHECK_EQ(std::get<1>(recs.back()), std::string("k48"));
  }
}

TEST(Wal_CorruptRecordStopsReplay) {
  kvtest::TempDir dir("wal");
  const std::string path = dir.file("a.log");
  WriteRecords(path, 30);

  // Flip a byte inside the value of some middle record.
  std::string bytes;
  CHECK_OK(kv::ReadFileToString(path, &bytes));
  bytes[bytes.size() / 2] ^= 0x40;
  CHECK_OK(kv::WriteFileAtomic(path, bytes));

  kv::WalReplayInfo info;
  auto recs = Replay(path, &info);
  CHECK(recs.size() < 30);
  CHECK(recs.size() > 5);
  CHECK(info.torn_tail());
  // Whatever was recovered must be an exact prefix of what was written.
  for (size_t i = 0; i < recs.size(); i++) {
    CHECK_EQ(std::get<1>(recs[i]), "k" + std::to_string(i));
  }
}

TEST(Wal_EmptyAndGarbageFiles) {
  kvtest::TempDir dir("wal");
  CHECK_OK(kv::WriteFileAtomic(dir.file("empty.log"), ""));
  kv::WalReplayInfo info;
  CHECK_EQ(Replay(dir.file("empty.log"), &info).size(), size_t(0));
  CHECK(!info.torn_tail());

  CHECK_OK(kv::WriteFileAtomic(dir.file("junk.log"), std::string(100, '\xab')));
  CHECK_EQ(Replay(dir.file("junk.log"), &info).size(), size_t(0));
  CHECK(info.torn_tail());
}
