// Interactive shell for a kvstore database.
//
//   kvcli <db-dir>                       read commands from stdin
//   kvcli <db-dir> put <key> <value>     run a single command
//
// Commands: put <key> <value> | get <key> | del <key> |
//           scan [start [end [limit]]] | flush | stats | help | quit

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kv/db.h"

namespace {

std::vector<std::string> Tokenize(const std::string& line, size_t max_tokens) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size()) break;
    if (out.size() + 1 == max_tokens) {  // last token swallows the rest of the line
      out.push_back(line.substr(i));
      break;
    }
    size_t j = i;
    while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) j++;
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

// Returns false on `quit`.
bool Run(kv::DB* db, const std::string& line) {
  const auto head = Tokenize(line, 2);
  if (head.empty()) return true;
  const std::string& cmd = head[0];

  if (cmd == "quit" || cmd == "exit") return false;

  if (cmd == "put") {
    const auto t = Tokenize(line, 3);
    if (t.size() < 3) {
      std::puts("usage: put <key> <value>");
    } else {
      kv::Status s = db->Put(t[1], t[2]);
      std::puts(s.ok() ? "OK" : s.ToString().c_str());
    }
  } else if (cmd == "get") {
    const auto t = Tokenize(line, 2);
    if (t.size() < 2) {
      std::puts("usage: get <key>");
    } else {
      std::string v;
      kv::Status s = db->Get(t[1], &v);
      std::puts(s.ok() ? v.c_str() : s.ToString().c_str());
    }
  } else if (cmd == "del") {
    const auto t = Tokenize(line, 2);
    if (t.size() < 2) {
      std::puts("usage: del <key>");
    } else {
      kv::Status s = db->Delete(t[1]);
      std::puts(s.ok() ? "OK" : s.ToString().c_str());
    }
  } else if (cmd == "scan") {
    const auto t = Tokenize(line, 4);
    const std::string start = t.size() > 1 ? t[1] : "";
    const std::string end = t.size() > 2 ? t[2] : "";
    const size_t limit = t.size() > 3 ? std::stoul(t[3]) : 100;
    std::vector<std::pair<std::string, std::string>> rows;
    kv::Status s = db->Scan(start, end, limit, &rows);
    for (const auto& [k, v] : rows) std::printf("%s = %s\n", k.c_str(), v.c_str());
    std::printf("(%zu rows)%s\n", rows.size(), s.ok() ? "" : (" " + s.ToString()).c_str());
  } else if (cmd == "flush") {
    kv::Status s = db->Flush();
    std::puts(s.ok() ? "OK" : s.ToString().c_str());
  } else if (cmd == "stats") {
    const kv::Stats st = db->GetStats();
    std::printf("puts=%llu deletes=%llu gets=%llu hits=%llu\n", (unsigned long long)st.puts,
                (unsigned long long)st.deletes, (unsigned long long)st.gets,
                (unsigned long long)st.get_hits);
    std::printf("flushes=%llu compactions=%llu bloom_skips=%llu block_reads=%llu\n",
                (unsigned long long)st.flushes, (unsigned long long)st.compactions,
                (unsigned long long)st.bloom_filter_skips, (unsigned long long)st.block_reads);
    std::fputs(db->DebugString().c_str(), stdout);
  } else if (cmd == "help") {
    std::puts("put <key> <value> | get <key> | del <key> | scan [start [end [limit]]]\n"
              "flush | stats | quit");
  } else {
    std::printf("unknown command '%s' (try 'help')\n", cmd.c_str());
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <db-dir> [command...]\n", argv[0]);
    return 2;
  }
  std::unique_ptr<kv::DB> db;
  kv::Status s = kv::DB::Open(kv::Options(), argv[1], &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  if (argc > 2) {
    std::string line;
    for (int i = 2; i < argc; i++) {
      if (i > 2) line += ' ';
      line += argv[i];
    }
    Run(db.get(), line);
    return 0;
  }

  std::string line;
  while (true) {
    if (isatty(0)) std::fputs("kv> ", stdout);
    if (!std::getline(std::cin, line)) break;
    if (!Run(db.get(), line)) break;
  }
  return 0;
}
