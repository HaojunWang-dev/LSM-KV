#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "coding.h"
#include "format.h"
#include "memtable.h"
#include "comparator_test_helpers.h"

namespace LSMKV {
namespace {

Slice StringSlice(const std::string& value) {
    // 测试中的字符串在断言结束前保持存活，Slice 只借用其字节范围。
    return Slice(value.data(), value.size());
}

SequenceNumber SequenceFromInternalKey(const Slice& internal_key) {
    // InternalKey 的最后 8 字节是 sequence << 8 | ValueType。
    const uint64_t tag =
        DecodeFixed64(internal_key.data() + internal_key.size() - 8);
    return tag >> 8;
}

TEST(MemTableTest, IteratorSeekFindsNewestVersionVisibleToSnapshot) {
    // snapshot=25 必须跳过 seq=30，并定位同一 UserKey 的 seq=20。
    MemTable table;
    const std::string key = "apple";
    const std::string old_value = "v10";
    const std::string visible_value = "v20";
    const std::string future_value = "v30";

    table.Add(10, ValueType::kValue, StringSlice(key), StringSlice(old_value));
    table.Add(20, ValueType::kValue, StringSlice(key), StringSlice(visible_value));
    table.Add(30, ValueType::kValue, StringSlice(key), StringSlice(future_value));

    LookupKey lookup(StringSlice(key), 25);
    MemTable::Iterator iterator(&table);
    iterator.Seek(lookup.internal_key());

    ASSERT_TRUE(iterator.Valid());
    EXPECT_EQ(ExtractUserKey(iterator.key()).ToString(), key);
    EXPECT_EQ(SequenceFromInternalKey(iterator.key()), 20U);
    EXPECT_EQ(iterator.value().ToString(), visible_value);
}

TEST(MemTableTest, GetReturnsValueAtTheRequestedSnapshot) {
    // Get 复用 Iterator::Seek 后，应返回 snapshot 可见版本的真实 value。
    MemTable table;
    const std::string key = "apple";
    const std::string old_value = "v10";
    const std::string visible_value = "v20";
    const std::string future_value = "v30";

    table.Add(10, ValueType::kValue, StringSlice(key), StringSlice(old_value));
    table.Add(20, ValueType::kValue, StringSlice(key), StringSlice(visible_value));
    table.Add(30, ValueType::kValue, StringSlice(key), StringSlice(future_value));

    std::string result;
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 25), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, visible_value);
}

TEST(MemTableTest, GetDistinguishesTombstoneFromMissingKey) {
    // tombstone 阻止下层返回旧值；不存在的 UserKey 才属于真正的未命中。
    MemTable table;
    const std::string key = "apple";
    const std::string value = "v10";
    const std::string missing = "missing";

    table.Add(10, ValueType::kValue, StringSlice(key), StringSlice(value));
    table.Add(20, ValueType::kDeletion, StringSlice(key), Slice());

    std::string result = "unchanged";
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 25), &result),
              MemTable::GetResult::kDeleted);
    EXPECT_EQ(result, "unchanged");
    EXPECT_EQ(table.Get(LookupKey(StringSlice(missing), 25), &result),
              MemTable::GetResult::kNotFound);
}

TEST(MemTableTest, SupportsEmptyUserKeyAndEmptyValue) {
    // 空 key 与空 value 都应按普通版本写入，不能与缺失 entry 混淆。
    MemTable table;
    std::string result = "not-empty";

    table.Add(1, ValueType::kValue, Slice(), Slice());

    EXPECT_EQ(table.Get(LookupKey(Slice(), 1), &result),
              MemTable::GetResult::kFound);
    EXPECT_TRUE(result.empty());
}

TEST(MemTableTest, GetsMultipleUserKeysIndependently) {
    // 同一张 MemTable 中的不同 UserKey 必须由排序和查找边界正确隔离。
    MemTable table;
    const std::string apple = "apple";
    const std::string banana = "banana";
    const std::string missing = "carrot";
    const std::string red = "red";
    const std::string yellow = "yellow";
    std::string result;

    table.Add(10, ValueType::kValue, StringSlice(apple), StringSlice(red));
    table.Add(10, ValueType::kValue, StringSlice(banana), StringSlice(yellow));

    EXPECT_EQ(table.Get(LookupKey(StringSlice(apple), 10), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, red);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(banana), 10), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, yellow);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(missing), 10), &result),
              MemTable::GetResult::kNotFound);
}

TEST(MemTableTest, GetSelectsTheCorrectVersionForEverySnapshotRange) {
    // 同一 UserKey 的版本按 sequence 降序排列，tombstone 对更晚 snapshot 可见。
    MemTable table;
    const std::string key = "apple";
    const std::string first = "v10";
    const std::string second = "v20";
    std::string result;

    table.Add(10, ValueType::kValue, StringSlice(key), StringSlice(first));
    table.Add(20, ValueType::kValue, StringSlice(key), StringSlice(second));
    table.Add(30, ValueType::kDeletion, StringSlice(key), Slice());

    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 5), &result),
              MemTable::GetResult::kNotFound);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 15), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, first);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 25), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, second);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 35), &result),
              MemTable::GetResult::kDeleted);
}

TEST(MemTableTest, PutAfterDeletionRestoresTheKeyForLaterSnapshots) {
    // 新 value 覆盖 tombstone 的可见性，但较早 snapshot 仍必须看到删除。
    MemTable table;
    const std::string key = "apple";
    const std::string old_value = "v10";
    const std::string restored_value = "v30";
    std::string result;

    table.Add(10, ValueType::kValue, StringSlice(key), StringSlice(old_value));
    table.Add(20, ValueType::kDeletion, StringSlice(key), Slice());
    table.Add(30, ValueType::kValue, StringSlice(key), StringSlice(restored_value));

    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 25), &result),
              MemTable::GetResult::kDeleted);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 35), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, restored_value);
}

TEST(MemTableTest, IteratorTraversesEveryVersionInInternalKeyOrder) {
    // 扫描不会折叠版本：同一 UserKey 的较新 sequence 必须先于较旧版本出现。
    MemTable table;
    const std::string apple = "apple";
    const std::string banana = "banana";
    const std::string value = "value";

    table.Add(10, ValueType::kValue, StringSlice(apple), StringSlice(value));
    table.Add(20, ValueType::kValue, StringSlice(apple), StringSlice(value));
    table.Add(5, ValueType::kValue, StringSlice(banana), StringSlice(value));

    MemTable::Iterator iterator(&table);
    std::vector<std::string> entries;
    for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
        entries.push_back(ExtractUserKey(iterator.key()).ToString() + "@" +
                          std::to_string(SequenceFromInternalKey(iterator.key())));
    }

    EXPECT_EQ(entries,
              (std::vector<std::string>{"apple@20", "apple@10", "banana@5"}));
}

TEST(MemTableTest, CustomComparatorControlsIterationAndSeek) {
    test::ReverseBytewiseComparator comparator;
    MemTable table(&comparator);
    table.Add(10, ValueType::kValue, Slice("apple"), Slice("a10"));
    table.Add(20, ValueType::kValue, Slice("banana"), Slice("b20"));
    table.Add(10, ValueType::kValue, Slice("banana"), Slice("b10"));
    MemTable::Iterator iterator(&table);
    std::vector<std::string> keys;
    for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
        keys.push_back(ExtractUserKey(iterator.key()).ToString() + "@" +
                       std::to_string(SequenceFromInternalKey(iterator.key())));
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"banana@20", "banana@10", "apple@10"}));

    iterator.Seek(LookupKey(Slice("banana"), 15).internal_key());
    ASSERT_TRUE(iterator.Valid());
    EXPECT_EQ(iterator.value().ToString(), "b10");
    std::string value = "unchanged";
    EXPECT_EQ(table.Get(LookupKey(Slice("avocado"), 30), &value),
              MemTable::GetResult::kNotFound);
    EXPECT_EQ(value, "unchanged");
}

TEST(MemTableTest, EquivalentUserKeysShareSnapshotAndTombstoneVisibility) {
    test::AsciiCaseInsensitiveComparator comparator;
    MemTable table(&comparator);
    table.Add(10, ValueType::kValue, Slice("Apple"), Slice("old"));
    table.Add(20, ValueType::kDeletion, Slice("APPLE"), Slice());
    table.Add(30, ValueType::kValue, Slice("apple"), Slice("new"));
    std::string value;
    EXPECT_EQ(table.Get(LookupKey(Slice("aPpLe"), 5), &value),
              MemTable::GetResult::kNotFound);
    EXPECT_EQ(table.Get(LookupKey(Slice("aPpLe"), 15), &value),
              MemTable::GetResult::kFound);
    EXPECT_EQ(value, "old");
    value = "unchanged";
    EXPECT_EQ(table.Get(LookupKey(Slice("aPpLe"), 25), &value),
              MemTable::GetResult::kDeleted);
    EXPECT_EQ(value, "unchanged");
    EXPECT_EQ(table.Get(LookupKey(Slice("aPpLe"), 35), &value),
              MemTable::GetResult::kFound);
    EXPECT_EQ(value, "new");
}

TEST(MemTableTest, CustomComparatorKeepsBinaryKeysAndMissingKeysDistinct) {
    test::AsciiCaseInsensitiveComparator comparator;
    MemTable table(&comparator);
    table.Add(1, ValueType::kValue, Slice("A\0B", 3), Slice("binary"));
    table.Add(1, ValueType::kValue, Slice(), Slice("empty"));
    std::string value;
    EXPECT_EQ(table.Get(LookupKey(Slice("a\0b", 3), 1), &value),
              MemTable::GetResult::kFound);
    EXPECT_EQ(value, "binary");
    EXPECT_EQ(table.Get(LookupKey(Slice(), 1), &value), MemTable::GetResult::kFound);
    EXPECT_EQ(value, "empty");
    value = "unchanged";
    EXPECT_EQ(table.Get(LookupKey(Slice("a", 1), 1), &value),
              MemTable::GetResult::kNotFound);
    EXPECT_EQ(value, "unchanged");
}

}  // namespace
}  // namespace LSMKV
