#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "format.h"
#include "comparator_test_helpers.h"

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

TEST(InternalKeyTest, ComparatorUsesCustomUserOrderThroughBaseInterface) {
    test::ReverseBytewiseComparator user_comparator;
    InternalKeyComparator comparator(&user_comparator);
    const Comparator& base = comparator;
    InternalKey apple(Slice("apple"), 30, ValueType::kValue);
    InternalKey banana_new(Slice("banana"), 20, ValueType::kValue);
    InternalKey banana_old(Slice("banana"), 10, ValueType::kValue);
    EXPECT_LT(base.Compare(banana_new.Encode(), apple.Encode()), 0);
    EXPECT_LT(base.Compare(banana_new.Encode(), banana_old.Encode()), 0);
    EXPECT_EQ(comparator.user_comparator(), &user_comparator);
}

TEST(InternalKeyTest, ComparatorOrdersTagsForEquivalentUserKeys) {
    test::AsciiCaseInsensitiveComparator user_comparator;
    InternalKeyComparator comparator(&user_comparator);
    InternalKey newer(Slice("APPLE"), 20, ValueType::kValue);
    InternalKey older(Slice("apple"), 10, ValueType::kValue);
    InternalKey same(Slice("ApPlE"), 20, ValueType::kValue);
    InternalKey deletion(Slice("apple"), 20, ValueType::kDeletion);
    EXPECT_LT(comparator.Compare(newer, older), 0);
    EXPECT_EQ(comparator.Compare(newer, same), 0);
    EXPECT_LT(comparator.Compare(newer, deletion), 0);
}

TEST(InternalKeyTest, SeparatorAppendsMaximumSeekTagToShortenedUserKey) {
    InternalKeyComparator comparator;
    InternalKey original(Slice("apple"), 20, ValueType::kValue);
    InternalKey limit(Slice("carrot"), 30, ValueType::kDeletion);
    std::string shortened = original.Encode().ToString();
    const Comparator& base = comparator;
    base.FindShortestSeparator(&shortened, limit.Encode());
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(shortened), &parsed));
    EXPECT_EQ(parsed.user_key.ToString(), "b");
    EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
    EXPECT_EQ(parsed.type, kValueTypeForSeek);
    EXPECT_LT(comparator.Compare(original.Encode(), Slice(shortened)), 0);
    EXPECT_LT(comparator.Compare(Slice(shortened), limit.Encode()), 0);
}

TEST(InternalKeyTest, ShortSuccessorAppendsMaximumSeekTag) {
    InternalKeyComparator comparator;
    InternalKey original(Slice("apple"), 20, ValueType::kDeletion);
    std::string shortened = original.Encode().ToString();
    comparator.FindShortSuccessor(&shortened);
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(shortened), &parsed));
    EXPECT_EQ(parsed.user_key.ToString(), "b");
    EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
    EXPECT_EQ(parsed.type, kValueTypeForSeek);
    EXPECT_LT(comparator.Compare(original.Encode(), Slice(shortened)), 0);
}

TEST(InternalKeyTest, ShortenersKeepOriginalTagsWhenUserKeyCannotBeShortened) {
    InternalKeyComparator comparator;
    InternalKey first(Slice("apple"), 20, ValueType::kValue);
    InternalKey last(Slice("apple"), 10, ValueType::kValue);
    std::string shortened = first.Encode().ToString();
    comparator.FindShortestSeparator(&shortened, last.Encode());
    EXPECT_EQ(shortened, first.Encode().ToString());

    InternalKey maximum(Slice(std::string(3, '\xff')), 5, ValueType::kDeletion);
    shortened = maximum.Encode().ToString();
    comparator.FindShortSuccessor(&shortened);
    EXPECT_EQ(shortened, maximum.Encode().ToString());

    test::ReverseBytewiseComparator reverse;
    InternalKeyComparator reversed(&reverse);
    shortened = last.Encode().ToString();
    reversed.FindShortSuccessor(&shortened);
    EXPECT_EQ(shortened, last.Encode().ToString());
}

TEST(InternalKeyTest, CustomShortenersUseLogicalRatherThanBytewiseOrder) {
    test::AsciiCaseInsensitiveShorteningComparator user_comparator;
    InternalKeyComparator comparator(&user_comparator);
    InternalKey original(Slice("apple"), 20, ValueType::kDeletion);
    InternalKey limit(Slice("carrot"), 30, ValueType::kValue);
    // B 在字节序中小于 apple，却在本比较器中严格大于 apple。
    EXPECT_LT(BytewiseComparator()->Compare(Slice("B"), Slice("apple")), 0);
    for (bool separator : {true, false}) {
        std::string shortened = original.Encode().ToString();
        if (separator) {
            comparator.FindShortestSeparator(&shortened, limit.Encode());
        } else {
            comparator.FindShortSuccessor(&shortened);
        }
        ParsedInternalKey parsed;
        ASSERT_TRUE(ParseInternalKey(Slice(shortened), &parsed));
        EXPECT_EQ(parsed.user_key.ToString(), "B");
        EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
        EXPECT_EQ(parsed.type, kValueTypeForSeek);
        EXPECT_LT(comparator.Compare(original.Encode(), Slice(shortened)), 0);
        EXPECT_LT(comparator.Compare(Slice(shortened), limit.Encode()), 0);
    }
}

TEST(InternalKeyTest, EquivalentShorterUserKeysMustNotChangeOriginalTag) {
    test::TrailingPaddingComparator user_comparator;
    InternalKeyComparator comparator(&user_comparator);
    InternalKey original(Slice("apple###"), 20, ValueType::kDeletion);
    InternalKey limit(Slice("carrot"), 30, ValueType::kValue);
    ASSERT_EQ(user_comparator.Compare(Slice("apple###"), Slice("apple")), 0);
    std::string key = original.Encode().ToString();
    comparator.FindShortestSeparator(&key, limit.Encode());
    EXPECT_EQ(key, original.Encode().ToString());
    comparator.FindShortSuccessor(&key);
    EXPECT_EQ(key, original.Encode().ToString());
}

TEST(InternalKeyTest, AppendsAndParsesParsedInternalKey) {
    const std::string user_key("a\0b", 3);
    const ParsedInternalKey expected(Slice(user_key), 17, ValueType::kValue);

    std::string encoded;
    AppendInternalKey(&encoded, expected);

    EXPECT_EQ(encoded.size(), InternalKeyEncodingLength(expected));

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(parsed.user_key.ToString(), user_key);
    EXPECT_EQ(parsed.sequence, 17U);
    EXPECT_EQ(parsed.type, ValueType::kValue);
}

TEST(InternalKeyTest, RejectsShortAndUnknownInternalKeys) {
    ParsedInternalKey parsed;
    EXPECT_FALSE(ParseInternalKey(Slice("short"), &parsed));

    std::string encoded("user_key", 8);
    encoded.append(8, '\0');
    encoded[encoded.size() - 8] = static_cast<char>(2);
    EXPECT_FALSE(ParseInternalKey(Slice(encoded), &parsed));
}

TEST(InternalKeyTest, OwningInternalKeyCanDecodeResetAndReencode) {
    const std::string user_key("owned\0key", 9);
    InternalKey key(Slice(user_key), 99, ValueType::kDeletion);
    EXPECT_EQ(key.Encode().ToString(),
              MakeInternalKey(user_key,
                              PackSequenceAndType(99, ValueType::kDeletion)));
    EXPECT_EQ(key.user_key().ToString(), user_key);

    InternalKey decoded;
    ASSERT_TRUE(decoded.DecodeFrom(key.Encode()));
    EXPECT_EQ(decoded.Encode().ToString(), key.Encode().ToString());

    ParsedInternalKey replacement(Slice("new"), 100, ValueType::kValue);
    decoded.SetFrom(replacement);
    EXPECT_EQ(decoded.user_key().ToString(), "new");
    decoded.Clear();
    EXPECT_TRUE(decoded.Encode().empty());
}

}  // namespace
}  // namespace LSMKV
