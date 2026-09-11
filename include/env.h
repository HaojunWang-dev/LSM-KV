#pragma once

#include "slice.h"
#include "status.h"

namespace LSMKV {

// WritableFile 是 WAL 所需的顺序写文件抽象。
// 调用方负责对象生命周期；同一个实例只允许一个写线程访问。

class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  // 通过基类指针销毁派生 POSIX 文件对象。
  virtual ~WritableFile() = default;

  // 将 data 追加到文件尾。实现应缓冲小片段写入，并在失败时返回 IOError。
  virtual Status Append(const Slice& data) = 0;

  // 关闭文件及其底层资源。成功 Close() 后不得再调用其他写操作。
  virtual Status Close() = 0;

  // 将实现自身的用户态缓冲区提交给内核；不保证数据已持久化到稳定介质。
  virtual Status Flush() = 0;

  // 将此前已 Append 的数据持久化到稳定介质；WAL 的提交点依赖此操作。
  virtual Status Sync() = 0;
};

}  // namespace LSMKV
