#include <cstdint>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "coding.h"
#include "crc32c.h"
#include "env.h"
#include "log_writer.h"

namespace LSMKV {
namespace log {
namespace {

// Writer 的公开接口必须允许 DB 写路径传入一个仍由调用方持有的文件对象，
// 并可从空文件或已存在的 WAL 末尾开始写入。
TEST(LogWriterTest, ExposesLevelDBStyleConstructionAndAppendApi) {
  static_assert(std::is_constructible_v<Writer, WritableFile*>);
  static_assert(
      std::is_constructible_v<Writer, WritableFile*, std::uint64_t>);
  static_assert(std::is_same_v<decltype(&Writer::AddRecord),
                               Status (Writer::*)(const Slice&)>);
}

class StringWritableFile final : public WritableFile {
 public:
  explicit StringWritableFile(std::string initial_contents = {})
      : contents_(std::move(initial_contents)) {}

  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }

  Status Close() override { return Status::OK(); }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }

  const std::string& contents() const { return contents_; }

 private:
  std::string contents_;
};

class FailingWritableFile final : public WritableFile {
 public:
  explicit FailingWritableFile(int failing_append_call = 0,
                               bool fail_flush = false)
      : failing_append_call_(failing_append_call), fail_flush_(fail_flush) {}

  Status Append(const Slice&) override {
    ++append_calls_;
    return append_calls_ == failing_append_call_
               ? Status::IOError(Slice("injected append failure"))
               : Status::OK();
  }

  Status Close() override { return Status::OK(); }
  Status Flush() override {
    return fail_flush_ ? Status::IOError(Slice("injected flush failure"))
                       : Status::OK();
  }
  Status Sync() override { return Status::OK(); }

 private:
  int failing_append_call_;
  bool fail_flush_;
  int append_calls_ = 0;
};

// 验证一个物理 WAL record 的完整磁盘布局，而非只验证 Writer 调用成功。
void ExpectPhysicalRecord(const std::string& data, size_t offset,
                          RecordType expected_type,
                          const std::string& expected_payload) {
  ASSERT_GE(data.size(), offset + kHeaderSize + expected_payload.size());

  const char type = static_cast<char>(expected_type);
  const std::uint32_t expected_crc = crc32c::Mask(crc32c::Extend(
      crc32c::Value(&type, 1), expected_payload.data(),
      expected_payload.size()));
  const std::uint16_t encoded_length =
      static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset + 4])) |
      (static_cast<std::uint16_t>(
           static_cast<unsigned char>(data[offset + 5]))
       << 8U);

  EXPECT_EQ(DecodeFixed32(data.data() + offset), expected_crc);
  EXPECT_EQ(encoded_length, expected_payload.size());
  EXPECT_EQ(static_cast<unsigned char>(data[offset + 6]),
            static_cast<unsigned char>(type));
  EXPECT_EQ(data.compare(offset + kHeaderSize, expected_payload.size(),
                         expected_payload),
            0);
}

// WAL header 保存的是经过 Mask() 的 CRC，避免校验和本身被误识别为 record 数据。
TEST(LogWriterTest, StoresMaskedChecksumInPhysicalRecordHeader) {
  StringWritableFile destination;
  Writer writer(&destination);

  ASSERT_TRUE(writer.AddRecord(Slice("x")).ok());
  ASSERT_EQ(destination.contents().size(), kHeaderSize + 1U);

  const char type = static_cast<char>(RecordType::kFullType);
  const std::uint32_t expected_crc = crc32c::Mask(
      crc32c::Extend(crc32c::Value(&type, 1), "x", 1));
  EXPECT_EQ(DecodeFixed32(destination.contents().data()), expected_crc);
}

// 即使逻辑记录为空，也必须写入一个可被 WAL Reader 识别的零长度 FULL record。
TEST(LogWriterTest, WritesAnEmptyLogicalRecordAsZeroLengthFullRecord) {
  StringWritableFile destination;
  Writer writer(&destination);

  ASSERT_TRUE(writer.AddRecord(Slice()).ok());
  ASSERT_EQ(destination.contents().size(), kHeaderSize);
  ExpectPhysicalRecord(destination.contents(), 0, RecordType::kFullType, "");
}

// header 加 payload 恰好填满一个 block 时，不应产生额外 fragment 或 padding。
TEST(LogWriterTest, FillsOneBlockWithExactlyOneFullRecord) {
  const std::string payload(kBlockSize - kHeaderSize, 'a');
  StringWritableFile destination;
  Writer writer(&destination);

  ASSERT_TRUE(writer.AddRecord(Slice(payload)).ok());
  ASSERT_EQ(destination.contents().size(), kBlockSize);
  ExpectPhysicalRecord(destination.contents(), 0, RecordType::kFullType,
                       payload);
}

// block 尾部不足 header 时，Writer 必须以零填充 trailer 并从下一 block 写记录。
TEST(LogWriterTest, PadsTrailerSmallerThanHeaderBeforeWritingNextRecord) {
  StringWritableFile destination(std::string(kBlockSize - 3, 'p'));
  Writer writer(&destination, kBlockSize - 3);

  ASSERT_TRUE(writer.AddRecord(Slice("x")).ok());
  ASSERT_EQ(destination.contents().size(), kBlockSize + kHeaderSize + 1U);
  EXPECT_EQ(destination.contents().substr(kBlockSize - 3, 3),
            std::string(3, '\0'));
  ExpectPhysicalRecord(destination.contents(), kBlockSize,
                       RecordType::kFullType, "x");
}

// 尾部恰好能容纳 header 时，第一个 fragment 必须是长度为 0 的 FIRST，
// 后续 payload 则从下一 block 以 LAST 写入。
TEST(LogWriterTest, SplitsRecordWhenExactlyOneHeaderRemainsInBlock) {
  const size_t initial_length = kBlockSize - kHeaderSize;
  StringWritableFile destination(std::string(initial_length, 'p'));
  Writer writer(&destination, initial_length);

  ASSERT_TRUE(writer.AddRecord(Slice("x")).ok());
  ASSERT_EQ(destination.contents().size(),
            kBlockSize + kHeaderSize + 1U);
  ExpectPhysicalRecord(destination.contents(), initial_length,
                       RecordType::kFirstType, "");
  ExpectPhysicalRecord(destination.contents(), kBlockSize,
                       RecordType::kLastType, "x");
}

// 超过两个 block payload 容量的逻辑记录必须依次形成 FIRST、MIDDLE、LAST，
// 每一个物理 record 都完整落在自己的 block 中。
TEST(LogWriterTest, FragmentsLargeRecordAcrossBlocksInOrder) {
  const size_t fragment_capacity = kBlockSize - kHeaderSize;
  const std::string payload(fragment_capacity * 2 + 11, 'v');
  StringWritableFile destination;
  Writer writer(&destination);

  ASSERT_TRUE(writer.AddRecord(Slice(payload)).ok());
  ASSERT_EQ(destination.contents().size(),
            kBlockSize * 2 + kHeaderSize + 11U);
  ExpectPhysicalRecord(destination.contents(), 0, RecordType::kFirstType,
                       payload.substr(0, fragment_capacity));
  ExpectPhysicalRecord(destination.contents(), kBlockSize,
                       RecordType::kMiddleType,
                       payload.substr(fragment_capacity, fragment_capacity));
  ExpectPhysicalRecord(destination.contents(), kBlockSize * 2,
                       RecordType::kLastType,
                       payload.substr(fragment_capacity * 2));
}

// 第一段 Append 写的是物理 header；该写入失败时不能继续写 payload 或报告成功。
TEST(LogWriterTest, PropagatesHeaderAppendFailure) {
  FailingWritableFile destination(/*failing_append_call=*/1);
  Writer writer(&destination);

  EXPECT_TRUE(writer.AddRecord(Slice("x")).IsIOError());
}

// 第二段 Append 写的是 payload；其失败同样必须传播给 WAL 调用方。
TEST(LogWriterTest, PropagatesPayloadAppendFailure) {
  FailingWritableFile destination(/*failing_append_call=*/2);
  Writer writer(&destination);

  EXPECT_TRUE(writer.AddRecord(Slice("x")).IsIOError());
}

// Writer 在每个物理 record 后 Flush；Flush 失败意味着该 record 未能成功提交。
TEST(LogWriterTest, PropagatesFlushFailure) {
  FailingWritableFile destination(/*failing_append_call=*/0,
                                  /*fail_flush=*/true);
  Writer writer(&destination);

  EXPECT_TRUE(writer.AddRecord(Slice("x")).IsIOError());
}

}  // namespace
}  // namespace log
}  // namespace LSMKV
