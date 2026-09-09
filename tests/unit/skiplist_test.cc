#include <gtest/gtest.h>

#include <random>
#include <set>
#include <vector>

#include "arena.h"
#include "skiplist.h"

namespace LSMKV {
namespace {

struct IntComparator {
    int operator()(int left, int right) const {
        return (left > right) - (left < right);
    }
};

using IntSkipList = SkipList<int, IntComparator>;

std::vector<int> CollectKeys(const IntSkipList& list) {
    // Iterator 的输出是 SkipList 对外可观察的排序结果。
    std::vector<int> keys;
    IntSkipList::Iterator iterator(&list);

    for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
        keys.push_back(iterator.key());
    }

    return keys;
}

TEST(SkipListTest, EmptyListDoesNotContainKeysAndHasNoFirstElement) {
    // 空表既不能命中查询，也不应让迭代器指向任何节点。
    Arena arena;
    IntSkipList list(IntComparator{}, &arena);
    IntSkipList::Iterator iterator(&list);

    EXPECT_FALSE(list.Contains(0));
    EXPECT_FALSE(list.Contains(42));

    iterator.SeekToFirst();
    EXPECT_FALSE(iterator.Valid());

    iterator.Seek(10);
    EXPECT_FALSE(iterator.Valid());
}

TEST(SkipListTest, InsertsOutOfOrderAndTraversesInComparatorOrder) {
    // 插入顺序不能影响 level 0 的全序；Comparator 决定最终遍历顺序。
    Arena arena;
    IntSkipList list(IntComparator{}, &arena);

    for (int key : {40, 10, 30, 20}) {
        list.Insert(key);
    }

    EXPECT_TRUE(list.Contains(10));
    EXPECT_TRUE(list.Contains(20));
    EXPECT_TRUE(list.Contains(30));
    EXPECT_TRUE(list.Contains(40));
    EXPECT_FALSE(list.Contains(25));

    EXPECT_EQ(CollectKeys(list), (std::vector<int>{10, 20, 30, 40}));
}

TEST(SkipListTest, SeekFindsTheFirstKeyNotLessThanTarget) {
    // Seek 的语义是 lower_bound：返回第一个不小于 target 的键。
    Arena arena;
    IntSkipList list(IntComparator{}, &arena);

    for (int key : {10, 20, 30}) {
        list.Insert(key);
    }

    IntSkipList::Iterator iterator(&list);

    iterator.Seek(20);
    ASSERT_TRUE(iterator.Valid());
    EXPECT_EQ(iterator.key(), 20);

    iterator.Seek(15);
    ASSERT_TRUE(iterator.Valid());
    EXPECT_EQ(iterator.key(), 20);

    iterator.Seek(0);
    ASSERT_TRUE(iterator.Valid());
    EXPECT_EQ(iterator.key(), 10);

    iterator.Seek(31);
    EXPECT_FALSE(iterator.Valid());
}

TEST(SkipListTest, RandomUniqueInsertsMatchSetOracle) {
    // 用 std::set 作为测试 Oracle，验证大量随机插入后的查找与全量排序。
    Arena arena;
    IntSkipList list(IntComparator{}, &arena);
    std::mt19937 generator(0x5EED);
    std::uniform_int_distribution<int> distribution(1, 100000);
    std::set<int> expected;

    while (expected.size() < 1000) {
        const int key = distribution(generator);
        if (expected.insert(key).second) {
            list.Insert(key);
        }
    }

    for (int key : expected) {
        EXPECT_TRUE(list.Contains(key));
    }
    EXPECT_FALSE(list.Contains(-1));

    EXPECT_EQ(CollectKeys(list),
              (std::vector<int>(expected.begin(), expected.end())));
}

#ifndef NDEBUG
TEST(SkipListTest, DuplicateInsertViolatesTheDebugOnlyUniqueKeyContract) {
    // 当前实现不覆盖写入同一键：Debug 构建必须通过 assert 明确拒绝它。
    ASSERT_DEATH(
        {
            Arena arena;
            IntSkipList list(IntComparator{}, &arena);
            list.Insert(7);
            list.Insert(7);
        },
        "");
}
#endif

}  // namespace
}  // namespace LSMKV
