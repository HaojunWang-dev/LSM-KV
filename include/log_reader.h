#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "log_format.h"
#include "slice.h"
#include "status.h"

namespace LSMKV {

class SequentialFile;

namespace log {

// 从 WAL 物理记录恢复完整逻辑记录。
class Reader {
 public:
  // 接收损坏记录的诊断信息；Reader 不拥有 Reporter。
  class Reporter {
   public:
    virtual ~Reporter();

    // 报告本次损坏丢弃的字节数。
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // file 与 reporter 必须在 Reader 存活期间有效。
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         std::uint64_t initial_offset);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader();

  // 读取下一条逻辑记录；record 在下次读取或修改 scratch 后失效。
  bool ReadRecord(Slice* record, std::string* scratch);

  // 返回最近一次成功读取的逻辑记录起始偏移。
 std::uint64_t LastRecordOffset();

 private:
  // 对齐到 initial_offset_ 所在的可读取 block。
  bool SkipToInitialBlock();

  // 解析一个物理记录；result 借用内部缓冲区。
  RecordType ReadPhysicalRecord(Slice* result);

  void ReportCorruption(std::uint64_t bytes, const char* reason);
  void ReportDrop(std::uint64_t bytes, const Status& reason);

  SequentialFile* const file_;  // 非 owning。
  Reporter* const reporter_;    // 非 owning，可为空。
  const bool checksum_;

  // Reader 持有 block 内存，buffer_ 仅引用其中一段。
  char* const backing_store_;
  Slice buffer_;
  bool eof_;

  std::uint64_t last_record_offset_;
  std::uint64_t end_of_buffer_offset_;
  const std::uint64_t initial_offset_;
  bool skipped_initial_block_;
  // 非零恢复偏移时丢弃孤立分片。
  bool resyncing_;
};

}  // namespace log
}  // namespace LSMKV
