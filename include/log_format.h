#pragma once

namespace LSMKV {

namespace log {

enum class RecordType {
  kZeroType = 0,

  kFullType = 1,

  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};

static const int kMaxRecordType = static_cast<int> (RecordType::kLastType);

static const int kBlockSize = 32768;

static const int kHeaderSize = 4 + 2 + 1;
} // namespace log
} // namespace LSMKV