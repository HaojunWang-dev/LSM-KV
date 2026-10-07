#include "format.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#include "coding.h"

namespace LSMKV {

std::uint64_t PackSequenceAndType(SequenceNumber sequence, ValueType type) {
  assert(sequence <= kMaxSequenceNumber);
  assert(type == ValueType::kDeletion || type == ValueType::kValue);
  return (sequence << 8) | static_cast<std::uint8_t>(type);
}

std::size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < 8) {
    return false;
  }

  const std::uint64_t packed =
      DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  const auto type = static_cast<ValueType>(packed & 0xffU);
  if (type != ValueType::kDeletion && type != ValueType::kValue) {
    return false;
  }

  result->user_key = Slice(internal_key.data(), internal_key.size() - 8);
  result->sequence = packed >> 8;
  result->type = type;
  return true;
}

Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

InternalKey::InternalKey(const Slice& user_key, SequenceNumber sequence,
                         ValueType type) {
  SetFrom(ParsedInternalKey(user_key, sequence, type));
}

bool InternalKey::DecodeFrom(const Slice& encoded) {
  rep_.assign(encoded.data(), encoded.size());
  return !rep_.empty();
}

Slice InternalKey::Encode() const { return Slice(rep_); }

Slice InternalKey::user_key() const { return ExtractUserKey(Encode()); }

void InternalKey::SetFrom(const ParsedInternalKey& key) {
  rep_.clear();
  rep_.reserve(InternalKeyEncodingLength(key));
  AppendInternalKey(&rep_, key);
}

void InternalKey::Clear() { rep_.clear(); }

std::string InternalKey::DebugString() const {
  ParsedInternalKey parsed;
  if (!ParseInternalKey(Encode(), &parsed)) {
    return "(bad)" + Encode().ToString();
  }

  std::ostringstream result;
  result << "'" << parsed.user_key.ToString() << "' @ "
         << parsed.sequence << " : "
         << static_cast<int>(parsed.type);
  return result.str();
}

InternalKeyComparator::InternalKeyComparator(const Comparator* user_comparator)
    : user_comparator_(user_comparator) {
  assert(user_comparator_ != nullptr);
}

const char* InternalKeyComparator::Name() const {
  return "LSMKV.InternalKeyComparator";
}

int InternalKeyComparator::Compare(const Slice& a, const Slice& b) const {
  const Slice user_a = ExtractUserKey(a);
  const Slice user_b = ExtractUserKey(b);

  const int user_comparison = user_comparator_->Compare(user_a, user_b);
  if (user_comparison != 0) {
    return user_comparison;
  }

  const std::uint64_t tag_a = DecodeFixed64(a.data() + a.size() - 8);
  const std::uint64_t tag_b = DecodeFixed64(b.data() + b.size() - 8);

  if (tag_a > tag_b) {
    return -1;
  }
  if (tag_a < tag_b) {
    return 1;
  }
  return 0;
}

int InternalKeyComparator::Compare(const InternalKey& a,
                                   const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                 const Slice& limit) const {
  assert(start != nullptr);
  const Slice original_user = ExtractUserKey(Slice(*start));
  const Slice limit_user = ExtractUserKey(limit);
  std::string shortened = original_user.ToString();
  user_comparator_->FindShortestSeparator(&shortened, limit_user);
  // 只替换真正缩短且逻辑上严格增加的 user key；相等时不能改掉原 tag。
  if (shortened.size() < original_user.size() &&
      user_comparator_->Compare(original_user, Slice(shortened)) < 0 &&
      user_comparator_->Compare(Slice(shortened), limit_user) < 0) {
    PutFixed64(&shortened,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(Compare(Slice(*start), Slice(shortened)) < 0);
    assert(Compare(Slice(shortened), limit) < 0);
    *start = std::move(shortened);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  assert(key != nullptr);
  const Slice original_user = ExtractUserKey(Slice(*key));
  std::string shortened = original_user.ToString();
  user_comparator_->FindShortSuccessor(&shortened);
  if (shortened.size() < original_user.size() &&
      user_comparator_->Compare(original_user, Slice(shortened)) < 0) {
    PutFixed64(&shortened,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(Compare(Slice(*key), Slice(shortened)) < 0);
    *key = std::move(shortened);
  }
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber sequence) {
  assert(user_key.size() <=
         std::numeric_limits<std::uint32_t>::max() - 8U);
  const std::size_t needed = user_key.size() + 13;
  char* dst = needed <= sizeof(space_) ? space_ : new char[needed];

  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<std::uint32_t>(user_key.size() + 8));
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), user_key.size());
  dst += user_key.size();
  EncodeFixed64(dst, PackSequenceAndType(sequence, kValueTypeForSeek));
  end_ = dst + 8;
}

LookupKey::~LookupKey() {
  if (start_ != space_) {
    delete[] start_;
  }
}

Slice LookupKey::memtable_key() const { return Slice(start_, end_ - start_); }

Slice LookupKey::internal_key() const { return Slice(kstart_, end_ - kstart_); }

Slice LookupKey::user_key() const {
  return Slice(kstart_, end_ - kstart_ - 8);
}

}  // namespace LSMKV
