#pragma once

#include <cstddef>
#include <cstdint>

#include "log_format.h"
#include "slice.h"
#include "status.h"

namespace LSMKV {

// WAL Writer 不拥有该对象；调用方必须保证其生命周期覆盖 Writer。
class WritableFile;

namespace log {

// Writer 将一条逻辑记录编码为一个或多个带 header 的物理 WAL record。
// 一条记录不会跨越 kBlockSize 边界：必要时拆为 FIRST/MIDDLE/LAST。
class Writer {
 public:
  // 创建向空文件追加记录的 Writer。dest 必须在 Writer 存活期间保持有效。
  explicit Writer(WritableFile* dest);

  // 创建向已有 WAL 末尾追加记录的 Writer；dest_length 必须是文件当前长度。
  Writer(WritableFile* dest, std::uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  // 写入一条逻辑记录，必要时将其拆分为多个物理记录。
  Status AddRecord(const Slice& slice);

 private:
  // 写出一个物理记录：[masked crc][length][type][payload]。
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;  // 非 owning；由调用方管理生命周期。
  int block_offset_;    // 当前 block 内已写入的字节数，范围为 [0, kBlockSize)。

  // 预计算每种 RecordType 的 CRC 前缀，避免每条物理记录重复计算 type 的 CRC。
  std::uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace log
}  // namespace LSMKV
