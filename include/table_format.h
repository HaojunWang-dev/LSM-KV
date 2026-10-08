#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "slice.h"
#include "status.h"

namespace LSMKV {

class RandomAccessFile;
struct ReadOptions;

// SSTable 文件布局：data blocks / meta blocks / metaindex / index / footer。
// footer 中的 fixed64 magic 使用 coding.h 的 little-endian 编码。
inline constexpr std::uint64_t kTableMagicNumber = 0xdb4775248b80fb57ULL;

// 每个存储块后跟 [1-byte compression type][fixed32 masked CRC32C]。
// CRC 覆盖存储的块字节及 compression type；trailer 不计入 BlockHandle::size()。
inline constexpr std::size_t kBlockTrailerSize = 1 + 4;

// 指向文件中的一个块，不拥有文件或块内存；offset 从文件起点计数。
// 磁盘编码为 [varint64 offset][varint64 size]，不是本对象的内存布局。
class BlockHandle {
 public:
  static constexpr std::size_t kMaxEncodedLength = 2 * 10;

  BlockHandle() = default;

  std::uint64_t offset() const { return offset_; }
  void set_offset(std::uint64_t offset) { offset_ = offset; }

  std::uint64_t size() const { return size_; }
  void set_size(std::uint64_t size) { size_ = size; }

  // 追加编码，不清空 dst；调用方须先设置 offset 和 size。
  void EncodeTo(std::string* dst) const;

  // 成功时消费两个 varint；格式错误返回 Corruption，不改变输入及本对象。
  // 这里只解析编码，块范围及 offset + size + trailer 的溢出由读取层校验。
  Status DecodeFrom(Slice* input);

 private:
  // 未设置的哨兵值；构造对象不意味着已获得可用于读取的块地址。
  std::uint64_t offset_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t size_ = std::numeric_limits<std::uint64_t>::max();
};

// 位于 SSTable 末尾的固定长度信息：
// [metaindex handle][index handle][padding to 40 bytes][fixed64 magic]。
class Footer {
 public:
  // 磁盘编码恰好为 48 字节，与 sizeof(Footer) 无关。
  static constexpr std::size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  Footer() = default;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& handle) { metaindex_handle_ = handle; }

  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& handle) { index_handle_ = handle; }

  // 向 dst 追加固定长度 footer；两个句柄须已设置，剩余 handle 区域补零。
  void EncodeTo(std::string* dst) const;

  // 校验长度、magic 及句柄编码；成功消费 48 字节，保留其后的输入。
  // 失败返回 Corruption，不改变输入及本对象。
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// ReadBlock() 的输出；data 是不包含 trailer 的块字节视图。
// 本结构不自动释放内存。heap_allocated 为 true 时，调用方负责 delete[]
// data.data()，或将释放责任转交给后续 Block；不能让两个对象同时负责释放。
// 为 false 时 data 借用外部存储，其拥有者的生命周期必须覆盖视图的使用。
// 复制本结构只复制视图和标志，不复制块字节，也不自动转移释放责任。
struct BlockContents {
  Slice data;
  bool cachable = false;        // 是否允许将块内容加入缓存。
  bool heap_allocated = false;  // 是否需要由调用方释放 data 指向的数组。
};

// 借用非空 file，调用方保证本次读取期间有效；result 必须非空。
// 按 handle 读取块及 trailer，结合 options 决定是否校验 checksum。
// 成功后填写三个字段：通过 heap_allocated 指明释放责任，通过 cachable
// 指明缓存资格；借用 file 所持存储时，调用方须保证 file 的生命周期。
// I/O 错误保留原 Status，截断或损坏返回 Corruption；失败不改变 result。
// 此处仅声明接口，尚未提供读取、解压或 checksum 校验的实现。
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

}  // namespace LSMKV
