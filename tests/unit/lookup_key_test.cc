#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "coding.h"
#include "format.h"

namespace LSMKV {
namespace {

TEST(LookupKeyTest, EncodesMemTableKeyAndInternalKeyForBinaryUserKey) {
    // MemTableKey 额外带有 varint32 长度前缀，InternalKey 则从 UserKey 开始。
    const std::string user_key("a\0b", 3);
    constexpr SequenceNumber sequence = 42;
    LookupKey lookup(Slice(user_key), sequence);

    const Slice memtable_key = lookup.memtable_key();
    uint32_t internal_key_size = 0;
    const char* const internal_key_start =
        DecodeVarint32(memtable_key.data(),
                       memtable_key.data() + memtable_key.size(),
                       &internal_key_size);

    ASSERT_NE(internal_key_start, nullptr);
    EXPECT_EQ(internal_key_size, user_key.size() + 8);
    EXPECT_EQ(internal_key_start, lookup.internal_key().data());
    EXPECT_EQ(lookup.internal_key().size(), user_key.size() + 8);
    EXPECT_EQ(lookup.user_key().ToString(), user_key);
    EXPECT_EQ(DecodeFixed64(lookup.internal_key().data() + user_key.size()),
              PackSequenceAndType(sequence, kValueTypeForSeek));
}

TEST(LookupKeyTest, UsesMultiByteLengthPrefixAtInternalKeySizeBoundary) {
    // 120 字节 UserKey 加上 8 字节 tag 后恰为 128，varint 长度应扩展到 2 字节。
    const std::string user_key(120, 'k');
    LookupKey lookup(Slice(user_key), kMaxSequenceNumber);

    const Slice memtable_key = lookup.memtable_key();
    uint32_t internal_key_size = 0;
    const char* const internal_key_start =
        DecodeVarint32(memtable_key.data(),
                       memtable_key.data() + memtable_key.size(),
                       &internal_key_size);

    ASSERT_NE(internal_key_start, nullptr);
    EXPECT_EQ(internal_key_size, 128U);
    EXPECT_EQ(internal_key_start - memtable_key.data(), 2);
    EXPECT_EQ(lookup.user_key().ToString(), user_key);
    EXPECT_EQ(DecodeFixed64(lookup.internal_key().data() + user_key.size()),
              PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
}

TEST(LookupKeyTest, SupportsEmptyUserKey) {
    // 空 UserKey 仍有一个 8 字节 tag，并可作为合法的查找目标。
    LookupKey lookup(Slice(), 0);

    EXPECT_TRUE(lookup.user_key().empty());
    EXPECT_EQ(lookup.internal_key().size(), 8U);
    EXPECT_EQ(lookup.memtable_key().size(), 9U);
}

TEST(LookupKeyTest, IsNonCopyableAndSupportsLongKeys) {
    static_assert(!std::is_copy_constructible_v<LookupKey>);
    static_assert(!std::is_copy_assignable_v<LookupKey>);

    const std::string user_key(400, 'x');
    LookupKey lookup(Slice(user_key), 123);

    EXPECT_EQ(lookup.user_key().ToString(), user_key);
    EXPECT_EQ(lookup.internal_key().size(), user_key.size() + 8);
    EXPECT_EQ(lookup.memtable_key().size(),
              VarintLength(user_key.size() + 8) + user_key.size() + 8);
}

}  // namespace
}  // namespace LSMKV
