#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "db.h"
#include "env.h"
#include "format.h"
#include "log_writer.h"
#include "memtable.h"
#include "slice.h"
#include "status.h"
#include "options.h"
#include "write_batch.h"

namespace LSMKV {

// 协调 WAL 与 MemTable 的 DB 实现。
class DBImpl final : public DB {
 public:
  // 打开或创建数据库，并将所有权交给 result。
  static Status Open(const Options& options, const std::string& dbname,
                     std::unique_ptr<DBImpl>* result);

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  ~DBImpl() override;

  Status Put(const WriteOptions& options, const Slice& key,
             const Slice& value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
 private:
  // 写队列节点，定义在实现文件中。
  struct Writer;

  DBImpl(const Options& options, const std::string& dbname);

  Status NewDB();

  Status OpenLogFile();

  // 写入 batch，并按需同步 WAL。
  Status WriteToWAL(const WriteOptions& options, WriteBatch* batch);

  // 按顺序将 batch 应用到 MemTable。
  void ApplyBatchToMemTable(WriteBatch* batch);

  // 为 batch 预留连续 sequence。
  SequenceNumber AllocateSequence(size_t count);

  // 合并队首开始的一组兼容写入。
  WriteBatch* BuildBatchGroup(Writer** last_writer);

  std::string LogFileName() const;

  const Options options_;

  const std::string dbname_;

  // DBImpl 持有当前 WAL 文件。
  std::unique_ptr<WritableFile> log_file_;

  // Writer 借用 log_file_。
  std::unique_ptr<log::Writer> log_writer_;

  // 当前可写 MemTable。
  std::unique_ptr<MemTable> mem_;

  SequenceNumber last_sequence_ = 0;

  // 保护写队列、sequence、WAL 和 MemTable。
  std::mutex mutex_;

  // 等待写入的 Writer 队列。
  std::deque<Writer*> writers_;

  // 合并多个 Writer 的临时 batch。
  std::unique_ptr<WriteBatch> tmp_batch_;

  Env* const env_;
};

}  // namespace LSMKV
