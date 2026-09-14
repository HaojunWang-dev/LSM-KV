#pragma once

namespace LSMKV {

namespace log {

enum class RecordType {
  kZeroType = 0,

  kFullType = 1,

  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,

  // Reader 专用状态，不写入 WAL。
  kEof = kLastType + 1,
  // 损坏、无效或应跳过的物理记录。
  kBadRecord = kLastType + 2,
};

static const int kMaxRecordType = static_cast<int> (RecordType::kLastType);

static const int kBlockSize = 32768;

static const int kHeaderSize = 4 + 2 + 1;
} // namespace log
} // namespace LSMKV
