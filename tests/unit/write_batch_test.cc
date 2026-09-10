#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coding.h"
#include "lookup_key.h"
#include "memtable.h"
#include "write_batch.h"
#include "write_batch_internal.h"

namespace LSMKV {
namespace {

Slice StringSlice(const std::string& value) {
    return Slice(value.data(), value.size());
}

struct Operation {
    ValueType type;
    std::string key;
    std::string value;
};

class RecordingHandler : public WriteBatch::Handler {
public:
    void Put(const Slice& key, const Slice& value) override {
        operations.push_back({ValueType::kValue, key.ToString(), value.ToString()});
    }

    void Delete(const Slice& key) override {
        operations.push_back({ValueType::kDeletion, key.ToString(), ""});
    }

    std::vector<Operation> operations;
};

TEST(WriteBatchTest, EncodesOperationsAndIteratesInInsertionOrder) {
    // Batch 的 sequence/count 位于头部；Iterate 必须保留 Put/Delete 的添加顺序。
    WriteBatch batch;
    const std::string binary_key("a\0b", 3);
    const std::string first_value = "v1";
    const std::string second_key = "second";
    const std::string second_value = "v2";

    WriteBatchInternal::SetSequence(&batch, 100);
    batch.Put(StringSlice(binary_key), StringSlice(first_value));
    batch.Delete(StringSlice(second_key));
    batch.Put(StringSlice(second_key), StringSlice(second_value));

    EXPECT_EQ(WriteBatchInternal::Sequence(&batch), 100U);
    EXPECT_EQ(WriteBatchInternal::Count(&batch), 3);
    EXPECT_GT(batch.ApproximateSize(), 12U);

    RecordingHandler handler;
    ASSERT_TRUE(batch.Iterate(&handler).ok());
    ASSERT_EQ(handler.operations.size(), 3U);
    EXPECT_EQ(handler.operations[0].type, ValueType::kValue);
    EXPECT_EQ(handler.operations[0].key, binary_key);
    EXPECT_EQ(handler.operations[0].value, first_value);
    EXPECT_EQ(handler.operations[1].type, ValueType::kDeletion);
    EXPECT_EQ(handler.operations[1].key, second_key);
    EXPECT_EQ(handler.operations[2].type, ValueType::kValue);
    EXPECT_EQ(handler.operations[2].value, second_value);
}

TEST(WriteBatchTest, AppendKeepsDestinationSequenceAndClearResetsHeader) {
    // 拼接只合并操作与 count，目标 batch 的 sequence 由写路径独立分配。
    WriteBatch destination;
    WriteBatch source;
    const std::string key = "key";
    const std::string value = "value";

    WriteBatchInternal::SetSequence(&destination, 10);
    destination.Put(StringSlice(key), StringSlice(value));
    WriteBatchInternal::SetSequence(&source, 100);
    source.Delete(StringSlice(key));
    source.Put(StringSlice(key), StringSlice(value));

    destination.Append(source);

    EXPECT_EQ(WriteBatchInternal::Sequence(&destination), 10U);
    EXPECT_EQ(WriteBatchInternal::Count(&destination), 3);

    destination.Clear();
    EXPECT_EQ(WriteBatchInternal::Count(&destination), 0);
    EXPECT_EQ(WriteBatchInternal::Sequence(&destination), 0U);
    EXPECT_EQ(destination.ApproximateSize(), 12U);
}

TEST(WriteBatchTest, InsertIntoAppliesConsecutiveSequencesToMemTable) {
    // Batch 内第 n 条操作必须使用 batch_sequence + n，保证 snapshot 语义正确。
    WriteBatch batch;
    MemTable table;
    const std::string key = "apple";
    const std::string first_value = "v10";
    const std::string final_value = "v12";
    std::string result;

    WriteBatchInternal::SetSequence(&batch, 10);
    batch.Put(StringSlice(key), StringSlice(first_value));
    batch.Delete(StringSlice(key));
    batch.Put(StringSlice(key), StringSlice(final_value));

    ASSERT_TRUE(WriteBatchInternal::InsertInto(&batch, &table).ok());
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 10), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, first_value);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 11), &result),
              MemTable::GetResult::kDeleted);
    EXPECT_EQ(table.Get(LookupKey(StringSlice(key), 12), &result),
              MemTable::GetResult::kFound);
    EXPECT_EQ(result, final_value);
}

TEST(WriteBatchTest, IterateRejectsUnknownTagAndIncorrectCount) {
    // WAL 恢复可能遇到损坏 rep_；Iterate 必须返回 Corruption，而非调用 Handler。
    WriteBatch batch;
    const std::string key = "key";
    const std::string value = "value";
    batch.Put(StringSlice(key), StringSlice(value));

    std::string contents = WriteBatchInternal::Contents(&batch).ToString();
    contents[12] = static_cast<char>(0x7f);
    WriteBatchInternal::SetContents(&batch, Slice(contents));

    RecordingHandler handler;
    EXPECT_TRUE(batch.Iterate(&handler).IsCorruption());

    WriteBatch valid_batch;
    valid_batch.Put(StringSlice(key), StringSlice(value));
    contents = WriteBatchInternal::Contents(&valid_batch).ToString();
    EncodeFixed32(contents.data() + 8, 2);
    WriteBatchInternal::SetContents(&valid_batch, Slice(contents));
    EXPECT_TRUE(valid_batch.Iterate(&handler).IsCorruption());
}

}  // namespace
}  // namespace LSMKV
