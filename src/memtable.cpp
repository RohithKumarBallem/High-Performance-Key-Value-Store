#include "memtable.h"

namespace kv {

void MemTable::Insert(std::string_view key, ValueType type, std::string_view value) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  auto it = map_.lower_bound(key);
  const size_t usage = usage_.load(std::memory_order_relaxed);
  if (it != map_.end() && it->first == key) {
    const size_t old_size = it->second.value.size();
    it->second.type = type;
    it->second.value.assign(value.data(), value.size());
    usage_.store(usage - old_size + value.size(), std::memory_order_relaxed);
  } else {
    map_.emplace_hint(it, std::string(key), Entry{type, std::string(value)});
    usage_.store(usage + key.size() + value.size() + kEntryOverhead, std::memory_order_relaxed);
  }
}

bool MemTable::Get(std::string_view key, std::string* value, ValueType* type) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  *type = it->second.type;
  if (it->second.type == ValueType::kValue) value->assign(it->second.value);
  return true;
}

bool MemTable::Empty() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return map_.empty();
}

void MemTable::ReadChunk(const std::string& after, bool inclusive, size_t max_items,
                         std::vector<Item>* out) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = inclusive ? map_.lower_bound(after) : map_.upper_bound(after);
  for (; it != map_.end() && out->size() < max_items; ++it) {
    out->push_back(Item{it->first, it->second.type, it->second.value});
  }
}

}  // namespace kv
