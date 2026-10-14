#pragma once

// A deliberately tiny test harness: TEST() registers a function, CHECK*()
// aborts the current test by throwing, and test_main.cpp runs everything.

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "kv/status.h"

namespace kvtest {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template <typename T>
std::string Show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

// Scratch directory that is removed when the test ends.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

}  // namespace kvtest

#define TEST(name)                                                       \
  static void name();                                                    \
  static ::kvtest::Registrar kvtest_registrar_##name(#name, &name);      \
  static void name()

#define KV_FAIL(msg)                                                              \
  throw ::kvtest::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg))

#define CHECK(cond) \
  do {              \
    if (!(cond)) KV_FAIL(std::string("CHECK failed: ") + #cond); \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    const auto& kv_a = (a);                                                              \
    const auto& kv_b = (b);                                                              \
    if (!(kv_a == kv_b)) {                                                               \
      KV_FAIL(std::string("CHECK_EQ failed: ") + #a + " == " + #b + " (" +               \
              ::kvtest::Show(kv_a) + " vs " + ::kvtest::Show(kv_b) + ")");               \
    }                                                                                    \
  } while (0)

#define CHECK_OK(expr)                                                                   \
  do {                                                                                   \
    const ::kv::Status kv_s = (expr);                                                    \
    if (!kv_s.ok()) KV_FAIL(std::string("CHECK_OK failed: ") + #expr + " -> " + kv_s.ToString()); \
  } while (0)
