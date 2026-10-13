#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "coding.h"
#include "comparator.h"
#include "crc32c.h"
#include "env_posix.h"
#include "format.h"
#include "options.h"
#include "table.h"

namespace LSMKV {

// Access to the existing private read path only. No fake TableCache, public
// production test methods, or new storage implementation is introduced.
class TableTestPeer {
 public:
  static Status Get(Table* table, const ReadOptions& options, const Slice& key,
                    void* arg, void (*save)(void*, const Slice&, const Slice&)) {
    return table->InternalGet(options, key, arg, save);
  }
};

namespace {

using Entry = std::pair<std::string, std::string>;

// Independent test fixtures, not a production TableBuilder. Every entry is
// a restart: [0][key length][value length][key][value], followed by fixed32
// offsets/count. Literal expectations in tests do not call the Table reader.
std::string EncodeBlock(const std::vector<Entry>& entries) {
  std::string bytes;
  std::vector<uint32_t> restarts;
  for (const auto& entry : entries) {
    restarts.push_back(static_cast<uint32_t>(bytes.size()));
    PutVarint32(&bytes, 0);
    PutVarint32(&bytes, static_cast<uint32_t>(entry.first.size()));
    PutVarint32(&bytes, static_cast<uint32_t>(entry.second.size()));
    bytes.append(entry.first);
    bytes.append(entry.second);
  }
  if (restarts.empty()) restarts.push_back(0);
  for (uint32_t offset : restarts) PutFixed32(&bytes, offset);
  PutFixed32(&bytes, static_cast<uint32_t>(restarts.size()));
  return bytes;
}

std::string EncodeHandle(const BlockHandle& handle) {
  std::string bytes;
  PutVarint64(&bytes, handle.offset());
  PutVarint64(&bytes, handle.size());
  return bytes;
}

struct TableImage {
  std::string bytes;
  std::vector<BlockHandle> data;
  BlockHandle metaindex;
  BlockHandle index;
  size_t footer_offset = 0;

  BlockHandle AppendBlock(const std::string& payload) {
    BlockHandle handle;
    handle.set_offset(bytes.size());
    handle.set_size(payload.size());
    bytes.append(payload);
    bytes.push_back('\0');  // No compression.
    const uint32_t crc = crc32c::Value(bytes.data() + handle.offset(),
                                      payload.size() + 1);
    PutFixed32(&bytes, crc32c::Mask(crc));
    return handle;
  }

  void WriteFooter() {
    // Deliberately do not use Footer::EncodeTo: encode the complete on-disk
    // layout independently so a matching encoder/decoder bug cannot hide.
    std::string footer = EncodeHandle(metaindex) + EncodeHandle(index);
    footer.resize(40, '\0');
    footer.append("\x57\xfb\x80\x8b\x24\x75\x47\xdb", 8);
    bytes.resize(footer_offset);
    bytes.append(footer);
  }
};

TableImage MakeImage(const std::vector<std::vector<Entry>>& blocks) {
  TableImage image;
  std::vector<Entry> index_entries;
  for (const auto& block : blocks) {
    const BlockHandle handle = image.AppendBlock(EncodeBlock(block));
    image.data.push_back(handle);
    // The index key is the literal largest key of its data block. No index
    // separator shortening or comparator-specific builder is needed here.
    if (!block.empty()) index_entries.emplace_back(block.back().first, EncodeHandle(handle));
  }
  image.metaindex = image.AppendBlock(EncodeBlock({}));
  image.index = image.AppendBlock(EncodeBlock(index_entries));
  image.footer_offset = image.bytes.size();
  image.WriteFooter();
  return image;
}

TableImage BasicImage() {
  return MakeImage({{{"a", "1"}, {"c", "3"}}, {{"e", "5"}, {"g", "7"}}});
}

void ReplaceIndex(TableImage* image, const std::vector<Entry>& entries) {
  image->bytes.resize(image->index.offset());
  image->index = image->AppendBlock(EncodeBlock(entries));
  image->footer_offset = image->bytes.size();
  image->WriteFooter();
}

class ImageFile final : public RandomAccessFile {
 public:
  explicit ImageFile(std::string bytes, int* destructions = nullptr)
      : bytes_(std::move(bytes)), destructions_(destructions) {}
  ~ImageFile() override {
    if (destructions_ != nullptr) ++*destructions_;
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    offsets.push_back(offset);
    *result = Slice();
    if (fail_read && offset == failure_offset) return Status::IOError("injected table read");
    if (offset >= bytes_.size()) return Status::OK();
    size_t count = std::min(n, bytes_.size() - static_cast<size_t>(offset));
    if (short_read && offset == short_offset) count = std::min(count, short_limit);
    const char* start = bytes_.data() + static_cast<size_t>(offset);
    if (borrow_storage) {
      *result = Slice(start, count);
    } else {
      std::memcpy(scratch, start, count);
      *result = Slice(scratch, count);
    }
    return Status::OK();
  }

  bool fail_read = false;
  uint64_t failure_offset = 0;
  bool short_read = false;
  uint64_t short_offset = 0;
  size_t short_limit = 0;
  bool borrow_storage = false;
  mutable std::vector<uint64_t> offsets;

 private:
  const std::string bytes_;
  int* const destructions_;
};

struct QueryResult {
  int calls = 0;
  std::string key;
  std::string value;

  static void Save(void* arg, const Slice& key, const Slice& value) {
    auto* result = static_cast<QueryResult*>(arg);
    ++result->calls;
    // Callback Slices borrow the data iterator; retain copies, not views.
    result->key = key.ToString();
    result->value = value.ToString();
  }
};

class ReverseComparator final : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override { return b.Compare(a); }
  const char* Name() const override { return "test.TableReverseBytewise"; }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

class TableTest : public testing::Test {
 protected:
  void SetUp() override { options_.comparator = BytewiseComparator(); }

  void Load(const TableImage& image) {
    table_.reset();
    image_ = image;
    file_ = std::make_unique<ImageFile>(image_.bytes);
  }

  Status Open() {
    Table* result = nullptr;
    const Status status = Table::Open(options_, file_.get(), image_.bytes.size(), &result);
    table_.reset(result);
    return status;
  }

  void ExpectQuery(const std::string& target, const std::string& key,
                   const std::string& value, const ReadOptions& options = ReadOptions()) {
    QueryResult result;
    const Status status = TableTestPeer::Get(table_.get(), options, target,
                                            &result, &QueryResult::Save);
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_EQ(result.calls, 1);
    EXPECT_EQ(result.key, key);
    EXPECT_EQ(result.value, value);
  }

  void ExpectQueryError(const std::string& target, bool io_error = false) {
    QueryResult result;
    ReadOptions options;
    options.verify_checksums = true;
    const Status status = TableTestPeer::Get(table_.get(), options, target,
                                            &result, &QueryResult::Save);
    if (io_error) {
      EXPECT_TRUE(status.IsIOError());
      EXPECT_EQ(status.ToString(), Status::IOError("injected table read").ToString());
    } else {
      EXPECT_TRUE(status.IsCorruption()) << status.ToString();
    }
    EXPECT_EQ(result.calls, 0);
  }

  // Comparators/file outlive the Table, which in turn owns its index block.
  ReverseComparator reverse_;
  InternalKeyComparator internal_;
  Options options_{};
  TableImage image_;
  std::unique_ptr<ImageFile> file_;
  std::unique_ptr<Table> table_;
};

TEST_F(TableTest, QueriesKeysInSeveralDataBlocks) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("c", "c", "3"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("e", "e", "5"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("g", "g", "7"));
}

TEST_F(TableTest, ValueInitializedOptionsSupportQueriesWithoutSettingComparator) {
  options_ = Options{};
  // Fail explicitly rather than dereferencing a missing default comparator.
  ASSERT_NE(options_.comparator, nullptr);
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("b", "c", "3"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("g", "g", "7"));
}

TEST_F(TableTest, DefaultInitializedOptionsSupportQueriesWithoutSettingComparator) {
  Options options;
  options_ = options;
  ASSERT_NE(options_.comparator, nullptr);
  Load(MakeImage({{{"a", "first"}, {std::string("a\0b", 3), "binary"}},
                  {{std::string("\xff", 1), "last"}}}));
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "first"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(std::string("a\0b", 3),
                                     std::string("a\0b", 3), "binary"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(std::string("\xff", 1),
                                     std::string("\xff", 1), "last"));
}

TEST_F(TableTest, PointQueryReturnsALowerBoundCandidateNotAnExactMatch) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("b", "c", "3"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("d", "e", "5"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("", "a", "1"));
}

TEST_F(TableTest, PastTheLastKeyReturnsOkWithoutCallingTheCallback) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  QueryResult result;
  EXPECT_TRUE(TableTestPeer::Get(table_.get(), ReadOptions(), "z", &result,
                                &QueryResult::Save).ok());
  EXPECT_EQ(result.calls, 0);  // NotFound is determined by the higher read layer.
}

TEST_F(TableTest, EmptyTableReturnsNoCandidate) {
  Load(MakeImage({}));
  ASSERT_TRUE(Open().ok());
  QueryResult result;
  EXPECT_TRUE(TableTestPeer::Get(table_.get(), ReadOptions(), "a", &result,
                                &QueryResult::Save).ok());
  EXPECT_EQ(result.calls, 0);
}

TEST_F(TableTest, EmptyDataBlockReturnsNoCandidate) {
  TableImage image = MakeImage({{}});
  ReplaceIndex(&image, {{"z", EncodeHandle(image.data[0])}});
  Load(image);
  ASSERT_TRUE(Open().ok());
  QueryResult result;
  EXPECT_TRUE(TableTestPeer::Get(table_.get(), ReadOptions(), "a", &result,
                                &QueryResult::Save).ok());
  EXPECT_EQ(result.calls, 0);
}

TEST_F(TableTest, SupportsEmptyAndBinaryKeysAndValues) {
  Load(MakeImage({{{"", ""}, {std::string("a\0b", 3), std::string("v\0x", 3)}},
                  {{std::string("\xff", 1), ""}}}));
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("", "", ""));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(std::string("a\0b", 3), std::string("a\0b", 3),
                                     std::string("v\0x", 3)));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(std::string("\xff", 1), std::string("\xff", 1), ""));
}

TEST_F(TableTest, HandlesMultibyteLengthsAndLargeValues) {
  const std::string key(130, 'a');
  const std::string value(16384, 'v');
  Load(MakeImage({{{key, value}}, {{"z", "last"}}}));
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(key, key, value));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("z", "z", "last"));
}

TEST_F(TableTest, UsesTheCustomComparatorForIndexAndDataBlocks) {
  options_.comparator = &reverse_;
  Load(MakeImage({{{"z", "1"}, {"m", "2"}}, {{"d", "3"}, {"a", "4"}}}));
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("n", "m", "2"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("e", "d", "3"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "4"));
}

TEST_F(TableTest, SnapshotLookupReturnsTheVisibleInternalKeyCandidate) {
  options_.comparator = &internal_;
  const std::string newest = InternalKey("key", 9, ValueType::kValue).Encode().ToString();
  const std::string deletion = InternalKey("key", 7, ValueType::kDeletion).Encode().ToString();
  const std::string oldest = InternalKey("key", 3, ValueType::kValue).Encode().ToString();
  Load(MakeImage({{{newest, "new"}}, {{deletion, ""}, {oldest, "old"}}}));
  ASSERT_TRUE(Open().ok());
  const LookupKey at8("key", 8);
  const LookupKey at4("key", 4);
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(at8.internal_key().ToString(), deletion, ""));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery(at4.internal_key().ToString(), oldest, "old"));
}

TEST_F(TableTest, OpenDoesNotEagerlyReadDataBlocks) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  EXPECT_EQ(std::count(file_->offsets.begin(), file_->offsets.end(), image_.data[0].offset()), 0);
  EXPECT_EQ(std::count(file_->offsets.begin(), file_->offsets.end(), image_.data[1].offset()), 0);
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("e", "e", "5"));
  EXPECT_EQ(std::count(file_->offsets.begin(), file_->offsets.end(), image_.data[0].offset()), 0);
  EXPECT_EQ(std::count(file_->offsets.begin(), file_->offsets.end(), image_.data[1].offset()), 1);
}

TEST_F(TableTest, RepeatedQueriesReleaseTheirDataBlocksAndRetainCopiedResults) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  QueryResult retained;
  ASSERT_TRUE(TableTestPeer::Get(table_.get(), ReadOptions(), "a", &retained,
                                &QueryResult::Save).ok());
  for (int i = 0; i < 32; ++i) {
    ASSERT_NO_FATAL_FAILURE(ExpectQuery("g", "g", "7"));
  }
  table_.reset();
  file_.reset();
  EXPECT_EQ(retained.calls, 1);
  EXPECT_EQ(retained.key, "a");
  EXPECT_EQ(retained.value, "1");
}

TEST_F(TableTest, FileBackedBlockStorageIsBorrowedRatherThanFreed) {
  Load(BasicImage());
  file_->borrow_storage = true;
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("g", "g", "7"));
  table_.reset();
  char scratch = 0;
  Slice read;
  ASSERT_TRUE(file_->Read(0, 1, &read, &scratch).ok());
  EXPECT_EQ(read.ToString(), std::string(1, '\0'));
}

TEST_F(TableTest, DestroyingTableDoesNotDestroyItsBorrowedFile) {
  int destructions = 0;
  const TableImage image = BasicImage();
  auto file = std::make_unique<ImageFile>(image.bytes, &destructions);
  Table* raw = nullptr;
  ASSERT_TRUE(Table::Open(options_, file.get(), image.bytes.size(), &raw).ok());
  std::unique_ptr<Table> table(raw);
  table.reset();
  EXPECT_EQ(destructions, 0);
  file.reset();
  EXPECT_EQ(destructions, 1);
}

TEST_F(TableTest, RejectsFilesShorterThanTheFooterAndClearsTheOutputSlot) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  for (size_t length : {size_t{0}, size_t{47}}) {
    Table* output = table_.get();  // Existing object remains owned by table_.
    EXPECT_TRUE(Table::Open(options_, file_.get(), length, &output).IsCorruption());
    EXPECT_EQ(output, nullptr);
  }
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
}

TEST_F(TableTest, FooterReadIoFailurePreservesTheStatus) {
  Load(BasicImage());
  file_->fail_read = true;
  file_->failure_offset = image_.footer_offset;
  const Status status = Open();
  EXPECT_TRUE(status.IsIOError());
  EXPECT_EQ(status.ToString(), Status::IOError("injected table read").ToString());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, RejectsShortFooterRead) {
  Load(BasicImage());
  file_->short_read = true;
  file_->short_offset = image_.footer_offset;
  file_->short_limit = 47;
  EXPECT_TRUE(Open().IsCorruption());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, RejectsBadFooterMagic) {
  TableImage image = BasicImage();
  image.bytes.back() ^= 1;
  Load(image);
  EXPECT_TRUE(Open().IsCorruption());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, RejectsMalformedFooterHandleEncoding) {
  TableImage image = BasicImage();
  image.bytes.replace(image.footer_offset, 10, std::string(9, '\xff') + '\x02');
  Load(image);
  EXPECT_TRUE(Open().IsCorruption());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, IndexReadIoFailurePreservesTheStatus) {
  Load(BasicImage());
  file_->fail_read = true;
  file_->failure_offset = image_.index.offset();
  const Status status = Open();
  EXPECT_TRUE(status.IsIOError());
  EXPECT_EQ(status.ToString(), Status::IOError("injected table read").ToString());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, RejectsShortIndexRead) {
  Load(BasicImage());
  file_->short_read = true;
  file_->short_offset = image_.index.offset();
  file_->short_limit = image_.index.size() + 4;
  EXPECT_TRUE(Open().IsCorruption());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, ParanoidOpenRejectsBadIndexChecksum) {
  TableImage image = BasicImage();
  image.bytes[image.index.offset() + image.index.size() + 1] ^= 1;
  Load(image);
  EXPECT_TRUE(Open().IsCorruption());
  EXPECT_EQ(table_, nullptr);
}

TEST_F(TableTest, NonParanoidOpenCanSkipIndexChecksumValidation) {
  options_.paranoid_checks = false;
  TableImage image = BasicImage();
  image.bytes[image.index.offset() + image.index.size() + 1] ^= 1;
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
}

TEST_F(TableTest, MissingOptionalMetadataDoesNotPreventPointQueries) {
  TableImage image = BasicImage();
  image.metaindex.set_offset(image.bytes.size() + 100);
  image.WriteFooter();
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
}

TEST_F(TableTest, DataReadIoFailureIsNotReportedAsAMiss) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  file_->fail_read = true;
  file_->failure_offset = image_.data[1].offset();
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("e", true));
}

TEST_F(TableTest, ShortDataReadReportsCorruptionWithoutCallingTheCallback) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  file_->short_read = true;
  file_->short_offset = image_.data[0].offset();
  file_->short_limit = image_.data[0].size() + 4;
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, ReadOptionsEnableDataChecksumVerification) {
  TableImage image = BasicImage();
  image.bytes[image.data[0].offset() + image.data[0].size() + 1] ^= 1;
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, ReadOptionsCanDisableDataChecksumVerification) {
  TableImage image = BasicImage();
  image.bytes[image.data[0].offset() + image.data[0].size() + 1] ^= 1;
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
}

TEST_F(TableTest, MalformedIndexValuePropagatesHandleDecodeError) {
  TableImage image = BasicImage();
  ReplaceIndex(&image, {{"g", std::string("\x80", 1)}});
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, IndexHandleExtensionsDoNotBreakPointQueries) {
  TableImage image = BasicImage();
  ReplaceIndex(&image, {{"c", EncodeHandle(image.data[0]) + "extension"},
                        {"g", EncodeHandle(image.data[1]) + "extension"}});
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("a", "a", "1"));
  ASSERT_NO_FATAL_FAILURE(ExpectQuery("e", "e", "5"));
}

TEST_F(TableTest, DataHandlePastEofReportsCorruption) {
  TableImage image = BasicImage();
  BlockHandle handle;
  handle.set_offset(image.bytes.size() + 100);
  handle.set_size(8);
  ReplaceIndex(&image, {{"g", EncodeHandle(handle)}});
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, MalformedDataBlockWithValidChecksumReportsCorruption) {
  TableImage image;
  const BlockHandle handle = image.AppendBlock(std::string(3, '\0'));
  image.metaindex = image.AppendBlock(EncodeBlock({}));
  image.index = image.AppendBlock(EncodeBlock({{"z", EncodeHandle(handle)}}));
  image.footer_offset = image.bytes.size();
  image.WriteFooter();
  Load(image);
  ASSERT_TRUE(Open().ok());
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, IndexIteratorErrorsPropagateInsteadOfReturningAMiss) {
  TableImage image = BasicImage();
  image.bytes.resize(image.index.offset());
  image.index = image.AppendBlock(std::string(3, '\0'));
  image.footer_offset = image.bytes.size();
  image.WriteFooter();
  Load(image);
  const Status opened = Open();
  // Both eager rejection and lazy iterator validation are valid policies.
  if (!opened.ok()) {
    EXPECT_TRUE(opened.IsCorruption());
    EXPECT_EQ(table_, nullptr);
    return;
  }
  ASSERT_NO_FATAL_FAILURE(ExpectQueryError("a"));
}

TEST_F(TableTest, ApproximateOffsetUsesIndexHandlesAndTheEndFallback) {
  Load(BasicImage());
  ASSERT_TRUE(Open().ok());
  // Each two-entry data block is 22 payload bytes + 5 trailer bytes.
  // The data block offsets are 0,27; the metaindex begins at 54.
  EXPECT_EQ(table_->ApproximateOffset("b"), 0U);
  EXPECT_EQ(table_->ApproximateOffset("f"), 27U);
  EXPECT_EQ(table_->ApproximateOffset("z"), 54U);
}

TEST_F(TableTest, ApproximateOffsetFallsBackForMalformedIndexValues) {
  TableImage image = BasicImage();
  ReplaceIndex(&image, {{"g", std::string("\x80", 1)}});
  Load(image);
  ASSERT_TRUE(Open().ok());
  EXPECT_EQ(table_->ApproximateOffset("a"), 54U);
}

struct TemporaryFile {
  ~TemporaryFile() {
    if (!path.empty()) ::unlink(path.c_str());
  }
  std::string path;
};

TEST(TablePosixTest, OpensAndQueriesACompleteSstableThroughTheRealFileInterface) {
  TemporaryFile temporary;
  char pattern[] = "/tmp/lsmkv-table-XXXXXX";
  const int fd = ::mkstemp(pattern);
  ASSERT_GE(fd, 0);
  temporary.path = pattern;
  ASSERT_EQ(::close(fd), 0);

  PosixEnv env;
  const TableImage image = BasicImage();
  WritableFile* raw_writer = nullptr;
  ASSERT_TRUE(env.NewWritableFile(temporary.path, &raw_writer).ok());
  std::unique_ptr<WritableFile> writer(raw_writer);
  ASSERT_TRUE(writer->Append(image.bytes).ok());
  ASSERT_TRUE(writer->Close().ok());
  writer.reset();

  uint64_t file_size = 0;
  ASSERT_TRUE(env.GetFileSize(temporary.path, &file_size).ok());
  RandomAccessFile* raw_file = nullptr;
  ASSERT_TRUE(env.NewRandomAccessFile(temporary.path, &raw_file).ok());
  std::unique_ptr<RandomAccessFile> file(raw_file);
  Options options{};
  options.comparator = BytewiseComparator();
  Table* raw_table = nullptr;
  ASSERT_TRUE(Table::Open(options, file.get(), file_size, &raw_table).ok());
  std::unique_ptr<Table> table(raw_table);

  ReadOptions read;
  read.verify_checksums = true;
  for (const auto& expected : std::vector<Entry>{{"a", "1"}, {"e", "5"}, {"g", "7"}}) {
    QueryResult result;
    ASSERT_TRUE(TableTestPeer::Get(table.get(), read, expected.first, &result,
                                  &QueryResult::Save).ok());
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(result.key, expected.first);
    EXPECT_EQ(result.value, expected.second);
  }
}

// Table::NewIterator is intentionally not exercised while its body has no
// return statement. Whole-table iteration requires its separate implementation.

}  // namespace
}  // namespace LSMKV
