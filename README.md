# High-Performance Key-Value Store

A crash-resilient, multithreaded, persistent key-value store in C++17, built as a
log-structured merge tree (LSM-tree) with a write-ahead log, bloom filters, and
background leveled compaction. No dependencies beyond the standard library and POSIX.

```cpp
#include "kv/db.h"

std::unique_ptr<kv::DB> db;
kv::Status s = kv::DB::Open(kv::Options(), "/tmp/mydb", &db);

db->Put("user:1", "ada");
std::string value;
db->Get("user:1", &value);                       // OK, value == "ada"
db->Delete("user:1");
db->Get("user:1", &value).IsNotFound();          // true

std::vector<std::pair<std::string, std::string>> rows;
db->Scan("user:", "user;", /*limit=*/100, &rows);  // ordered range scan
```

## How it works

```
                 Put / Delete                               Get / Scan
                      |                                         |
          +-----------v-----------+                 +-----------v-----------+
          | 1. append to WAL      |                 |  active memtable      |
          |    (CRC-checked)      |                 |  immutable memtable   |
          | 2. insert in memtable |                 |  L0 tables (newest→)  |
          +-----------+-----------+                 |  L1 … L5 (one table   |
                      | memtable full               |   per level per key)  |
                      v                             +-----------------------+
            freeze + new WAL;  background thread
                      |
                      v                  Leveled compaction (background):
        flush -> L0 SSTable              L0 -> L1 when L0 has >= 4 tables
                      |                  Ln -> Ln+1 when Ln exceeds its size budget
                      v                  Merges newest-wins, drops shadowed values,
            MANIFEST (atomic rename)     drops tombstones once nothing older remains.
```

| Component | What it does | Source |
|---|---|---|
| **WAL** | Every write is appended (`crc32 \| len \| type \| key \| value`) before it touches the memtable. Recovery replays the intact prefix and stops at the first torn or corrupt record. | [src/wal.cpp](src/wal.cpp) |
| **MemTable** | Sorted in-memory buffer under a reader/writer lock; readers run in parallel. | [src/memtable.cpp](src/memtable.cpp) |
| **SSTable** | Immutable sorted file: checksummed 4 KiB data blocks, a block index, a bloom filter, and a footer. Point lookups binary-search the index and read one block. | [src/sstable.cpp](src/sstable.cpp) |
| **Bloom filter** | 10 bits/key (~1% false positives). Lets a lookup skip a table without any disk I/O. | [src/bloom_filter.cpp](src/bloom_filter.cpp) |
| **Block cache** | 16-way sharded LRU over decoded blocks, so concurrent readers rarely contend. | [src/sstable.cpp](src/sstable.cpp) |
| **Compaction** | One background thread flushes memtables (priority) and runs leveled compaction. Writers get backpressure instead of unbounded L0 growth. | [src/db_impl.cpp](src/db_impl.cpp) |
| **MANIFEST** | The authoritative list of live tables, replaced atomically (temp file + fsync + rename). Anything on disk it doesn't mention is crash debris and is deleted on open. | [src/db_impl.cpp](src/db_impl.cpp) |

### Concurrency model

* Writers are serialized by one mutex (WAL order must match memtable order). A writer holds
  it for its own WAL append and memtable insert, and while waiting out backpressure; it
  never performs a flush or compaction itself.
* Readers take a brief lock only to pin a consistent *(memtable, immutable memtable,
  version)* triple as `shared_ptr`s, then read with no lock held. Compaction swaps in a new
  immutable `Version` and unlinks old files; readers still holding the old version keep
  working through their open file descriptors.
* A single background thread does all flush and compaction work, so the table set only
  ever changes from one place.

### Durability, precisely

| `Options::sync_writes` | Process crash / `kill -9` | Power loss / kernel panic |
|---|---|---|
| `false` (default) | Every acknowledged write survives (it is in the OS page cache). | Database reopens consistent; the most recent writes may be lost. |
| `true` | Every acknowledged write survives. | Every acknowledged write survives (`fsync`; `F_FULLFSYNC` on macOS). |

Tables and the MANIFEST are always `fsync`ed, so the structure on disk is consistent in
both modes. Reads verify block checksums and report `Corruption` rather than returning
bad data.

## Build and run

Requires a C++17 compiler and `make`. Built and tested on macOS (Apple clang) and Linux
(GCC).

```sh
make            # library, kvcli, kvbench, kv_tests   (output in build/)
make test       # run the test suite
make bench      # run the benchmark
make test SAN=undefined   # also: SAN=address, SAN=thread
```

```sh
build/kvcli /tmp/mydb put hello world
build/kvcli /tmp/mydb get hello
build/kvcli /tmp/mydb            # interactive: put/get/del/scan/flush/stats
```

Embed it by compiling `src/*.cpp` (or linking `build/libkvstore.a`) with `-Iinclude`;
the public API is [include/kv/db.h](include/kv/db.h) and
[include/kv/options.h](include/kv/options.h).

## Testing

47 tests, `make test` runs in about 7 seconds:

* **Model check** — 40,000 random put/delete/get operations (with periodic restarts and
  many flushes/compactions) compared against a `std::map` oracle, plus a full-scan
  comparison at the end.
* **Crash recovery** — `SIGKILL` a writing child process at random points, six rounds on the
  same database, and require that *every acknowledged write* is present with its exact
  value after reopening. Also: torn WAL tails, a retired log reappearing, orphaned tables
  and temp manifests, a corrupt MANIFEST, a missing table file, a bit-flipped table.
* **Concurrency** — writers and readers running together, asserting readers never see a
  wrong value or miss an acknowledged write; competing writers on the same keys.
* **Unit tests** — bloom filter (no false negatives, FP rate), CRC-32 against a known
  vector and a bitwise reference, WAL, SSTable (iteration, seek, corruption detection),
  merge iterator.

CI runs the full suite on Linux (release, ThreadSanitizer, AddressSanitizer, UBSan) and on
macOS, and all five jobs pass. The sanitizer runtimes don't start on the development
machine this was written on, so TSan and ASan results come from CI only.

## Benchmarks

`build/kvbench` — 200,000 keys, 100-byte values, 4 threads, default `Options`
(4 MiB write buffer, 10-bit bloom filters, 8 MiB block cache). The data set is ~22 MiB, so
it is larger than the block cache but entirely in the OS page cache: this measures the
store's CPU and syscall path, not disk seek time.

Measured on an Intel Core i7-9750H MacBook (macOS, local SSD) over three runs; the
machine had some unrelated background load, so treat the spread as part of the result.

| Workload | Throughput | Mean latency |
|---|---|---|
| Random writes, 4 threads, no per-write fsync | 65k – 106k ops/s | — |
| Random reads of existing keys | 300k – 585k ops/s | 6.5 – 12 µs |
| Random reads of absent keys, bloom filters on | 1.4M – 1.7M ops/s | 2.0 – 2.4 µs |
| Random writes, **fsync every write**, 1 thread | 26 – 31 ops/s | ~35 ms |

**Bloom filter effect** — the same on-disk data read with filters enabled and then ignored,
identical key sequence:

| Read mix | Mean latency, filters off → on | Reduction |
|---|---|---|
| 100% existing keys | 6.6 – 11.3 µs → 6.5 – 12.3 µs | none (within noise) |
| 50% existing / 50% absent | 7.2 – 13.6 µs → 4.4 – 6.9 µs | **38 – 49%** |
| 100% absent keys | 6.8 – 9.5 µs → 2.0 – 2.4 µs | 68 – 77% |

So the filters pay off in proportion to how many lookups target keys that aren't there
(or aren't in the newest tables). They do nothing for lookups that hit, which is expected:
a hit has to read its block regardless.

**Reading these numbers honestly**

* "Crash-resilient at 15,000+ ops/s" holds for process crashes (`sync_writes = false`).
  With `sync_writes = true` on this machine throughput is bounded by the drive's flush
  latency — about 30 ops/s per writer on macOS with `F_FULLFSYNC`. There is no group
  commit, so concurrent writers don't amortize the flush.
* Numbers are from a laptop with a warm page cache; they are not a statement about
  other hardware or a cold disk.

## Limitations

* Single background thread: a long compaction can delay a flush and stall writers.
* No group commit, compression, snapshots/iterator API, or column families.
* `Scan` reads tables from a consistent snapshot but sees the active memtable as it is at
  the time each chunk is visited.
* A corrupt record in the *middle* of a WAL ends replay there; later records in that log
  are not recovered (the standard "point-in-time" behaviour for a log without sync points).
* POSIX only (`pread`, `flock`).

## Layout

```
include/kv/   public API (db.h, options.h, stats.h, status.h)
src/          LSM implementation
tests/        test suite (own minimal harness; no external deps)
bench/        kvbench
tools/        kvcli
```
