#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"

namespace LSMKV {

struct Options;

// 构建与 Block 读取格式对应的未压缩 block：前缀压缩的 entries，
// 后接 fixed32 restart offsets 和 fixed32 restart count。
// 本类拥有编码缓冲区；不负责压缩、checksum、block trailer 或文件 I/O。
// 同一实例的访问由调用方串行化。
class BlockBuilder {
 public:
  // 借用 options 及其 comparator，二者须覆盖 builder 的生命周期。
  // options 和 comparator 必须非空，构建期间不能改变排序规则。
  explicit BlockBuilder(const Options* options);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // 清空内容并恢复到初始构建状态，可在 Finish() 后复用。
  // 使此前 Finish() 返回的 Slice 失效。
  void Reset();

  // 要求尚未 Finish()，且 key 按 options 的 comparator 严格大于上一条 key。
  // key/value 可以包含 '\0'；调用结束后不保留对输入字节的借用。
  void Add(const Slice& key, const Slice& value);

  // 追加 restart 数组及其数量，结束本轮构建；每轮只能调用一次。
  // 返回值借用 buffer_，在 Reset() 或 builder 析构前有效。
  // Finish() 后必须先 Reset() 才能再次 Add()。
  Slice Finish();

  // 当前未压缩 block 的估算字节数，包含 restart 数组及数量字段。
  std::size_t CurrentSizeEstimate() const;

  // 构建阶段用于判断是否尚未添加 entry；Finish() 会向缓冲区追加元数据。
  bool empty() const { return buffer_.empty(); }

 private:
  const Options* options_;               // 非 owning 的配置引用。
  std::string buffer_;                   // entry 编码及完成后的 block 内容。
  std::vector<std::uint32_t> restarts_;   // 不依赖前一条 key 的 entry 的偏移。
  int counter_;                         // 当前 restart 分组已写入的 entry 数。
  bool finished_;                       // 本轮是否已经调用 Finish()。
  std::string last_key_;                 // 上一条完整 key，用于前缀压缩及顺序校验。
};

}  // namespace LSMKV
