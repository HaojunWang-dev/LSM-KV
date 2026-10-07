#pragma once

#include "slice.h"
#include "status.h"

#include <cstdarg>
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

// 诊断日志接收器，不是用于恢复的 WAL；调用方负责其生命周期。
// 实现必须支持并发 Logv()，并在调用期间消费参数，不得保存借用的参数。
class Logger {
 public:
  Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  virtual ~Logger() = default;

  // format 必须为有效的 printf 格式；args 仅在本次调用期间有效。
  virtual void Logv(const char* format, std::va_list args) = 0;
};

class Env {
 public:
  virtual ~Env() = default;

  // 创建并截断同名文件。result 必须非空；成功时返回调用方拥有的对象，
  // 调用方负责 Close()/delete（也可交给 RAII 管理）；失败时 *result 为 nullptr。
  // 工厂不释放输出槽中原来的对象，复用输出槽前应由调用方处理其所有权。
  virtual Status NewWritableFile(const std::string& filename,
                                 WritableFile** result) = 0;
  // 以追加模式打开文件，不截断已有内容；缺失时创建。
  // 输出及所有权规则同 NewWritableFile；默认实现返回 NotSupported。
  virtual Status NewAppendableFile(const std::string& filename,
                                  WritableFile** result);
  // 打开顺序读取文件。成功时返回调用方拥有的对象，调用方负责 delete
  //（也可交给 RAII 管理）；失败时 *result 为 nullptr。输出槽规则同上。
  virtual Status NewSequentialFile(const std::string& filename,
                                   SequentialFile** result) = 0;
  virtual Status FileExists(const std::string& filename, bool* exists) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;

  // 查询文件字节数。size 必须非空；失败时清零，不创建或修改文件。
  // 默认实现返回 NotSupported，允许不支持 MANIFEST 复用的 Env 正常恢复。
  virtual Status GetFileSize(const std::string& filename, uint64_t* size);

  // 重命名文件；同一文件系统内替换已有目标是原子的，但不隐含持久化。
  virtual Status RenameFile(const std::string& source,
                            const std::string& target) = 0;

  // 删除单个文件或符号链接，不递归删除目录；缺失文件返回 NotFound。
  virtual Status RemoveFile(const std::string& filename) = 0;

  // 持久化目录项的创建、删除和重命名；不代替文件自身的 Sync()。
  virtual Status SyncDir(const std::string& dirname) = 0;

  static Env* Default();
};

// 同步读取整个文件；借用 env，内部创建的 SequentialFile 由本函数释放。
// data 必须非空且不能与 filename 别名；调用开始时清空 data。
// 支持二进制内容，空文件成功；打开失败时 data 为空，读取失败时保留
// 此前成功读取的前缀。调用方必须检查 Status，不能把错误时的前缀当作完整文件。
Status ReadFileToString(Env* env, const std::string& filename, std::string* data);

}  // namespace LSMKV
