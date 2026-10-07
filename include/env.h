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

  // 创建并截断同名文件。result 必须非空；成功时返回调用方拥有的对象，
  // 调用方负责 Close()/delete（也可交给 RAII 管理）；失败时 *result 为 nullptr。
  // 工厂不释放输出槽中原来的对象，复用输出槽前应由调用方处理其所有权。
  virtual Status NewWritableFile(const std::string& filename,
                                 WritableFile** result) = 0;
  // 打开顺序读取文件。成功时返回调用方拥有的对象，调用方负责 delete
  //（也可交给 RAII 管理）；失败时 *result 为 nullptr。输出槽规则同上。
  virtual Status NewSequentialFile(const std::string& filename,
                                   SequentialFile** result) = 0;
  virtual Status FileExists(const std::string& filename, bool* exists) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;

  // 重命名文件；同一文件系统内替换已有目标是原子的，但不隐含持久化。
  virtual Status RenameFile(const std::string& source,
                            const std::string& target) = 0;

  // 删除单个文件或符号链接，不递归删除目录；缺失文件返回 NotFound。
  virtual Status RemoveFile(const std::string& filename) = 0;

  // 持久化目录项的创建、删除和重命名；不代替文件自身的 Sync()。
  virtual Status SyncDir(const std::string& dirname) = 0;

  static Env* Default();
};
}  // namespace LSMKV
