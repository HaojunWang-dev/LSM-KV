#pragma once

#include <cstdint>

#include "options.h"
#include "slice.h"
#include "status.h"

namespace LSMKV {

class BlockBuilder;
class BlockHandle;
class WritableFile;

// 将按 comparator 严格递增的 key/value 构建成不可变 SSTable。
// 本文件仅声明接口；不代表已实现文件构建、压缩或过滤器。
// 无修改操作时可并发调用 const 方法；若有线程修改本对象，
// 所有访问都必须由调用方同步，同一输出文件也不得被其他写入者使用。
class TableBuilder {
 public:
  // 保存 options 的副本，但借用其中的 comparator 等外部对象。
  // file 必须非空，且至少存活到 Finish() 或 Abandon() 返回。
  // 不拥有、关闭或删除 file；Sync() 和 Close() 均由调用方负责。
  TableBuilder(const Options& options, WritableFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 析构前必须已经调用 Finish() 或 Abandon()，即使构建遇到错误也一样。
  ~TableBuilder();

  // 更新允许动态调整的配置，不能更换 comparator。
  // 若包含不允许的变更，返回错误且保留原有配置；要求构建尚未结束。
  Status ChangeOptions(const Options& options);

  // key 必须按 comparator 严格大于上一条 key；支持二进制 key/value。
  // 不保留对输入字节的借用；要求尚未 Finish() 或 Abandon()。
  // I/O 等构建错误通过 status() 或 Finish() 获取。
  void Add(const Slice& key, const Slice& value);

  // 将当前缓冲的 data block 写出，使随后添加的 entry 位于新的 block。
  // 要求构建尚未结束；不等同于 WritableFile::Sync()，不保证持久化。
  void Flush();

  // 返回构建过程中记录的错误，未遇到错误时返回 OK。
  Status status() const;

  // 写出剩余数据、索引及 footer，结束构建并返回最终状态。
  // 无论成功或失败，返回后均不再使用 file；不负责 Sync() 或 Close()。
  // 要求尚未 Finish() 或 Abandon()；调用后不能继续 Add() 或 Flush()。
  Status Finish();

  // 放弃构建并停止使用 file；要求尚未 Finish() 或 Abandon()。
  // 不回滚已写入字节，也不关闭或删除文件；不完整文件由调用方清理。
  void Abandon();

  // 已添加的 entry 数。
  std::uint64_t NumEntries() const;

  // 构建器记录的已生成文件字节数；尚未写出的缓冲数据不计入。
  // 成功 Finish() 后为最终文件大小。
  std::uint64_t FileSize() const;

 private:
  bool ok() const { return status().ok(); }

  // 写出一个 block 及 trailer，并记录其文件位置。
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);

  // data 为已按 type 准备好的块字节；写出字节及压缩类型/checksum trailer。
  // BlockHandle 的 size 不包含 trailer；压缩方式沿用类枚举 CompressionType。
  void WriteRawBlock(const Slice& data, CompressionType type, BlockHandle* handle);

  struct Rep;
  Rep* rep_;  // 本对象拥有内部状态，由析构函数释放；Rep 不拥有输出文件。
};

}  // namespace LSMKV
