#include "iterator.h"

#include <cassert>
#include <utility>

namespace LSMKV {

Iterator::Iterator() = default;

Iterator::~Iterator() {
  // 每次先摘下一个节点，避免 unique_ptr 链表递归析构导致深栈。
  while (cleanup_head_) {
    auto node = std::move(cleanup_head_);
    cleanup_head_ = std::move(node->next);
    node->function(node->arg1, node->arg2);
  }
  cleanup_tail_ = nullptr;
}

void Iterator::RegisterCleanup(CleanupFunction function, void* arg1, void* arg2) {
  assert(function != nullptr);
  auto node = std::make_unique<CleanupNode>();
  node->function = function;
  node->arg1 = arg1;
  node->arg2 = arg2;
  CleanupNode* tail = node.get();
  if (cleanup_tail_ == nullptr) {
    cleanup_head_ = std::move(node);
  } else {
    cleanup_tail_->next = std::move(node);
  }
  cleanup_tail_ = tail;
}

namespace {

// 空和错误迭代器都没有 entry；区别仅在其拥有的 Status。
class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(const Status& status) : status_(status) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}

  void Next() override { assert(Valid()); }
  void Prev() override { assert(Valid()); }
  Slice key() const override {
    assert(Valid());
    return Slice();
  }
  Slice value() const override {
    assert(Valid());
    return Slice();
  }
  Status status() const override { return status_; }

 private:
  const Status status_;
};

}  // namespace

Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

Iterator* NewErrorIterator(const Status& status) {
  return new EmptyIterator(status);
}

}  // namespace LSMKV
