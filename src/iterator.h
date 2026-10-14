#pragma once

#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "kv/status.h"
#include "memtable.h"
#include "types.h"

namespace kv {

// Forward iterator over (key, type, value) entries in ascending key order.
// key()/value() are valid until the next call to Next().
class KVIterator {
 public:
  virtual ~KVIterator() = default;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual ValueType type() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK once the iterator hit corruption or an I/O error (and is invalid).
  virtual Status status() const = 0;
};

// Walks a memtable in chunks so no lock is held between calls to Next().
class MemTableIterator final : public KVIterator {
 public:
  MemTableIterator(std::shared_ptr<const MemTable> mem, std::string start);

  bool Valid() const override { return pos_ < chunk_.size(); }
  void Next() override;
  std::string_view key() const override { return chunk_[pos_].key; }
  ValueType type() const override { return chunk_[pos_].type; }
  std::string_view value() const override { return chunk_[pos_].value; }
  Status status() const override { return Status::OK(); }

 private:
  static constexpr size_t kChunkSize = 256;

  void Refill(const std::string& after, bool inclusive);

  std::shared_ptr<const MemTable> mem_;
  std::vector<MemTable::Item> chunk_;
  size_t pos_ = 0;
  bool exhausted_ = false;
};

// K-way merge of sorted sources. `children[0]` is the newest source: when the
// same key appears in several children only the newest entry is emitted, which
// is how newer writes (and tombstones) shadow older ones. Tombstones are
// emitted; the caller decides whether to drop them.
class MergingIterator final : public KVIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<KVIterator>> children);

  bool Valid() const override { return !heap_.empty(); }
  void Next() override;
  std::string_view key() const override { return children_[heap_.top()]->key(); }
  ValueType type() const override { return children_[heap_.top()]->type(); }
  std::string_view value() const override { return children_[heap_.top()]->value(); }
  Status status() const override;

 private:
  struct Greater {
    const MergingIterator* self;
    bool operator()(size_t a, size_t b) const {
      const int c = self->children_[a]->key().compare(self->children_[b]->key());
      return c != 0 ? c > 0 : a > b;  // min-heap on key, then newest child first
    }
  };

  std::vector<std::unique_ptr<KVIterator>> children_;
  std::priority_queue<size_t, std::vector<size_t>, Greater> heap_;
  std::string last_key_;
};

}  // namespace kv
