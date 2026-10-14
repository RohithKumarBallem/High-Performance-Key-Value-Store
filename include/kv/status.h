#pragma once

#include <string>
#include <utility>

namespace kv {

// Result of an operation. Cheap to return and copy in the success case.
class Status {
 public:
  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") { return Status(Code::kNotFound, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(Code::kCorruption, std::move(msg)); }
  static Status IOError(std::string msg) { return Status(Code::kIOError, std::move(msg)); }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

  std::string ToString() const {
    const char* name = "OK";
    switch (code_) {
      case Code::kOk: break;
      case Code::kNotFound: name = "NotFound"; break;
      case Code::kCorruption: name = "Corruption"; break;
      case Code::kIOError: name = "IOError"; break;
      case Code::kInvalidArgument: name = "InvalidArgument"; break;
    }
    return msg_.empty() ? std::string(name) : std::string(name) + ": " + msg_;
  }

 private:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace kv
