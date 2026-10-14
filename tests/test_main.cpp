#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "test_framework.h"

namespace kvtest {

TempDir::TempDir(const std::string& label) {
  static std::atomic<int> counter{0};
  path_ = (std::filesystem::temp_directory_path() /
           ("kvtest-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + "-" + label))
              .string();
  std::filesystem::remove_all(path_);
  std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

}  // namespace kvtest

// Usage: kv_tests [substring-filter]
int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed = 0;
  for (const auto& t : kvtest::Registry()) {
    if (filter && std::strstr(t.name, filter) == nullptr) continue;
    ran++;
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    try {
      t.fn();
    } catch (const std::exception& e) {
      error = e.what();
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    if (error.empty()) {
      std::printf("[ PASS ] %-44s %8.1f ms\n", t.name, ms);
    } else {
      failed++;
      std::printf("[ FAIL ] %-44s %8.1f ms\n         %s\n", t.name, ms, error.c_str());
    }
    std::fflush(stdout);
  }
  std::printf("\n%d tests, %d failed\n", ran, failed);
  return failed == 0 ? 0 : 1;
}
