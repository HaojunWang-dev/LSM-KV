#pragma once

#include "slice.h"
#include "status.h"

namespace LSMKV {

// 有序 key/value 扫描接口。
class Iterator {
 public:
  Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator();

  // 是否指向有效 entry。
  virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;

  virtual void SeekToLast() = 0;

  // 定位到第一个 key 不小于 target 的 entry。
  virtual void Seek(const Slice& target) = 0;

  virtual void Next() = 0;

  virtual void Prev() = 0;

  // 返回当前 key；迭代器移动后失效。
  virtual Slice key() const = 0;

  // 返回当前 value；迭代器移动后失效。
  virtual Slice value() const = 0;

  // 返回迭代期间的错误状态。
  virtual Status status() const = 0;

  // 析构时执行的资源清理回调。
  using CleanupFunction = void (*)(void* arg1, void* arg2);

  // 注册析构回调，按注册顺序执行。
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  struct CleanupNode {
    CleanupFunction function;
    void* arg1;
    void* arg2;
    CleanupNode* next;
  };

  CleanupNode* cleanup_head_;

  CleanupNode* cleanup_tail_;
};

// 返回状态为 OK 的空迭代器。
Iterator* NewEmptyIterator();

// 返回携带指定错误的空迭代器。
Iterator* NewErrorIterator(const Status& status);

}  // namespace LSMKV
