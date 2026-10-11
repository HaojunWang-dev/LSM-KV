#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "coding.h"
#include "crc32c.h"
#include "env.h"
#include "options.h"
#include "table_format.h"

namespace LSMKV {
namespace {

BlockHandle Handle(uint64_t offset, uint64_t size) {
  BlockHandle handle;
  handle.set_offset(offset);
  handle.set_size(size);
  return handle;
}

// A hand-checked footer, independent of Footer::EncodeTo(): handles (1,2)
// and (3,4), 36 padding bytes, then little-endian fixed64 magic.
std::string LiteralFooter() {
  return std::string("\x01\x02\x03\x04", 4) + std::string(36, '\0') +
         std::string("\x57\xfb\x80\x8b\x24\x75\x47\xdb", 8);
}

Footer InitializedFooter() {
  Footer footer;
  footer.set_metaindex_handle(Handle(11, 12));
  footer.set_index_handle(Handle(13, 14));
  return footer;
}

void ExpectUnchangedFooter(const Footer& footer) {
  EXPECT_EQ(footer.metaindex_handle().offset(), 11U);
  EXPECT_EQ(footer.metaindex_handle().size(), 12U);
  EXPECT_EQ(footer.index_handle().offset(), 13U);
  EXPECT_EQ(footer.index_handle().size(), 14U);
}

TEST(BlockHandleTest, EncodingAppendsLiteralVarints) {
  const BlockHandle handle = Handle(128, 300);
  std::string encoded("prefix\0", 7);
  handle.EncodeTo(&encoded);
  EXPECT_EQ(encoded, std::string("prefix\0", 7) +
                         std::string("\x80\x01\xac\x02", 4));
}

TEST(BlockHandleTest, DecodingConsumesOnlyTheTwoVarints) {
  const std::string encoded = std::string("\x80\x01\xac\x02", 4) + "tail";
  Slice input(encoded);
  BlockHandle handle;
  ASSERT_TRUE(handle.DecodeFrom(&input).ok());
  EXPECT_EQ(handle.offset(), 128U);
  EXPECT_EQ(handle.size(), 300U);
  EXPECT_EQ(input.ToString(), "tail");
}

TEST(BlockHandleTest, ZeroOffsetAndSizeHaveValidEncoding) {
  BlockHandle handle = Handle(0, 0);
  std::string encoded;
  handle.EncodeTo(&encoded);
  EXPECT_EQ(encoded, std::string(2, '\0'));
  Slice input(encoded);
  ASSERT_TRUE(handle.DecodeFrom(&input).ok());
  EXPECT_TRUE(input.empty());
  EXPECT_EQ(handle.offset(), 0U);
  EXPECT_EQ(handle.size(), 0U);
}

TEST(BlockHandleTest, SupportsOffsetsBeyond32Bits) {
  // 2^32 and 2^35, not values recomputed by the decoder under test.
  const std::string encoded("\x80\x80\x80\x80\x10"
                            "\x80\x80\x80\x80\x80\x01", 11);
  BlockHandle handle;
  Slice input(encoded);
  ASSERT_TRUE(handle.DecodeFrom(&input).ok());
  EXPECT_EQ(handle.offset(), uint64_t{1} << 32);
  EXPECT_EQ(handle.size(), uint64_t{1} << 35);
  EXPECT_TRUE(input.empty());
  std::string output;
  handle.EncodeTo(&output);
  EXPECT_EQ(output, encoded);
}

class BlockHandleMalformedTest : public testing::TestWithParam<std::string> {};

TEST_P(BlockHandleMalformedTest, FailurePreservesInputAndHandle) {
  const std::string encoded = GetParam();
  Slice input(encoded);
  const char* original_data = input.data();
  BlockHandle handle = Handle(11, 12);
  EXPECT_TRUE(handle.DecodeFrom(&input).IsCorruption());
  EXPECT_EQ(input.data(), original_data);
  EXPECT_EQ(input.size(), encoded.size());
  EXPECT_EQ(handle.offset(), 11U);
  EXPECT_EQ(handle.size(), 12U);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidVarints, BlockHandleMalformedTest,
    testing::Values(std::string(), std::string("\x80", 1),
                    std::string("\x01", 1), std::string("\x01\x80", 2),
                    std::string(9, '\xff') + '\x02',
                    std::string("\x01", 1) + std::string(9, '\xff') + '\x02'));

TEST(FooterTest, EncodingMatchesFixedLengthLiteralLayout) {
  Footer footer;
  footer.set_metaindex_handle(Handle(1, 2));
  footer.set_index_handle(Handle(3, 4));
  std::string encoded;
  footer.EncodeTo(&encoded);
  EXPECT_EQ(encoded, LiteralFooter());
}

TEST(FooterTest, EncodingPreservesAnExistingBinaryPrefix) {
  Footer footer;
  footer.set_metaindex_handle(Handle(1, 2));
  footer.set_index_handle(Handle(3, 4));
  const std::string prefix("sst\0prefix", 10);
  std::string encoded = prefix;
  footer.EncodeTo(&encoded);
  EXPECT_EQ(encoded, prefix + LiteralFooter());
}

TEST(FooterTest, DecodesBothDistinctHandlesAndPreservesTrailingInput) {
  const std::string encoded = LiteralFooter() + "tail";
  Slice input(encoded);
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(&input).ok());
  EXPECT_EQ(footer.metaindex_handle().offset(), 1U);
  EXPECT_EQ(footer.metaindex_handle().size(), 2U);
  EXPECT_EQ(footer.index_handle().offset(), 3U);
  EXPECT_EQ(footer.index_handle().size(), 4U);
  EXPECT_EQ(input.ToString(), "tail");
}

TEST(FooterTest, RejectsEveryTruncatedFooterWithoutChangingState) {
  const std::string complete = LiteralFooter();
  for (size_t length = 0; length < complete.size(); ++length) {
    SCOPED_TRACE(length);
    Slice input(complete.data(), length);
    Footer footer = InitializedFooter();
    EXPECT_TRUE(footer.DecodeFrom(&input).IsCorruption());
    EXPECT_EQ(input.data(), complete.data());
    EXPECT_EQ(input.size(), length);
    ExpectUnchangedFooter(footer);
  }
}

TEST(FooterTest, RejectsBadMagicWithoutChangingState) {
  std::string encoded = LiteralFooter();
  encoded.back() ^= 1;
  Slice input(encoded);
  Footer footer = InitializedFooter();
  EXPECT_TRUE(footer.DecodeFrom(&input).IsCorruption());
  EXPECT_EQ(input.data(), encoded.data());
  EXPECT_EQ(input.size(), encoded.size());
  ExpectUnchangedFooter(footer);
}

TEST(FooterTest, MalformedIndexHandleDoesNotCommitTheMetaindexHandle) {
  std::string encoded = LiteralFooter();
  // Metaindex is valid; the following index offset overflows uint64_t.
  encoded.replace(2, 10, std::string(9, '\xff') + '\x02');
  Slice input(encoded);
  Footer footer = InitializedFooter();
  EXPECT_TRUE(footer.DecodeFrom(&input).IsCorruption());
  EXPECT_EQ(input.data(), encoded.data());
  EXPECT_EQ(input.size(), encoded.size());
  ExpectUnchangedFooter(footer);
}

TEST(FooterTest, MalformedMaximumWidthHandlePreservesState) {
  std::string encoded = LiteralFooter();
  // Four valid ten-byte varints fill the entire 40-byte handle area.
  // Making the final one unterminated must fail transactionally.
  const std::string large = std::string(9, '\xff') + '\x01';
  encoded.replace(0, 40, large + large + large + std::string(10, '\x80'));
  Slice input(encoded);
  Footer footer = InitializedFooter();
  EXPECT_TRUE(footer.DecodeFrom(&input).IsCorruption());
  EXPECT_EQ(input.data(), encoded.data());
  EXPECT_EQ(input.size(), encoded.size());
  ExpectUnchangedFooter(footer);
}

TEST(FooterTest, DecodesFourMaximumWidthHandlesWithoutReadingTheMagic) {
  const std::string large = std::string("\xfe", 1) + std::string(8, '\xff') + '\x01';
  const std::string encoded = large + large + large + large +
                              std::string("\x57\xfb\x80\x8b\x24\x75\x47\xdb", 8);
  Slice input(encoded);
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(&input).ok());
  const uint64_t expected = std::numeric_limits<uint64_t>::max() - 1;
  EXPECT_EQ(footer.metaindex_handle().offset(), expected);
  EXPECT_EQ(footer.metaindex_handle().size(), expected);
  EXPECT_EQ(footer.index_handle().offset(), expected);
  EXPECT_EQ(footer.index_handle().size(), expected);
  EXPECT_TRUE(input.empty());
}

// Only the file boundary is substituted. Parsing and checksum validation
// exercise the real ReadBlock(), and reads still honor offset and short EOF.
class MemoryRandomAccessFile final : public RandomAccessFile {
 public:
  enum class Mode { kCopyToScratch, kBorrowStorage, kFail };

  explicit MemoryRandomAccessFile(std::string bytes,
                                 Mode mode = Mode::kCopyToScratch)
      : bytes_(std::move(bytes)), mode_(mode) {}

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    ++reads;
    last_offset = offset;
    last_size = n;
    *result = Slice();
    if (mode_ == Mode::kFail) return Status::IOError("injected block read");
    if (offset >= bytes_.size()) return Status::OK();
    const size_t count = std::min(n, bytes_.size() - static_cast<size_t>(offset));
    const char* start = bytes_.data() + static_cast<size_t>(offset);
    if (mode_ == Mode::kBorrowStorage) {
      *result = Slice(start, count);
    } else {
      std::memcpy(scratch, start, count);
      *result = Slice(scratch, count);
    }
    return Status::OK();
  }

  const char* data() const { return bytes_.data(); }
  mutable int reads = 0;
  mutable uint64_t last_offset = 0;
  mutable size_t last_size = 0;

 private:
  const std::string bytes_;
  const Mode mode_;
};

std::string StoredBlock(const std::string& payload, uint8_t compression = 0) {
  std::string encoded = payload;
  encoded.push_back(static_cast<char>(compression));
  PutFixed32(&encoded, crc32c::Mask(crc32c::Value(encoded.data(), encoded.size())));
  return encoded;
}

// Test-only owner of a successfully returned allocation. BlockContents itself
// intentionally does not have a destructor or transfer ownership on copying.
struct ScopedContents {
  ~ScopedContents() {
    if (contents.heap_allocated) delete[] contents.data.data();
  }
  BlockContents contents;
};

TEST(ReadBlockTest, ReadsAtTheHandleOffsetAndExcludesTheTrailer) {
  MemoryRandomAccessFile file("prefix" + StoredBlock("abc") + "unrelated");
  ReadOptions options;
  options.verify_checksums = true;
  ScopedContents result;
  ASSERT_TRUE(ReadBlock(&file, options, Handle(6, 3), &result.contents).ok());
  EXPECT_EQ(result.contents.data.ToString(), "abc");
  EXPECT_TRUE(result.contents.heap_allocated);
  EXPECT_TRUE(result.contents.cachable);
  EXPECT_EQ(file.reads, 1);
  EXPECT_EQ(file.last_offset, 6U);
  EXPECT_EQ(file.last_size, 8U);
}

TEST(ReadBlockTest, ScratchBackedContentsSurviveFileDestruction) {
  ScopedContents result;
  {
    MemoryRandomAccessFile file(StoredBlock("abc"));
    ASSERT_TRUE(ReadBlock(&file, ReadOptions(), Handle(0, 3), &result.contents).ok());
    ASSERT_TRUE(result.contents.heap_allocated);
  }
  EXPECT_EQ(result.contents.data.ToString(), "abc");
}

TEST(ReadBlockTest, BorrowedFileStorageIsNotMarkedOwnedOrCacheable) {
  MemoryRandomAccessFile file(StoredBlock("abc"),
                             MemoryRandomAccessFile::Mode::kBorrowStorage);
  ScopedContents result;
  ASSERT_TRUE(ReadBlock(&file, ReadOptions(), Handle(0, 3), &result.contents).ok());
  EXPECT_EQ(result.contents.data.data(), file.data());
  EXPECT_EQ(result.contents.data.ToString(), "abc");
  EXPECT_FALSE(result.contents.heap_allocated);
  EXPECT_FALSE(result.contents.cachable);
}

TEST(ReadBlockTest, SupportsEmptyAndBinaryPayloads) {
  for (const std::string& payload : {std::string(), std::string("a\0\xff" "b", 4)}) {
    SCOPED_TRACE(payload.size());
    MemoryRandomAccessFile file(StoredBlock(payload));
    ReadOptions options;
    options.verify_checksums = true;
    ScopedContents result;
    ASSERT_TRUE(ReadBlock(&file, options, Handle(0, payload.size()),
                          &result.contents).ok());
    EXPECT_EQ(result.contents.data.ToString(), payload);
    EXPECT_TRUE(result.contents.heap_allocated);
  }
}

TEST(ReadBlockTest, DisabledChecksumVerificationAllowsBadChecksum) {
  std::string encoded = StoredBlock("abc");
  encoded.back() ^= 1;
  MemoryRandomAccessFile file(encoded);
  ScopedContents result;
  ASSERT_TRUE(ReadBlock(&file, ReadOptions(), Handle(0, 3), &result.contents).ok());
  EXPECT_EQ(result.contents.data.ToString(), "abc");
}

TEST(ReadBlockTest, IoFailurePreservesStatusAndPriorOutput) {
  MemoryRandomAccessFile file("", MemoryRandomAccessFile::Mode::kFail);
  const std::string previous("old\0bytes", 9);
  BlockContents result{Slice(previous), true, false};
  const Status status = ReadBlock(&file, ReadOptions(), Handle(0, 3), &result);
  EXPECT_TRUE(status.IsIOError());
  EXPECT_EQ(status.ToString(), Status::IOError("injected block read").ToString());
  EXPECT_EQ(result.data.data(), previous.data());
  EXPECT_EQ(result.data.ToString(), previous);
  EXPECT_TRUE(result.cachable);
  EXPECT_FALSE(result.heap_allocated);
}

class ReadBlockChecksumTest : public testing::TestWithParam<size_t> {};

TEST_P(ReadBlockChecksumTest, RejectsCorruptionWithoutChangingPriorOutput) {
  std::string encoded = StoredBlock("abc");
  encoded[GetParam()] ^= 1;  // payload, compression byte, or stored CRC.
  MemoryRandomAccessFile file(encoded);
  ReadOptions options;
  options.verify_checksums = true;
  const std::string previous = "previous";
  BlockContents result{Slice(previous), true, false};
  EXPECT_TRUE(ReadBlock(&file, options, Handle(0, 3), &result).IsCorruption());
  EXPECT_EQ(result.data.data(), previous.data());
  EXPECT_EQ(result.data.ToString(), previous);
  EXPECT_TRUE(result.cachable);
  EXPECT_FALSE(result.heap_allocated);
}

INSTANTIATE_TEST_SUITE_P(CorruptedBytes, ReadBlockChecksumTest,
                         testing::Values(size_t{0}, size_t{3}, size_t{7}));

class ReadBlockTruncatedTest : public testing::TestWithParam<size_t> {};

TEST_P(ReadBlockTruncatedTest, ShortReadPreservesPriorOutput) {
  MemoryRandomAccessFile file(StoredBlock("abc").substr(0, GetParam()));
  const std::string previous = "previous";
  BlockContents result{Slice(previous), true, false};
  EXPECT_TRUE(ReadBlock(&file, ReadOptions(), Handle(0, 3), &result).IsCorruption());
  EXPECT_EQ(result.data.data(), previous.data());
  EXPECT_EQ(result.data.ToString(), previous);
  EXPECT_TRUE(result.cachable);
  EXPECT_FALSE(result.heap_allocated);
}

INSTANTIATE_TEST_SUITE_P(TruncatedBytes, ReadBlockTruncatedTest,
                         testing::Values(size_t{0}, size_t{2}, size_t{3}, size_t{7}));

TEST(ReadBlockTest, UnknownCompressionIsRejectedEvenWithAValidChecksum) {
  MemoryRandomAccessFile file(StoredBlock("abc", 255));
  ReadOptions options;
  options.verify_checksums = true;
  ScopedContents result;
  EXPECT_TRUE(ReadBlock(&file, options, Handle(0, 3), &result.contents).IsCorruption());
}

TEST(ReadBlockTest, UnsupportedCompressionPreservesPriorOutput) {
  for (CompressionType type : {CompressionType::kSnappyCompression,
                               CompressionType::kZstdCompression}) {
    SCOPED_TRACE(static_cast<unsigned>(type));
    MemoryRandomAccessFile file(StoredBlock("abc", static_cast<uint8_t>(type)));
    ReadOptions options;
    options.verify_checksums = true;
    const std::string previous = "previous";
    BlockContents result{Slice(previous), true, false};
    EXPECT_TRUE(ReadBlock(&file, options, Handle(0, 3), &result).IsNotSupportedError());
    EXPECT_EQ(result.data.data(), previous.data());
    EXPECT_EQ(result.data.ToString(), previous);
    EXPECT_TRUE(result.cachable);
    EXPECT_FALSE(result.heap_allocated);
  }
}

TEST(ReadBlockTest, RejectsLengthsBeyondThePointerDifferenceRange) {
  MemoryRandomAccessFile file("", MemoryRandomAccessFile::Mode::kFail);
  const uint64_t size = static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max());
  const std::string previous = "previous";
  BlockContents result{Slice(previous), true, false};
  EXPECT_TRUE(ReadBlock(&file, ReadOptions(), Handle(0, size), &result).IsCorruption());
  EXPECT_EQ(file.reads, 0);
  EXPECT_EQ(result.data.data(), previous.data());
  EXPECT_EQ(result.data.ToString(), previous);
  EXPECT_TRUE(result.cachable);
}

class ReadBlockRangeTest : public testing::TestWithParam<bool> {};

TEST_P(ReadBlockRangeTest, RejectsOverflowBeforeAllocationOrFileRead) {
  // A failing file makes a missing range guard observable without permitting
  // an out-of-bounds read through the wrapped allocation in today's code.
  MemoryRandomAccessFile file("", MemoryRandomAccessFile::Mode::kFail);
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  const BlockHandle handle = GetParam() ? Handle(0, maximum) : Handle(maximum, 1);
  const std::string previous = "previous";
  BlockContents result{Slice(previous), true, false};
  const Status status = ReadBlock(&file, ReadOptions(), handle, &result);
  EXPECT_TRUE(status.IsCorruption() || status.IsInvalidArgument());
  EXPECT_EQ(file.reads, 0);
  EXPECT_EQ(result.data.data(), previous.data());
  EXPECT_EQ(result.data.ToString(), previous);
  EXPECT_TRUE(result.cachable);
}

INSTANTIATE_TEST_SUITE_P(OverflowingRanges, ReadBlockRangeTest,
                         testing::Bool());

}  // namespace
}  // namespace LSMKV
