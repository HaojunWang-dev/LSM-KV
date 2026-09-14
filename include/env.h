#pragma once

#include "slice.h"
#include "status.h"

#include <cstdint>
#include <memory>
#include <string>

namespace LSMKV {

// WAL 的顺序写文件接口；调用方负责生命周期。

class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  virtual ~WritableFile() = default;

  // 追加数据到文件尾。
  virtual Status Append(const Slice& data) = 0;

  // 关闭文件；之后不再允许写入。
  virtual Status Close() = 0;

  // 提交用户态缓冲区，不保证持久化。
  virtual Status Flush() = 0;

  // 持久化已追加的数据。
  virtual Status Sync() = 0;
};

// WAL 恢复使用的顺序读文件接口。
class SequentialFile {
public:
  SequentialFile() = default;

  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  virtual ~SequentialFile() = default;

  // 读取至多 n 字节，result 借用 scratch。
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

  // 向前跳过 n 字节。
  virtual Status Skip(uint64_t n) = 0;
};

class Env {
 public:
  virtual ~Env() = default;

  virtual Status NewWritableFile(
      const std::string& filename,
      std::unique_ptr<WritableFile>* result) = 0;
  virtual Status NewSequentialFile(
      const std::string& filename,
      std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status FileExists(const std::string& filename, bool* exists) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;

  static Env* Default();
};
}  // namespace LSMKV
