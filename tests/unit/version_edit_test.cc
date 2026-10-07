#include "VersionEdit.h"

#include <string>
#include <type_traits>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "coding.h"

namespace LSMKV {
namespace {

TEST(VersionEditTest, FileMetadataOwnsItsInternalKeyBounds) {
  FileMetaData file;
  EXPECT_EQ(file.refs, 0);
  EXPECT_EQ(file.number, 0U);
  EXPECT_EQ(file.file_size, 0U);

  InternalKey smallest(Slice("a\0b", 3), 10, ValueType::kValue);
  InternalKey largest(Slice("z"), 20, ValueType::kDeletion);
  file.smallest = smallest;
  file.largest = largest;
  smallest.Clear();
  largest.Clear();

  EXPECT_EQ(file.smallest.user_key().ToString(), std::string("a\0b", 3));
  EXPECT_EQ(file.largest.user_key().ToString(), "z");
}

TEST(VersionEditTest, DeclaresLevelDBStyleEditApi) {
  static_assert(std::is_same_v<decltype(&VersionEdit::EncodeTo),
                              void (VersionEdit::*)(std::string*) const>);
  static_assert(std::is_same_v<decltype(&VersionEdit::DecodeFrom),
                              Status (VersionEdit::*)(const Slice&)>);
  static_assert(std::is_same_v<decltype(&VersionEdit::DebugString),
                              std::string (VersionEdit::*)() const>);

  VersionEdit edit;
  edit.SetComparatorName(Slice("bytewise"));
  edit.SetLogNumber(1);
  edit.SetPrevLogNumber(0);
  edit.SetNextFile(2);
  edit.SetLastSequence(kMaxSequenceNumber);
  InternalKey first(Slice("a"), 10, ValueType::kValue);
  InternalKey last(Slice("z"), 5, ValueType::kDeletion);
  edit.SetCompactPointer(0, first);
  edit.AddFile(0, 3, 4096, first, last);
  edit.RemoveFile(0, 4);
  edit.Clear();
}

std::string EncodeEdit(const VersionEdit& edit) {
  std::string encoded;
  edit.EncodeTo(&encoded);
  return encoded;
}

TEST(VersionEditTest, UsesLevelDBTagsAndAppendsToDestination) {
  VersionEdit edit;
  edit.SetComparatorName(Slice("cmp"));
  edit.SetLogNumber(128);
  edit.SetPrevLogNumber(0);
  edit.SetNextFile(129);
  edit.SetLastSequence(7);
  edit.RemoveFile(6, 5);
  edit.RemoveFile(6, 5);

  const std::string expected("\x01\x03" "cmp" "\x02\x80\x01"
                             "\x09\x00\x03\x81\x01\x04\x07\x06\x06\x05", 18);
  EXPECT_EQ(EncodeEdit(edit), expected);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(Slice(expected)).ok());
  EXPECT_EQ(EncodeEdit(decoded), expected);

  std::string destination("prefix");
  edit.EncodeTo(&destination);
  EXPECT_EQ(destination, "prefix" + expected);
}

TEST(VersionEditTest, EncodesFileBoundsAndCompactionPointerWithLengthPrefixes) {
  VersionEdit edit;
  InternalKey key(Slice(), 0, ValueType::kValue);
  edit.SetCompactPointer(6, key);
  edit.AddFile(0, 1, 2, key, key);

  // 空 user key 的 tag 为 01 00 00 00 00 00 00 00，长度为 8。
  const std::string expected(
      "\x05\x06\x08\x01\x00\x00\x00\x00\x00\x00\x00"
      "\x07\x00\x01\x02"
      "\x08\x01\x00\x00\x00\x00\x00\x00\x00"
      "\x08\x01\x00\x00\x00\x00\x00\x00\x00", 33);
  EXPECT_EQ(EncodeEdit(edit), expected);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(Slice(expected)).ok());
  EXPECT_EQ(EncodeEdit(decoded), expected);
}

TEST(VersionEditTest, RoundTripsAllFieldsAndOwnsDecodedBytes) {
  VersionEdit edit;
  edit.SetComparatorName(Slice("cmp\0binary", 10));
  edit.SetLogNumber(std::numeric_limits<std::uint64_t>::max());
  edit.SetPrevLogNumber(0);
  edit.SetNextFile(1ULL << 40);
  edit.SetLastSequence(kMaxSequenceNumber);
  InternalKey first(Slice("a\0b", 3), kMaxSequenceNumber, ValueType::kValue);
  InternalKey last(Slice(std::string(256, 'z')), 0, ValueType::kDeletion);
  edit.SetCompactPointer(0, first);
  edit.SetCompactPointer(6, last);
  edit.RemoveFile(6, 1ULL << 40);
  edit.RemoveFile(0, 128);
  edit.AddFile(6, 1ULL << 40, std::numeric_limits<std::uint64_t>::max(),
               first, last);
  std::string encoded = EncodeEdit(edit);
  const std::string expected = encoded;
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(Slice(encoded)).ok());
  encoded.assign(encoded.size(), 'x');
  EXPECT_EQ(EncodeEdit(decoded), expected);
}

TEST(VersionEditTest, ClearAndEmptyDecodeRemoveAllPreviousChanges) {
  VersionEdit edit;
  edit.SetComparatorName(Slice(""));
  edit.SetLogNumber(0);
  edit.SetPrevLogNumber(0);
  edit.SetNextFile(0);
  edit.SetLastSequence(0);
  const std::string explicit_zero("\x01\x00\x02\x00\x09\x00\x03\x00\x04\x00", 10);
  EXPECT_EQ(EncodeEdit(edit), explicit_zero);
  ASSERT_TRUE(edit.DecodeFrom(Slice()).ok());
  EXPECT_TRUE(EncodeEdit(edit).empty());
  ASSERT_TRUE(edit.DecodeFrom(Slice(explicit_zero)).ok());
  edit.SetCompactPointer(0, InternalKey(Slice("a"), 1, ValueType::kValue));
  edit.AddFile(0, 1, 2, InternalKey(Slice("a"), 1, ValueType::kValue),
               InternalKey(Slice("z"), 0, ValueType::kValue));
  edit.RemoveFile(1, 3);
  edit.Clear();
  EXPECT_TRUE(EncodeEdit(edit).empty());
}

TEST(VersionEditTest, ReplacesStateAndUsesLastRepeatedScalar) {
  VersionEdit edit;
  edit.SetNextFile(99);
  const std::string input("\x02\x01\x02\x80\x01\x06\x00\x05\x06\x00\x05", 11);
  ASSERT_TRUE(edit.DecodeFrom(Slice(input)).ok());
  EXPECT_EQ(EncodeEdit(edit), std::string("\x02\x80\x01\x06\x00\x05", 6));
}

TEST(VersionEditTest, RejectsEveryTruncatedSingleField) {
  InternalKey key(Slice("a"), 10, ValueType::kValue);
  std::vector<VersionEdit> edits(8);
  edits[0].SetComparatorName(Slice("cmp"));
  edits[1].SetLogNumber(128);
  edits[2].SetPrevLogNumber(128);
  edits[3].SetNextFile(128);
  edits[4].SetLastSequence(128);
  edits[5].SetCompactPointer(0, key);
  edits[6].RemoveFile(0, 128);
  edits[7].AddFile(0, 128, 4096, key, key);
  for (const auto& source : edits) {
    const std::string encoded = EncodeEdit(source);
    for (size_t size = 1; size < encoded.size(); ++size) {
      SCOPED_TRACE(size);
      VersionEdit decoded;
      EXPECT_TRUE(decoded.DecodeFrom(Slice(encoded.data(), size)).IsCorruption());
      EXPECT_TRUE(EncodeEdit(decoded).empty());
    }
  }
}

TEST(VersionEditTest, RejectsMalformedFieldsAndClearsPartialDecode) {
  const std::vector<std::string> malformed = {
      std::string("\x08", 1),                         // 保留 tag。
      std::string("\x00", 1),                         // 未知 tag。
      std::string("\x80", 1),                         // 截断 tag。
      std::string("\xff\xff\xff\xff\x10", 5),     // tag 溢出。
      std::string("\x01\xff\xff\xff\xff\x10", 6), // 长度溢出。
      std::string("\x02") + std::string(10, '\xff') + '\x02', // uint64 溢出。
      std::string("\x06\x07\x00", 3),               // level 7。
      std::string("\x05\x07\x00", 3),               // pointer level 7。
      std::string("\x07\x07\x00\x00", 4),           // file level 7。
      std::string("\x06\xff\xff\xff\xff\x0f\x00", 7), // 负层号编码。
      std::string("\x05\x00\x00", 3),               // 空 internal key。
      std::string("\x05\x00\x01x", 4),              // 短 internal key。
      std::string("\x05\x00\x08\x02\x00\x00\x00\x00\x00\x00\x00", 11),
      std::string("\x04\x80\x80\x80\x80\x80\x80\x80\x80\x01", 10), // sequence 2^56。
  };
  for (const auto& input : malformed) {
    VersionEdit edit;
    edit.SetNextFile(99);
    const std::string with_prefix = std::string("\x02\x01", 2) + input;
    const Status status = edit.DecodeFrom(Slice(with_prefix));
    EXPECT_TRUE(status.IsCorruption()) << status.ToString();
    EXPECT_NE(status.ToString().find("VersionEdit"), std::string::npos);
    EXPECT_TRUE(EncodeEdit(edit).empty());
  }
}

TEST(VersionEditTest, RejectsMalformedFileBounds) {
  std::string input("\x07\x00\x01\x02", 4);
  PutLengthPrefixedSlice(&input, Slice("short"));
  PutLengthPrefixedSlice(&input, InternalKey(Slice("z"), 0, ValueType::kValue).Encode());
  VersionEdit edit;
  EXPECT_TRUE(edit.DecodeFrom(Slice(input)).IsCorruption());

}

TEST(VersionEditTest, DecodesBoundsWithoutAssumingUserKeyOrder) {
  // z..a 是逆序比较器下的合法边界。解码器只有名称，不能推断排序规则。
  VersionEdit reversed;
  reversed.SetComparatorName(Slice("test.ReverseBytewise"));
  reversed.AddFile(0, 1, 2, InternalKey(Slice("z"), 0, ValueType::kValue),
                   InternalKey(Slice("a"), 0, ValueType::kValue));
  const std::string encoded = EncodeEdit(reversed);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(Slice(encoded)).ok());
  EXPECT_EQ(EncodeEdit(decoded), encoded);
}

TEST(VersionEditTest, DebugStringDescribesAllChangesWithoutRawControlBytes) {
  VersionEdit edit;
  edit.SetComparatorName(Slice("cmp\0\n", 5));
  edit.SetLogNumber(1);
  edit.SetPrevLogNumber(0);
  edit.SetNextFile(2);
  edit.SetLastSequence(3);
  InternalKey first(Slice("a\0", 2), 4, ValueType::kValue);
  InternalKey last(Slice("z"), 5, ValueType::kDeletion);
  edit.SetCompactPointer(6, first);
  edit.RemoveFile(0, 8);
  edit.AddFile(1, 9, 4096, first, last);
  const std::string debug = edit.DebugString();
  for (const char* field : {"Comparator:", "LogNumber: 1", "PrevLogNumber: 0",
                            "NextFile: 2", "LastSeq: 3", "CompactPointer: 6",
                            "RemoveFile: 0 8", "AddFile: 1 9 4096",
                            "a\\x00", "cmp\\x00\\x0a", "@ 4 : 1", "@ 5 : 0"}) {
    EXPECT_NE(debug.find(field), std::string::npos) << debug;
  }
  EXPECT_EQ(debug.find('\0'), std::string::npos);
  edit.Clear();
  EXPECT_EQ(edit.DebugString().find("LogNumber"), std::string::npos);
}

}  // namespace
}  // namespace LSMKV
