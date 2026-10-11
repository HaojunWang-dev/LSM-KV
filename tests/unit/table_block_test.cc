#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "coding.h"
#include "comparator.h"
#include "format.h"
#include "table_block.h"

namespace LSMKV {
namespace {

using Entry = std::pair<std::string, std::string>;

std::string WithRestarts(std::string entries, const std::vector<uint32_t>& restarts) {
  for (uint32_t offset : restarts) PutFixed32(&entries, offset);
  PutFixed32(&entries, static_cast<uint32_t>(restarts.size()));
  return entries;
}

// Test-only wire fixture builder, not a production BlockBuilder. Expected
// keys/values and seek destinations remain explicit in the individual tests.
std::string EncodeEntries(const std::vector<Entry>& entries, size_t interval) {
  std::string encoded;
  std::string previous;
  std::vector<uint32_t> restarts;
  for (size_t i = 0; i < entries.size(); ++i) {
    size_t shared = 0;
    if (i % interval == 0) {
      restarts.push_back(static_cast<uint32_t>(encoded.size()));
    } else {
      while (shared < previous.size() && shared < entries[i].first.size() &&
             previous[shared] == entries[i].first[shared]) ++shared;
    }
    PutVarint32(&encoded, static_cast<uint32_t>(shared));
    PutVarint32(&encoded, static_cast<uint32_t>(entries[i].first.size() - shared));
    PutVarint32(&encoded, static_cast<uint32_t>(entries[i].second.size()));
    encoded.append(entries[i].first.data() + shared, entries[i].first.size() - shared);
    encoded.append(entries[i].second);
    previous = entries[i].first;
  }
  if (restarts.empty()) restarts.push_back(0);
  return WithRestarts(std::move(encoded), restarts);
}

void ExpectEntry(const Iterator& iterator, const std::string& key,
                 const std::string& value) {
  ASSERT_TRUE(iterator.Valid()) << iterator.status().ToString();
  const std::string actual_key = iterator.key().ToString();
  const std::string actual_value = iterator.value().ToString();
  if (key.size() <= 256) {
    EXPECT_EQ(actual_key, key);
  } else {
    EXPECT_EQ(actual_key.size(), key.size());
    EXPECT_TRUE(actual_key == key) << "large key contents differ";
  }
  if (value.size() <= 256) {
    EXPECT_EQ(actual_value, value);
  } else {
    EXPECT_EQ(actual_value.size(), value.size());
    EXPECT_TRUE(actual_value == value) << "large value contents differ";
  }
  EXPECT_TRUE(iterator.status().ok());
}

TEST(TableBlockTest, NewIteratorStartsInvalidWithoutAnError) {
  const std::string bytes = EncodeEntries({{"a", "1"}}, 1);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, EmptyBlockWithOneRestartSupportsAllSeeks) {
  const std::string bytes = WithRestarts("", {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  iterator->SeekToLast();
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek("anything");
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, EmptyBlockWithNoRestartsReturnsEmptyIterator) {
  const std::string bytes = WithRestarts("", {});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  iterator->SeekToLast();
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek("anything");
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, SingleEntryCanBeReadFromEitherEnd) {
  // Literal entry [shared=0][non_shared=1][value_length=1][a][v].
  const std::string bytes = WithRestarts(std::string("\0\1\1av", 5), {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "v"));
  iterator->Next();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
  iterator->SeekToLast();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "v"));
}

TEST(TableBlockTest, PrevBeforeTheFirstEntryBecomesInvalid) {
  const std::string bytes = EncodeEntries({{"a", "1"}}, 1);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_TRUE(iterator->Valid());
  iterator->Prev();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, ForwardIterationReconstructsLiteralSharedPrefixes) {
  const std::string entries = std::string("\0\5\1apple1", 9) +
                              std::string("\2\5\1ricot2", 9) +
                              std::string("\0\6\1banana3", 10);
  const std::string bytes = WithRestarts(entries, {0, 18});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "apple", "1"));
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "apricot", "2"));
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "banana", "3"));
  iterator->Next();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, ForwardIterationCrossesSeveralRestartRegions) {
  const std::vector<Entry> entries = {{"ant", "1"}, {"ape", "2"}, {"bat", "3"},
                                     {"bee", "4"}, {"cat", "5"}, {"cow", "6"},
                                     {"dog", "7"}};
  const std::string bytes = EncodeEntries(entries, 2);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  for (const auto& entry : entries) {
    ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, entry.first, entry.second));
    iterator->Next();
  }
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, SeekToLastHandlesTwoEntriesInTheLastRegion) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"b", "2"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToLast();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "b", "2"));
}

TEST(TableBlockTest, BackwardIterationCrossesRestartBoundaries) {
  const std::vector<Entry> entries = {{"a", "1"}, {"b", "2"}, {"c", "3"},
                                     {"d", "4"}, {"e", "5"}};
  const std::string bytes = EncodeEntries(entries, 2);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToLast();
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, entry->first, entry->second));
    iterator->Prev();
  }
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, DirectionChangesDoNotSkipEntries) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"b", "2"}, {"c", "3"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_TRUE(iterator->Valid());
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "b", "2"));
  iterator->Prev();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "1"));
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "b", "2"));
}

TEST(TableBlockTest, SeekWithinOneRestartRegionReturnsTheLowerBound) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"c", "3"}, {"e", "5"}}, 8);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->Seek("");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "1"));
  iterator->Seek("b");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "c", "3"));
  iterator->Seek("c");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "c", "3"));
  iterator->Seek("d");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "e", "5"));
  iterator->Seek("z");
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(TableBlockTest, FreshSeekUsesTheCorrectRestartOffset) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"c", "3"}, {"e", "5"}}, 1);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->Seek("b");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "c", "3"));
}

TEST(TableBlockTest, ForwardSeekDoesNotSkipTheFirstEligibleRestart) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"c", "3"}, {"e", "5"}}, 1);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_TRUE(iterator->Valid());
  iterator->Seek("b");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "c", "3"));
}

TEST(TableBlockTest, BackwardSeekAcrossRestartsTerminatesAtTheLowerBound) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"c", "3"}, {"e", "5"}}, 1);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToLast();
  ASSERT_TRUE(iterator->Valid());
  iterator->Seek("b");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "c", "3"));
}

TEST(TableBlockTest, SeekingAfterEndCanRepositionToTheBeginning) {
  const std::string bytes = EncodeEntries({{"a", "1"}, {"c", "3"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_TRUE(iterator->Valid());
  iterator->Next();
  ASSERT_TRUE(iterator->Valid());
  iterator->Next();
  ASSERT_FALSE(iterator->Valid());
  iterator->Seek("a");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "1"));
}

TEST(TableBlockTest, SupportsEmptyAndBinaryKeysAndValues) {
  const std::vector<Entry> entries = {{"", ""}, {std::string("a\0b", 3),
                                               std::string("v\0x", 3)},
                                     {std::string("\xff", 1), ""}};
  const std::string bytes = EncodeEntries(entries, 8);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  for (const auto& entry : entries) {
    ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, entry.first, entry.second));
    iterator->Next();
  }
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek(Slice("a\0a", 3));
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, std::string("a\0b", 3),
                                     std::string("v\0x", 3)));
}

class TableBlockVarintTest : public testing::TestWithParam<size_t> {};

TEST_P(TableBlockVarintTest, DecodesMultibyteKeyValueAndSharedLengths) {
  const std::string first_key(GetParam(), 'a');
  const std::string second_key = first_key + 'b';
  const std::string value(GetParam(), 'v');
  const std::string bytes = EncodeEntries({{first_key, value}, {second_key, "2"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, first_key, value));
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, second_key, "2"));
  iterator->Next();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

INSTANTIATE_TEST_SUITE_P(VarintBoundaries, TableBlockVarintTest,
                         testing::Values(size_t{127}, size_t{128}, size_t{16384}));

class ReverseComparator final : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override { return b.Compare(a); }
  const char* Name() const override { return "test.ReverseBytewise"; }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

TEST(TableBlockTest, SeekUsesTheSuppliedComparatorRatherThanBytewiseOrder) {
  const ReverseComparator comparator;
  const std::string bytes = EncodeEntries({{"z", "1"}, {"m", "2"}, {"a", "3"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(&comparator));
  iterator->Seek("n");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "m", "2"));
  iterator->Seek("b");
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "3"));
}

TEST(TableBlockTest, InternalKeySeekRespectsDescendingSequenceAndTombstones) {
  const InternalKeyComparator comparator;
  const InternalKey newest("key", 9, ValueType::kValue);
  const InternalKey deletion("key", 7, ValueType::kDeletion);
  const InternalKey oldest("key", 3, ValueType::kValue);
  const std::string bytes = EncodeEntries({{newest.Encode().ToString(), "new"},
                                          {deletion.Encode().ToString(), ""},
                                          {oldest.Encode().ToString(), "old"}}, 4);
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(&comparator));
  const LookupKey target("key", 8);
  iterator->Seek(target.internal_key());
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, deletion.Encode().ToString(), ""));
  iterator->Next();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, oldest.Encode().ToString(), "old"));
}

TEST(TableBlockTest, BorrowedStorageCanBeReusedAfterBlockDestruction) {
  const std::string bytes = EncodeEntries({{"a", "1"}}, 1);
  for (int pass = 0; pass < 2; ++pass) {
    Block block({Slice(bytes), false, false});
    std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
    iterator->SeekToFirst();
    ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "1"));
  }
}

TEST(TableBlockTest, BlockTakesResponsibilityForOwnedContents) {
  const std::string bytes = EncodeEntries({{"a", "1"}}, 1);
  auto allocation = std::make_unique<char[]>(bytes.size());
  std::memcpy(allocation.get(), bytes.data(), bytes.size());
  BlockContents contents{Slice(allocation.get(), bytes.size()), true, true};
  Block block(contents);
  allocation.release();  // Block is now the sole owner; ASAN checks its cleanup.
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "1"));
}

TEST(TableBlockTest, RejectsBlocksShorterThanTheRestartCount) {
  for (size_t length = 0; length < 4; ++length) {
    const std::string bytes(length, '\0');
    Block block({Slice(bytes), false, false});
    std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
    EXPECT_FALSE(iterator->Valid());
    EXPECT_TRUE(iterator->status().IsCorruption());
  }
}

TEST(TableBlockTest, RejectsRestartCountLargerThanAvailableOffsets) {
  const std::string bytes = WithRestarts("", {0});
  std::string malformed = bytes;
  EncodeFixed32(&malformed[malformed.size() - 4], 2);
  Block block({Slice(malformed), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

class TableBlockMalformedEntryTest : public testing::TestWithParam<std::string> {};

TEST_P(TableBlockMalformedEntryTest, InvalidEntryReportsCorruption) {
  const std::string bytes = WithRestarts(GetParam(), {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

INSTANTIATE_TEST_SUITE_P(
    InvalidEntries, TableBlockMalformedEntryTest,
    testing::Values(std::string("\0", 1), std::string("\0\1", 2),
                    std::string("\0\5\1a", 4), std::string("\1\1\1av", 5),
                    std::string("\0\0\x80", 3)));

TEST(TableBlockTest, SharedPrefixCannotExceedThePreviousKeyLength) {
  const std::string entries = std::string("\0\1\1av", 5) +
                              std::string("\2\1\1bw", 5);
  const std::string bytes = WithRestarts(entries, {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "v"));
  iterator->Next();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

TEST(TableBlockTest, RestartOffsetInsideTheTrailerReportsCorruption) {
  const std::string entries("\0\1\1av", 5);
  const std::string bytes = WithRestarts(entries, {5});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

TEST(TableBlockTest, RejectsOverflowingKeyLengthVarint) {
  // 2^32 is not a valid uint32 length. Padding also prevents a broken
  // single-byte fast path from rejecting this fixture merely as truncated.
  const std::string entries = std::string("\0\x80\x80\x80\x80\x10\0", 7) +
                              std::string(512, 'x');
  const std::string bytes = WithRestarts(entries, {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

TEST(TableBlockTest, KeyAndValueLengthsCannotWrapTheirSum) {
  // UINT32_MAX + 1 must not wrap to zero and bypass the payload bounds check.
  const std::string entries = std::string("\0\xff\xff\xff\xff\x0f\1", 7) +
                              std::string(512, 'x');
  const std::string bytes = WithRestarts(entries, {0});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

class TableBlockRestartTest : public testing::TestWithParam<std::vector<uint32_t>> {};

TEST_P(TableBlockRestartTest, InvalidRestartTableReportsCorruption) {
  const std::string entries = std::string("\0\1\1av", 5) +
                              std::string("\0\1\1bw", 5);
  const std::string bytes = WithRestarts(entries, GetParam());
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

INSTANTIATE_TEST_SUITE_P(
    InvalidRestarts, TableBlockRestartTest,
    testing::Values(std::vector<uint32_t>{1}, std::vector<uint32_t>{0, 10},
                    std::vector<uint32_t>{0, 0}, std::vector<uint32_t>{0, 5, 4}));

TEST(TableBlockTest, RestartEntryCannotHaveASharedPrefix) {
  const std::string entries = std::string("\0\1\1av", 5) +
                              std::string("\1\1\1bw", 5);
  const std::string bytes = WithRestarts(entries, {0, 5});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  ASSERT_NO_FATAL_FAILURE(ExpectEntry(*iterator, "a", "v"));
  iterator->Next();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

TEST(TableBlockTest, RestartCannotPointInsideAnEntry) {
  const std::string entries = std::string("\0\3\1abcv", 7) +
                              std::string("\0\3\1xyzw", 7);
  const std::string bytes = WithRestarts(entries, {0, 4});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

class TableBlockDirectRestartTest : public testing::TestWithParam<bool> {};

TEST_P(TableBlockDirectRestartTest, CannotDecodeValueBytesAsARestartEntry) {
  // key "a" has value [0,1,1,'z','v'], which looks like a second entry.
  // The claimed restart at offset 4 is inside that value, not on a boundary.
  const std::string entries("\0\1\5a\0\1\1zv", 9);
  const std::string bytes = WithRestarts(entries, {0, 4});
  Block block({Slice(bytes), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  if (GetParam()) {
    iterator->SeekToLast();
  } else {
    iterator->Seek("zz");
  }
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}

INSTANTIATE_TEST_SUITE_P(DirectPositioning, TableBlockDirectRestartTest,
                         testing::Bool());

#if SIZE_MAX > UINT32_MAX
TEST(TableBlockTest, RejectsUnrepresentableBlockLengthBeforeReadingIt) {
  const char storage = 0;
  const size_t length = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1;
  // No bytes may be inspected: the offset representation cannot describe it.
  Block block({Slice(&storage, length), false, false});
  std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsCorruption());
}
#endif

TEST(TableBlockTest, SeekReturnsLowerBoundsAcrossDifferentRestartIntervals) {
  const std::vector<Entry> entries = {{"a", "1"}, {"c", "3"}, {"e", "5"},
                                     {"g", "7"}, {"i", "9"}, {"k", "11"}};
  const std::vector<Entry> lookups = {{"", "a"}, {"b", "c"}, {"e", "e"},
                                     {"j", "k"}, {"d", "e"}, {"a", "a"}};
  for (size_t interval : {size_t{1}, size_t{2}, size_t{3}, size_t{8}}) {
    SCOPED_TRACE(interval);
    const std::string bytes = EncodeEntries(entries, interval);
    Block block({Slice(bytes), false, false});
    std::unique_ptr<Iterator> iterator(block.NewIterator(BytewiseComparator()));
    for (const auto& lookup : lookups) {
      SCOPED_TRACE(lookup.first);
      iterator->Seek(lookup.first);
      ASSERT_TRUE(iterator->Valid()) << iterator->status().ToString();
      EXPECT_EQ(iterator->key().ToString(), lookup.second);
    }
    iterator->Seek("z");
    EXPECT_FALSE(iterator->Valid());
    EXPECT_TRUE(iterator->status().ok());
  }
}

}  // namespace
}  // namespace LSMKV
