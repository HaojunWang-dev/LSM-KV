#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "comparator.h"

namespace LSMKV {
namespace {

TEST(ComparatorTest, BytewiseOrdersEmptyPrefixAndBinaryKeys) {
  const Comparator* comparator = BytewiseComparator();
  const std::vector<std::string> ordered = {
      "", std::string("\0", 1), std::string("\0a", 2), "a",
      std::string("a\0", 2), "aa", "ab", std::string("\x80", 1),
      std::string("\xff", 1)};
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    for (std::size_t j = 0; j < ordered.size(); ++j) {
      const int result = comparator->Compare(Slice(ordered[i]), Slice(ordered[j]));
      if (i < j) {
        EXPECT_LT(result, 0);
      } else if (i == j) {
        EXPECT_EQ(result, 0);
      } else {
        EXPECT_GT(result, 0);
      }
    }
  }
}

TEST(ComparatorTest, DefaultComparatorHasStableIdentity) {
  const Comparator* first = BytewiseComparator();
  EXPECT_EQ(first, BytewiseComparator());
  EXPECT_NE(std::string(first->Name()), "");
  EXPECT_STREQ(first->Name(), BytewiseComparator()->Name());
}

TEST(ComparatorTest, SeparatorShortensWhileKeepingBothBounds) {
  std::string start = "apple";
  BytewiseComparator()->FindShortestSeparator(&start, Slice("carrot"));
  EXPECT_EQ(start, "b");
  EXPECT_LE(BytewiseComparator()->Compare(Slice("apple"), Slice(start)), 0);
  EXPECT_LT(BytewiseComparator()->Compare(Slice(start), Slice("carrot")), 0);

  start = std::string("a\0tail", 6);
  const std::string limit("a\x02tail", 6);
  BytewiseComparator()->FindShortestSeparator(&start, Slice(limit));
  EXPECT_EQ(start, std::string("a\x01", 2));
}

TEST(ComparatorTest, SeparatorHandlesPrefixesAdjacentAndMaximumBytes) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"", "abc"}, {"abc", "abcd"}, {"abcd", "abce"},
      {"same", "same"}, {std::string("a\xffz", 3), "b"}};
  for (const auto& entry : cases) {
    std::string start = entry.first;
    BytewiseComparator()->FindShortestSeparator(&start, Slice(entry.second));
    EXPECT_EQ(start, entry.first);
  }
}

TEST(ComparatorTest, SuccessorShortensBinaryKeysWithoutWrapping) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"apple", "b"}, {std::string("\0tail", 5), std::string("\x01", 1)},
      {std::string("\xff\x7ftail", 6), std::string("\xff\x80", 2)},
      {std::string(3, '\xff'), std::string(3, '\xff')}, {"", ""}};
  for (const auto& entry : cases) {
    std::string key = entry.first;
    BytewiseComparator()->FindShortSuccessor(&key);
    EXPECT_EQ(key, entry.second);
    EXPECT_LE(BytewiseComparator()->Compare(Slice(entry.first), Slice(key)), 0);
  }
}

TEST(ComparatorTest, SeparatorPreservesRangeForBinaryCorpus) {
  const std::vector<std::string> ordered = {
      "", std::string("\0", 1), std::string("\0x", 2), "a", "abc", "abd",
      "abz", "carrot", std::string("\xff", 1), std::string("\xff\xff", 2)};
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    for (std::size_t j = i + 1; j < ordered.size(); ++j) {
      std::string shortened = ordered[i];
      BytewiseComparator()->FindShortestSeparator(&shortened, Slice(ordered[j]));
      EXPECT_LE(BytewiseComparator()->Compare(Slice(ordered[i]), Slice(shortened)), 0);
      EXPECT_LT(BytewiseComparator()->Compare(Slice(shortened), Slice(ordered[j])), 0);
    }
  }
}

}  // namespace
}  // namespace LSMKV
