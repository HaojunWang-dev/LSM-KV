#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "comparator.h"
#include "slice.h"

namespace LSMKV {


// Grouping of constants.  We may want to make some of these
// parameters set via options.
namespace config {
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
static const int kReadBytesPeriod = 1048576;

}  // namespace config

using SequenceNumber = std::uint64_t;

enum class ValueType : std::uint8_t {
  kDeletion = 0,
  kValue = 1,
};

constexpr SequenceNumber kMaxSequenceNumber =
    (static_cast<SequenceNumber>(1) << 56) - 1;

constexpr ValueType kValueTypeForSeek = ValueType::kValue;

std::uint64_t PackSequenceAndType(SequenceNumber sequence, ValueType type);

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() = default;
  ParsedInternalKey(const Slice& user_key, SequenceNumber sequence,
                   ValueType type)
      : user_key(user_key), sequence(sequence), type(type) {}
};

std::size_t InternalKeyEncodingLength(const ParsedInternalKey& key);
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

Slice ExtractUserKey(const Slice& internal_key);

class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(const Slice& user_key, SequenceNumber sequence, ValueType type);

  bool DecodeFrom(const Slice& encoded);
  Slice Encode() const;
  Slice user_key() const;
  void SetFrom(const ParsedInternalKey& key);
  void Clear();
  std::string DebugString() const;

 private:
  std::string rep_;
};

// 借用 user_comparator；先按其规则比较 user key，再按完整 tag 降序。
// 输入必须包含有效的 internal key 编码。比较器及其规则须保持有效且稳定。
class InternalKeyComparator : public Comparator {
 public:
  explicit InternalKeyComparator(
      const Comparator* user_comparator = BytewiseComparator());

  const char* Name() const override;
  int Compare(const Slice& a, const Slice& b) const override;
  int Compare(const InternalKey& a, const InternalKey& b) const;
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;  // Non-owning，须比本对象活得更久。
};

// LookupKey owns its encoded bytes. Slices returned by this class borrow
// storage owned by the LookupKey and remain valid until it is destroyed.
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);
  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;
  ~LookupKey();

  Slice memtable_key() const;
  Slice internal_key() const;
  Slice user_key() const;

 private:
  const char* start_ = nullptr;
  const char* kstart_ = nullptr;
  const char* end_ = nullptr;
  char space_[200] = {};
};

}  // namespace LSMKV
