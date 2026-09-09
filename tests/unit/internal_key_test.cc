#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "internal_key.h"

namespace LSMKV {
namespace {

std::string MakeInternalKey(const std::string& user_key, uint64_t tag) {
    std::string encoded = user_key;

    for (int index = 0; index < 8; ++index) {
        encoded.push_back(static_cast<char>(tag & 0xff));
        tag >>= 8;
    }

    return encoded;
}

TEST(InternalKeyTest, PackSequenceAndTypeUsesHighBitsForSequence) {
    constexpr SequenceNumber sequence = 0x00ABCD12345678ULL;

    const uint64_t value_tag = PackSequenceAndType(sequence, ValueType::kValue);
    const uint64_t deletion_tag =
        PackSequenceAndType(sequence, ValueType::kDeletion);

    EXPECT_EQ(value_tag >> 8, sequence);
    EXPECT_EQ(value_tag & 0xff, static_cast<uint64_t>(ValueType::kValue));
    EXPECT_EQ(deletion_tag >> 8, sequence);
    EXPECT_EQ(deletion_tag & 0xff, static_cast<uint64_t>(ValueType::kDeletion));
}

TEST(InternalKeyTest, ExtractUserKeyExcludesTheEightByteTag) {
    const std::string encoded = MakeInternalKey(
        std::string("a\0b", 3),
        PackSequenceAndType(7, ValueType::kValue));

    const Slice user_key = ExtractUserKey(Slice(encoded));

    EXPECT_EQ(user_key.ToString(), std::string("a\0b", 3));
}

TEST(InternalKeyTest, ComparatorSortsUserKeysAscendingThenTagsDescending) {
    InternalKeyComparator comparator;
    const std::string apple_old = MakeInternalKey(
        "apple", PackSequenceAndType(10, ValueType::kValue));
    const std::string apple_new = MakeInternalKey(
        "apple", PackSequenceAndType(20, ValueType::kValue));
    const std::string apple_deletion = MakeInternalKey(
        "apple", PackSequenceAndType(20, ValueType::kDeletion));
    const std::string banana = MakeInternalKey(
        "banana", PackSequenceAndType(1, ValueType::kValue));

    EXPECT_LT(comparator.Compare(Slice(apple_new), Slice(apple_old)), 0);
    EXPECT_LT(comparator.Compare(Slice(apple_new), Slice(apple_deletion)), 0);
    EXPECT_LT(comparator.Compare(Slice(apple_old), Slice(banana)), 0);
    EXPECT_EQ(comparator.Compare(Slice(apple_new), Slice(apple_new)), 0);
}

}  // namespace
}  // namespace LSMKV
