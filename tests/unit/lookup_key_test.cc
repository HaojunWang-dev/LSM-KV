#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "coding.h"
#include "internal_key.h"
#include "lookup_key.h"

namespace LSMKV {
namespace {

TEST(LookupKeyTest, EncodesMemTableKeyAndInternalKeyForBinaryUserKey) {
    // MemTableKey 额外带有 varint32 长度前缀，InternalKey 则从 UserKey 开始。
    const std::string user_key("a\0b", 3);
    constexpr SequenceNumber sequence = 42;
    LookupKey lookup(Slice(user_key), sequence);

    const Slice memtable_key = lookup.MemTableKey();
    uint32_t internal_key_size = 0;
    const char* const internal_key_start =
        DecodeVarint32(memtable_key.data(), &internal_key_size);

    ASSERT_NE(internal_key_start, nullptr);
    EXPECT_EQ(internal_key_size, user_key.size() + 8);
    EXPECT_EQ(internal_key_start, lookup.InternalKey().data());
    EXPECT_EQ(lookup.InternalKey().size(), user_key.size() + 8);
    EXPECT_EQ(lookup.UserKey().ToString(), user_key);
    EXPECT_EQ(DecodeFixed64(lookup.InternalKey().data() + user_key.size()),
              PackSequenceAndType(sequence, kValueTypeForSeek));
}

TEST(LookupKeyTest, UsesMultiByteLengthPrefixAtInternalKeySizeBoundary) {
    // 120 字节 UserKey 加上 8 字节 tag 后恰为 128，varint 长度应扩展到 2 字节。
    const std::string user_key(120, 'k');
    LookupKey lookup(Slice(user_key), kMaxSequenceNumber);

    const Slice memtable_key = lookup.MemTableKey();
    uint32_t internal_key_size = 0;
    const char* const internal_key_start =
        DecodeVarint32(memtable_key.data(), &internal_key_size);

    ASSERT_NE(internal_key_start, nullptr);
    EXPECT_EQ(internal_key_size, 128U);
    EXPECT_EQ(internal_key_start - memtable_key.data(), 2);
    EXPECT_EQ(lookup.UserKey().ToString(), user_key);
    EXPECT_EQ(DecodeFixed64(lookup.InternalKey().data() + user_key.size()),
              PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
}

TEST(LookupKeyTest, SupportsEmptyUserKey) {
    // 空 UserKey 仍有一个 8 字节 tag，并可作为合法的查找目标。
    LookupKey lookup(Slice(), 0);

    EXPECT_TRUE(lookup.UserKey().empty());
    EXPECT_EQ(lookup.InternalKey().size(), 8U);
    EXPECT_EQ(lookup.MemTableKey().size(), 9U);
}

}  // namespace
}  // namespace LSMKV
