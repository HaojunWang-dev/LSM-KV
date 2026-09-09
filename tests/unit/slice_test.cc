#include <gtest/gtest.h>

#include <string>

#include "slice.h"

namespace LSMKV {
namespace {

TEST(SliceTest, DefaultSliceIsEmpty) {
    Slice slice;

    EXPECT_TRUE(slice.empty());
    EXPECT_EQ(slice.size(), 0U);
    EXPECT_EQ(slice.ToString(), "");
}

TEST(SliceTest, PreservesExplicitLengthIncludingEmbeddedNullBytes) {
    const std::string source("a\0b", 3);
    Slice slice(source.data(), source.size());

    EXPECT_FALSE(slice.empty());
    EXPECT_EQ(slice.size(), 3U);
    EXPECT_EQ(slice.ToString(), source);
}

TEST(SliceTest, CompareUsesBytewiseLexicographicOrder) {
    const Slice apple("apple", 5);
    const Slice apple_copy("apple", 5);
    const Slice apples("apples", 6);
    const Slice banana("banana", 6);

    EXPECT_EQ(apple.Compare(apple_copy), 0);
    EXPECT_LT(apple.Compare(apples), 0);
    EXPECT_GT(apples.Compare(apple), 0);
    EXPECT_LT(apple.Compare(banana), 0);
}

}  // namespace
}  // namespace LSMKV
