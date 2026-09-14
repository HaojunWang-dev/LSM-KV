#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "coding.h"
#include "crc32c.h"
#include "env.h"
#include "log_reader.h"
#include "log_writer.h"

namespace LSMKV {
namespace log {
namespace {

class StringSequentialFile final : public SequentialFile {
 public:
  explicit StringSequentialFile(std::string contents)
      : contents_(std::move(contents)) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    const size_t available = contents_.size() - pos_;
    const size_t to_read = std::min(n, available);
    std::memcpy(scratch, contents_.data() + pos_, to_read);
    pos_ += to_read;
    *result = Slice(scratch, to_read);
    return Status::OK();
  }

  Status Skip(std::uint64_t n) override {
    if (n >= contents_.size() - pos_) {
      pos_ = contents_.size();
    } else {
      pos_ += static_cast<size_t>(n);
    }
    return Status::OK();
  }

 private:
  std::string contents_;
  size_t pos_ = 0;
};

class StringWritableFile final : public WritableFile {
 public:
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

class RecordingReporter final : public Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    ++count_;
    bytes_ += bytes;
    last_status_ = status;
  }

  int count() const { return count_; }
  size_t bytes() const { return bytes_; }
  const Status& last_status() const { return last_status_; }

 private:
  int count_ = 0;
  size_t bytes_ = 0;
  Status last_status_;
};

std::string BuildWal(const std::vector<std::string>& records) {
  StringWritableFile destination;
  Writer writer(&destination);
  for (const std::string& record : records) {
    if (!writer.AddRecord(Slice(record)).ok()) {
      return {};
    }
  }
  return destination.contents();
}

void AppendPhysicalRecord(std::string* data, RecordType type,
                          const std::string& payload) {
  ASSERT_LE(payload.size(), 0xffffU);

  char header[kHeaderSize] = {};
  header[4] = static_cast<char>(payload.size() & 0xffU);
  header[5] = static_cast<char>((payload.size() >> 8U) & 0xffU);
  header[6] = static_cast<char>(type);

  const std::uint32_t crc = crc32c::Mask(crc32c::Extend(
      crc32c::Value(&header[6], 1), payload.data(), payload.size()));
  EncodeFixed32(header, crc);

  data->append(header, kHeaderSize);
  data->append(payload);
}

void ExpectNextRecord(Reader* reader, const std::string& expected,
                      std::uint64_t expected_offset, std::string* scratch) {
  Slice record;
  ASSERT_TRUE(reader->ReadRecord(&record, scratch));
  EXPECT_EQ(record.ToString(), expected);
  EXPECT_EQ(reader->LastRecordOffset(), expected_offset);
}

TEST(LogReaderTest, ExposesLevelDBStyleRecoveryApi) {
  static_assert(std::is_constructible_v<Reader, SequentialFile*,
                                        Reader::Reporter*, bool,
                                        std::uint64_t>);
  static_assert(std::is_same_v<decltype(&Reader::ReadRecord),
                               bool (Reader::*)(Slice*, std::string*)>);
  static_assert(std::is_same_v<decltype(&Reader::LastRecordOffset),
                               std::uint64_t (Reader::*)()>);
  static_assert(std::is_same_v<decltype(&Reader::Reporter::Corruption),
                               void (Reader::Reporter::*)(size_t,
                                                          const Status&)>);
}

TEST(LogReaderTest, ReadsFullEmptyAndMultipleRecords) {
  const std::string wal = BuildWal({"hello", "", "world"});
  ASSERT_FALSE(wal.empty());

  StringSequentialFile file(wal);
  Reader reader(&file, nullptr, true, 0);
  std::string scratch;

  ExpectNextRecord(&reader, "hello", 0, &scratch);
  ExpectNextRecord(&reader, "", kHeaderSize + 5U, &scratch);
  ExpectNextRecord(&reader, "world", (kHeaderSize + 5U) + kHeaderSize,
                   &scratch);

  Slice record;
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
}

TEST(LogReaderTest, ReassemblesFragmentedRecordAndContinues) {
  const size_t fragment_capacity = kBlockSize - kHeaderSize;
  const std::string large_payload(fragment_capacity * 2 + 11, 'v');
  const std::string wal = BuildWal({large_payload, "tail"});
  ASSERT_FALSE(wal.empty());

  StringSequentialFile file(wal);
  Reader reader(&file, nullptr, true, 0);
  std::string scratch;

  ExpectNextRecord(&reader, large_payload, 0, &scratch);
  ExpectNextRecord(&reader, "tail",
                   2 * kBlockSize + kHeaderSize + 11U, &scratch);

  Slice record;
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
}

TEST(LogReaderTest, SkipsChecksumMismatchAndResynchronizesAtNextBlock) {
  std::string wal;
  AppendPhysicalRecord(&wal, RecordType::kFullType, "good");
  wal.append(kBlockSize - wal.size(), '\0');

  const size_t bad_block_start = wal.size();
  ASSERT_EQ(bad_block_start, kBlockSize);
  AppendPhysicalRecord(&wal, RecordType::kFullType, "bad");
  wal[kBlockSize + kHeaderSize] ^= 0x01U;
  wal.append(kBlockSize - (wal.size() % kBlockSize), '\0');
  AppendPhysicalRecord(&wal, RecordType::kFullType, "after");

  StringSequentialFile file(wal);
  RecordingReporter reporter;
  Reader reader(&file, &reporter, true, 0);
  std::string scratch;

  ExpectNextRecord(&reader, "good", 0, &scratch);
  ExpectNextRecord(&reader, "after", 2 * kBlockSize, &scratch);

  EXPECT_GT(reporter.count(), 0);
  EXPECT_GT(reporter.bytes(), 0U);
  EXPECT_TRUE(reporter.last_status().IsCorruption());

  Slice record;
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
}

TEST(LogReaderTest, ChecksumDisabledStillParsesCorruptedPayload) {
  const std::string wal = BuildWal({"good", "bad", "after"});
  ASSERT_FALSE(wal.empty());

  std::string corrupted = wal;
  corrupted[kHeaderSize + 4U + kHeaderSize] ^= 0x01U;

  StringSequentialFile file(corrupted);
  Reader reader(&file, nullptr, false, 0);
  std::string scratch;

  ExpectNextRecord(&reader, "good", 0, &scratch);
  ExpectNextRecord(&reader, "cad", kHeaderSize + 4U, &scratch);
  ExpectNextRecord(
      &reader, "after",
      (kHeaderSize + 4U) + (kHeaderSize + 3U), &scratch);
}

TEST(LogReaderTest, InitialOffsetSkipsBlocksAndTrailerPadding) {
  const std::string first(kBlockSize - kHeaderSize, 'a');
  const std::string wal = BuildWal({first, "second"});
  ASSERT_FALSE(wal.empty());

  {
    StringSequentialFile file(wal);
    Reader reader(&file, nullptr, true, kBlockSize);
    std::string scratch;
    ExpectNextRecord(&reader, "second", kBlockSize, &scratch);

    Slice record;
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
  }

  {
    StringSequentialFile file(wal);
    Reader reader(&file, nullptr, true, kBlockSize - 3);
    std::string scratch;
    ExpectNextRecord(&reader, "second", kBlockSize, &scratch);
  }
}

TEST(LogReaderTest, DiscardsOrphanFragmentsAfterInitialOffset) {
  const size_t fragment_capacity = kBlockSize - kHeaderSize;
  const std::string fragmented_payload(fragment_capacity + 5, 'x');
  const std::string wal = BuildWal({fragmented_payload, "tail"});
  ASSERT_FALSE(wal.empty());

  StringSequentialFile file(wal);
  RecordingReporter reporter;
  Reader reader(&file, &reporter, true, kBlockSize);
  std::string scratch;

  ExpectNextRecord(&reader, "tail",
                   kBlockSize + kHeaderSize + 5U, &scratch);
  EXPECT_EQ(reporter.count(), 0);

  Slice record;
  EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
}

}  // namespace
}  // namespace log
}  // namespace LSMKV
