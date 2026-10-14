#include "iterator.h"

namespace kv {

MemTableIterator::MemTableIterator(std::shared_ptr<const MemTable> mem, std::string start)
    : mem_(std::move(mem)) {
  Refill(start, /*inclusive=*/true);
}

void MemTableIterator::Refill(const std::string& after, bool inclusive) {
  chunk_.clear();
  pos_ = 0;
  mem_->ReadChunk(after, inclusive, kChunkSize, &chunk_);
  exhausted_ = chunk_.size() < kChunkSize;
}

void MemTableIterator::Next() {
  if (++pos_ < chunk_.size() || exhausted_) return;
  // Resume strictly after the last key of the chunk we just consumed. Copy it
  // because Refill() clears chunk_.
  const std::string last = chunk_.back().key;
  Refill(last, /*inclusive=*/false);
}

MergingIterator::MergingIterator(std::vector<std::unique_ptr<KVIterator>> children)
    : children_(std::move(children)), heap_(Greater{this}) {
  for (size_t i = 0; i < children_.size(); i++) {
    if (children_[i]->Valid()) heap_.push(i);
  }
}

void MergingIterator::Next() {
  last_key_.assign(key());
  // Advance every child positioned at the emitted key, so older duplicates of
  // it are skipped as well.
  while (!heap_.empty() && children_[heap_.top()]->key() == last_key_) {
    const size_t i = heap_.top();
    heap_.pop();
    children_[i]->Next();
    if (children_[i]->Valid()) heap_.push(i);
  }
}

Status MergingIterator::status() const {
  for (const auto& child : children_) {
    Status s = child->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace kv
