#pragma once

#include <memory>

#include "slice.h"
#include "status.h"

namespace LSMKV {

// 有序 key/value 扫描接口；Seek 和遍历使用所属数据源的比较规则。
// 同一实例的移动、访问、注册清理和析构需要调用方同步。
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

  // Next/Prev/key/value 都要求 Valid() 为 true。
  virtual void Next() = 0;

  virtual void Prev() = 0;

  // 返回借用的当前 key；至少在下次移动或析构前有效。
  virtual Slice key() const = 0;

  // 返回借用的当前 value；至少在下次移动或析构前有效。
  virtual Slice value() const = 0;

  // 返回迭代期间的错误状态。
  virtual Status status() const = 0;

  // 析构时执行的资源清理回调。
  using CleanupFunction = void (*)(void* arg1, void* arg2);

  // 注册非空析构回调，按注册顺序执行且每个注册项仅执行一次。
  // 回调在派生类析构之后执行，不能访问已析构的派生对象、
  // 抛出异常或重新操作本迭代器。arg1/arg2 可为空；非空参数所指对象
  // 必须在回调使用期间有效，其释放策略由回调和调用方约定。
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  struct CleanupNode {
    CleanupFunction function;
    void* arg1;
    void* arg2;
    std::unique_ptr<CleanupNode> next;
  };

  std::unique_ptr<CleanupNode> cleanup_head_;  // 拥有全部清理节点。

  CleanupNode* cleanup_tail_ = nullptr;  // 借用链表最后一个节点。
};

// 返回状态为 OK 的空迭代器。返回值归调用方所有，建议用 unique_ptr 接管。
Iterator* NewEmptyIterator();

// 返回持有 status 副本的空迭代器（也允许 OK）。所有权同 NewEmptyIterator。
Iterator* NewErrorIterator(const Status& status);

}  // namespace LSMKV
