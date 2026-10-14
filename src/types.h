#pragma once

#include <cstddef>
#include <cstdint>

namespace kv {

// A deletion is recorded as a tombstone so it can shadow older versions of the
// key living in lower levels until compaction proves they are gone.
enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };

constexpr size_t kMaxKeySize = 64 * 1024;
constexpr size_t kMaxValueSize = 64 * 1024 * 1024;

}  // namespace kv
