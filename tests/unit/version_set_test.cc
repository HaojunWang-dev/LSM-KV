#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <utility>
#include <vector>
#include <unistd.h>

#include "VersionSet.h"
#include "coding.h"
#include "crc32c.h"
#include "env_posix.h"
#include "filename.h"
#include "log_reader.h"

namespace LSMKV {

// 构造测试所需的已验证文件集合，不为生产接口增加测试专用方法。
class VersionSetTestPeer {
 public:
  struct File {
    int level;
    uint64_t number;
    uint64_t size;
    const char* smallest;
    const char* largest;
  };

  static void InstallFiles(VersionSet* versions, const std::vector<File>& files) {
    auto destroy = [](Version* version) { delete version; };
    std::unique_ptr<Version, decltype(destroy)> next(new Version(versions), destroy);
    for (const File& source : files) {
      auto file = std::make_unique<FileMetaData>();
      file->number = source.number;
      file->file_size = source.size;
      file->smallest = InternalKey(Slice(source.smallest), 1, ValueType::kValue);
      file->largest = InternalKey(Slice(source.largest), 1, ValueType::kValue);
      file->refs = 1;
      next->files_[source.level].push_back(file.get());
      file.release();  // 元数据所有权转交给 Version 的引用计数管理。
    }
    versions->AppendVersion(next.get());
    next.release();
  }

  static void SetStoredNumbers(VersionSet* versions, uint64_t log,
                               uint64_t previous_log, uint64_t manifest) {
    versions->log_number_ = log;
    versions->prev_log_number_ = previous_log;
    versions->manifest_file_number_ = manifest;
  }

  static std::pair<int, double> FinalizeCurrent(VersionSet* versions) {
    versions->Finalize(versions->current_);
    return {versions->current_->compaction_level_,
            versions->current_->compaction_score_};
  }

  static bool ReuseManifest(VersionSet* versions, const std::string& name,
                            const std::string& basename) {
    return versions->ReuseManifest(name, basename);
  }

  static bool HasManifestWriter(VersionSet* versions) {
    return versions->descriptor_file_ != nullptr && versions->descriptor_log_ != nullptr;
  }

  static Status AppendManifestRecord(VersionSet* versions, const Slice& record) {
    return versions->descriptor_log_->AddRecord(record);
  }
};

namespace {

class VersionSetTest : public ::testing::Test {
 protected:
  Options options_;
  InternalKeyComparator comparator_;
  VersionSet versions_{"metadata-test", &options_, nullptr, &comparator_};
};

class ManifestEnv final : public Env {
 public:
  std::string contents;
  Status size_status;
  Status append_status;
  Status read_status;
  bool return_null_file = false;
  int append_opens = 0;
  int size_queries = 0;
  int file_destructions = 0;

  class Output final : public WritableFile {
   public:
    explicit Output(ManifestEnv* env) : env_(env) {}
    ~Output() override { ++env_->file_destructions; }
    Status Append(const Slice& bytes) override {
      env_->contents.append(bytes.data(), bytes.size());
      return Status::OK();
    }
    Status Flush() override { return Status::OK(); }
    Status Sync() override { return Status::OK(); }
    Status Close() override { return Status::OK(); }
   private:
    ManifestEnv* env_;  // 借用，Env 覆盖文件生命周期。
  };
  class Input final : public SequentialFile {
   public:
    explicit Input(std::string contents) : contents_(std::move(contents)) {}
    Status Read(size_t n, Slice* result, char* scratch) override {
      const size_t count = std::min(n, contents_.size() - position_);
      std::memcpy(scratch, contents_.data() + position_, count);
      position_ += count;
      *result = Slice(scratch, count);
      return Status::OK();
    }
    Status Skip(uint64_t) override { return Status::NotSupported("unused"); }
   private:
    std::string contents_;
    size_t position_ = 0;
  };
  Status NewWritableFile(const std::string&, WritableFile** result) override {
    *result = nullptr;
    return Status::NotSupported("unused");
  }
  Status NewAppendableFile(const std::string& name, WritableFile** result) override {
    EXPECT_EQ(name, "metadata-test/MANIFEST-000007");
    ++append_opens;
    *result = nullptr;
    if (append_status.ok() && !return_null_file) *result = new Output(this);
    return append_status;
  }
  Status NewSequentialFile(const std::string& name, SequentialFile** result) override {
    EXPECT_EQ(name, "metadata-test/MANIFEST-000007");
    *result = read_status.ok() ? new Input(contents) : nullptr;
    return read_status;
  }
  Status GetFileSize(const std::string& name, uint64_t* size) override {
    EXPECT_EQ(name, "metadata-test/MANIFEST-000007");
    ++size_queries;
    *size = size_status.ok() ? contents.size() : 0;
    return size_status;
  }
  Status FileExists(const std::string&, bool*) override { return Status::NotSupported("unused"); }
  Status CreateDir(const std::string&) override { return Status::NotSupported("unused"); }
  Status RenameFile(const std::string&, const std::string&) override { return Status::NotSupported("unused"); }
  Status RemoveFile(const std::string&) override { return Status::NotSupported("unused"); }
  Status SyncDir(const std::string&) override { return Status::NotSupported("unused"); }
};

class ManifestReuseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    options_.reuse_logs = true;
    versions_ = std::make_unique<VersionSet>("metadata-test", &options_, nullptr, &comparator_);
    versions_->MarkFileNumberUsed(50);
    VersionSetTestPeer::SetStoredNumbers(versions_.get(), 2, 0, 51);
    ManifestEnv::Output file(&env_);
    log::Writer writer(&file);
    ASSERT_TRUE(writer.AddRecord("old metadata").ok());
  }
  bool Reuse(const std::string& basename = "MANIFEST-000007") {
    return VersionSetTestPeer::ReuseManifest(
        versions_.get(), "metadata-test/" + basename, basename);
  }
  void ExpectUnchanged(const std::string& original) {
    EXPECT_EQ(env_.contents, original);
    EXPECT_EQ(versions_->ManifestFileNumber(), 51U);
    EXPECT_EQ(versions_->LogNumber(), 2U);
    EXPECT_FALSE(VersionSetTestPeer::HasManifestWriter(versions_.get()));
  }
  ManifestEnv env_;
  Options options_;
  InternalKeyComparator comparator_;
  std::unique_ptr<VersionSet> versions_;
};

TEST_F(ManifestReuseTest, DisabledByDefaultWithoutOpeningFiles) {
  Options defaults;
  options_.reuse_logs = defaults.reuse_logs;
  const std::string original = env_.contents;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(original);
  EXPECT_EQ(env_.size_queries, 0);
  EXPECT_EQ(env_.append_opens, 0);
}

TEST_F(ManifestReuseTest, RejectsInvalidNamesAndMismatchedPaths) {
  const std::string original = env_.contents;
  for (const std::string name : {"CURRENT", "000007.log", "MANIFEST-0", "MANIFEST-x", "../MANIFEST-000007", "MANIFEST-000051"}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(Reuse(name));
    ExpectUnchanged(original);
  }
  EXPECT_FALSE(VersionSetTestPeer::ReuseManifest(versions_.get(), "other/MANIFEST-000007", "MANIFEST-000007"));
  EXPECT_EQ(env_.size_queries, 0);
  EXPECT_EQ(env_.append_opens, 0);
}

TEST_F(ManifestReuseTest, RejectsSizeAtOrAboveConfiguredLimit) {
  const std::string original = env_.contents;
  for (size_t limit : {original.size() - 1, original.size(), size_t{0}}) {
    options_.max_file_size = limit;
    EXPECT_FALSE(Reuse());
    ExpectUnchanged(original);
  }
  EXPECT_EQ(env_.append_opens, 0);
  options_.max_file_size = original.size() + 1;
  EXPECT_TRUE(Reuse());
  EXPECT_EQ(versions_->ManifestFileNumber(), 7U);
}

TEST_F(ManifestReuseTest, SizeOrOpenErrorsLeaveStateUnchanged) {
  const std::string original = env_.contents;
  env_.size_status = Status::IOError("stat failed");
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(original);
  EXPECT_EQ(env_.append_opens, 0);
  env_.size_status = Status::OK();
  env_.read_status = Status::IOError("read failed");
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(original);
  EXPECT_EQ(env_.append_opens, 0);
  env_.read_status = Status::OK();
  env_.append_status = Status::IOError("open failed");
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(original);
  env_.append_status = Status::OK();
  env_.return_null_file = true;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(original);
}

TEST_F(ManifestReuseTest, IncompleteTailMustNotBeReused) {
  const std::string complete = env_.contents;
  env_.contents += "bad";
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(complete + "bad");
  env_.contents = complete.substr(0, complete.size() - 1);
  const std::string truncated = env_.contents;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(truncated);
  EXPECT_EQ(env_.append_opens, 0);
}

TEST_F(ManifestReuseTest, RejectsInvalidChecksumOrUnfinishedLogicalRecord) {
  const std::string original = env_.contents;
  env_.contents[0] ^= 1;
  const std::string bad_checksum = env_.contents;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(bad_checksum);

  // CRC 正确的孤立 MIDDLE，不能只校验物理记录而漏掉逻辑分片约束。
  env_.contents = original;
  env_.contents[6] = static_cast<char>(log::RecordType::kMiddleType);
  EncodeFixed32(env_.contents.data(), crc32c::Mask(
      crc32c::Value(env_.contents.data() + 6, env_.contents.size() - 6)));
  const std::string orphan_fragment = env_.contents;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(orphan_fragment);

  env_.contents.clear();
  {
    ManifestEnv::Output file(&env_);
    log::Writer writer(&file);
    ASSERT_TRUE(writer.AddRecord(std::string(40000, 'x')).ok());
  }
  // FIRST 物理记录完整，但其后的 LAST 丢失。
  env_.contents.resize(log::kBlockSize);
  const std::string unfinished = env_.contents;
  EXPECT_FALSE(Reuse());
  ExpectUnchanged(unfinished);
  EXPECT_EQ(env_.append_opens, 0);
}

TEST_F(ManifestReuseTest, VersionSetOwnsReusedFileUntilDestruction) {
  const int before = env_.file_destructions;
  ASSERT_TRUE(Reuse());
  EXPECT_EQ(env_.file_destructions, before);
  versions_.reset();
  EXPECT_EQ(env_.file_destructions, before + 1);
}

TEST_F(ManifestReuseTest, AppendsAtExistingBlockOffsetAndKeepsOldRecordsReadable) {
  for (size_t payload_size : {size_t{12}, size_t{32758}, size_t{32761}, size_t{40000}}) {
    SCOPED_TRACE(payload_size);
    env_.contents.clear();
    const std::string old_record(payload_size, 'x');
    {
      ManifestEnv::Output file(&env_);
      log::Writer writer(&file);
      ASSERT_TRUE(writer.AddRecord(old_record).ok());
    }
    const std::string original = env_.contents;
    {
      VersionSet versions("metadata-test", &options_, nullptr, &comparator_);
      versions.MarkFileNumberUsed(50);
      ASSERT_TRUE(VersionSetTestPeer::ReuseManifest(
          &versions, "metadata-test/MANIFEST-000007", "MANIFEST-000007"));
      EXPECT_EQ(versions.ManifestFileNumber(), 7U);
      ASSERT_TRUE(VersionSetTestPeer::AppendManifestRecord(&versions, "new metadata").ok());
    }
    EXPECT_EQ(env_.contents.substr(0, original.size()), original);
    ManifestEnv::Input file(env_.contents);
    log::Reader reader(&file, nullptr, true, 0);
    std::string scratch;
    Slice record;
    ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
    EXPECT_EQ(record.ToString(), old_record);
    ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
    EXPECT_EQ(record.ToString(), "new metadata");
    EXPECT_FALSE(reader.ReadRecord(&record, &scratch));
  }
}

class ManifestRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/lsmkv-manifest-XXXXXX";
    char* directory = ::mkdtemp(pattern);
    ASSERT_NE(directory, nullptr);
    path_ = directory;
    options_.env = &env_;
    options_.reuse_logs = true;
    VersionEdit edit;
    edit.SetComparatorName(comparator_.user_comparator()->Name());
    edit.SetLogNumber(2);
    edit.SetNextFile(50);
    edit.SetLastSequence(10);
    edit.AddFile(1, 20, 100, InternalKey("a", 10, ValueType::kValue),
                 InternalKey("b", 10, ValueType::kValue));
    std::string record;
    edit.EncodeTo(&record);
    WritableFile* result = nullptr;
    ASSERT_TRUE(env_.NewWritableFile(DescriptorFileName(path_, 1), &result).ok());
    std::unique_ptr<WritableFile> file(result);
    { log::Writer writer(file.get()); ASSERT_TRUE(writer.AddRecord(record).ok()); }
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
    ASSERT_TRUE(SetCurrentFile(&env_, path_, 1).ok());
  }
  void TearDown() override {
    if (path_.empty()) return;
    for (const char* name : {"CURRENT", "MANIFEST-000001", "000001.dbtmp"}) {
      ::unlink((path_ + "/" + name).c_str());
    }
    ::rmdir(path_.c_str());
  }
  PosixEnv env_;
  Options options_;
  InternalKeyComparator comparator_;
  std::string path_;
};

TEST_F(ManifestRecoveryTest, ReuseSurvivesAppendAndSecondRecovery) {
  std::string original;
  ASSERT_TRUE(ReadFileToString(&env_, DescriptorFileName(path_, 1), &original).ok());
  {
    VersionSet versions(path_, &options_, nullptr, &comparator_);
    bool save_manifest = false;
    ASSERT_TRUE(versions.Recover(&save_manifest).ok());
    EXPECT_FALSE(save_manifest);
    EXPECT_EQ(versions.ManifestFileNumber(), 1U);
    versions.SetLastSequence(11);
    VersionEdit edit;
    edit.AddFile(1, 21, 200, InternalKey("c", 11, ValueType::kValue),
                 InternalKey("d", 11, ValueType::kValue));
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(versions.LogAndApply(&edit, &mutex).ok());
  }
  std::string contents;
  ASSERT_TRUE(ReadFileToString(&env_, DescriptorFileName(path_, 1), &contents).ok());
  EXPECT_EQ(contents.substr(0, original.size()), original);
  EXPECT_GT(contents.size(), original.size());
  VersionSet recovered(path_, &options_, nullptr, &comparator_);
  bool save_manifest = false;
  ASSERT_TRUE(recovered.Recover(&save_manifest).ok());
  EXPECT_FALSE(save_manifest);
  EXPECT_EQ(recovered.NumLevelFiles(1), 2);
  EXPECT_EQ(recovered.LastSequence(), 11U);
}

TEST_F(ManifestRecoveryTest, DisabledReuseRequestsNewManifest) {
  options_.reuse_logs = false;
  VersionSet versions(path_, &options_, nullptr, &comparator_);
  bool save_manifest = false;
  ASSERT_TRUE(versions.Recover(&save_manifest).ok());
  EXPECT_TRUE(save_manifest);
  EXPECT_FALSE(VersionSetTestPeer::HasManifestWriter(&versions));
}

TEST_F(ManifestRecoveryTest, IncompleteTailRequestsFreshManifestWithoutChangingOldFile) {
  WritableFile* result = nullptr;
  ASSERT_TRUE(env_.NewAppendableFile(DescriptorFileName(path_, 1), &result).ok());
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(file->Append("bad").ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  file.reset();
  std::string original;
  ASSERT_TRUE(ReadFileToString(&env_, DescriptorFileName(path_, 1), &original).ok());
  VersionSet versions(path_, &options_, nullptr, &comparator_);
  bool save_manifest = false;
  ASSERT_TRUE(versions.Recover(&save_manifest).ok());
  EXPECT_TRUE(save_manifest);
  EXPECT_FALSE(VersionSetTestPeer::HasManifestWriter(&versions));
  EXPECT_EQ(versions.NumLevelFiles(1), 1);
  std::string after;
  ASSERT_TRUE(ReadFileToString(&env_, DescriptorFileName(path_, 1), &after).ok());
  EXPECT_EQ(after, original);
}

TEST_F(VersionSetTest, FinalizeEmptyVersionHasZeroScore) {
  const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 0);
  EXPECT_DOUBLE_EQ(result.second, 0.0);
}

TEST_F(VersionSetTest, FinalizeLevelZeroUsesFileCountRatherThanBytes) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, 1ULL << 40, "a", "z"},
                   {0, 11, 1, "a", "z"}, {0, 12, 1, "a", "z"}});
  auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 0);
  EXPECT_DOUBLE_EQ(result.second, 0.75);
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, 1, "a", "z"}, {0, 11, 1, "a", "z"},
                   {0, 12, 1, "a", "z"}, {0, 13, 1, "a", "z"}});
  result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_DOUBLE_EQ(result.second, 1.0);
}

TEST_F(VersionSetTest, FinalizeHigherLevelsUseGeometricallyGrowingByteLimits) {
  const std::pair<int, uint64_t> limits[] = {
      {1, 10485760}, {2, 104857600}, {3, 1048576000},
      {4, 10485760000ULL}, {5, 104857600000ULL}};
  for (const auto& [level, limit] : limits) {
    SCOPED_TRACE(level);
    for (int adjustment : {-1, 0, 1}) {
      SCOPED_TRACE(adjustment);
      const uint64_t bytes = adjustment < 0 ? limit - 1 : limit + adjustment;
      VersionSetTestPeer::InstallFiles(&versions_, {{level, 10, bytes, "a", "b"}});
      const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
      EXPECT_EQ(result.first, level);
      if (adjustment < 0) EXPECT_LT(result.second, 1.0);
      else if (adjustment > 0) EXPECT_GT(result.second, 1.0);
      else EXPECT_DOUBLE_EQ(result.second, 1.0);
    }
  }
}

TEST_F(VersionSetTest, FinalizeSumsFilesAndSelectsLargestScore) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, 1, "a", "z"},
                   {1, 20, 7864320, "a", "b"}, {1, 21, 7864320, "c", "d"},
                   {2, 30, 104857600, "a", "b"}});
  const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 1);
  EXPECT_DOUBLE_EQ(result.second, 1.5);
}

TEST_F(VersionSetTest, FinalizePrefersLowerLevelForEqualScores) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, 1, "a", "z"}, {0, 11, 1, "a", "z"},
                   {0, 12, 1, "a", "z"}, {0, 13, 1, "a", "z"},
                   {1, 20, 10485760, "a", "b"},
                   {2, 30, 104857600, "a", "b"}});
  const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 0);
  EXPECT_DOUBLE_EQ(result.second, 1.0);
}

TEST_F(VersionSetTest, FinalizeDoesNotSelectBottomLevelWithoutAnOutputLevel) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{config::kNumLevels - 1, 10, 1ULL << 60, "a", "b"}});
  const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 0);
  EXPECT_DOUBLE_EQ(result.second, 0.0);
}

TEST_F(VersionSetTest, FinalizeLargeFileSizesDoNotWrapByteTotals) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{1, 10, 1ULL << 63, "a", "b"},
                   {1, 11, 1ULL << 63, "c", "d"}});
  const auto result = VersionSetTestPeer::FinalizeCurrent(&versions_);
  EXPECT_EQ(result.first, 1);
  EXPECT_GT(result.second, 1e12);
}

TEST_F(VersionSetTest, StartsWithEmptyVersionAndUnassignedPersistentNumbers) {
  ASSERT_NE(versions_.current(), nullptr);
  EXPECT_EQ(versions_.LastSequence(), 0U);
  EXPECT_EQ(versions_.LogNumber(), 0U);
  EXPECT_EQ(versions_.PrevLogNumber(), 0U);
  EXPECT_EQ(versions_.ManifestFileNumber(), 0U);
  for (int level = 0; level < config::kNumLevels; ++level) {
    EXPECT_EQ(versions_.NumLevelFiles(level), 0);
    EXPECT_EQ(versions_.NumLevelBytes(level), 0);
  }
}

TEST_F(VersionSetTest, AllocationAdvancesBeyondRecoveredFileNumbers) {
  versions_.MarkFileNumberUsed(1000);
  EXPECT_EQ(versions_.NewFileNumber(), 1001U);
  versions_.MarkFileNumberUsed(5);
  EXPECT_EQ(versions_.NewFileNumber(), 1002U);
  versions_.MarkFileNumberUsed(1ULL << 40);
  EXPECT_EQ(versions_.NewFileNumber(), (1ULL << 40) + 1);
}

TEST_F(VersionSetTest, ReusesOnlyTheLatestAllocatedNumber) {
  const uint64_t first = versions_.NewFileNumber();
  const uint64_t second = versions_.NewFileNumber();
  EXPECT_EQ(second, first + 1);
  versions_.ReuseFileNumber(first);
  EXPECT_EQ(versions_.NewFileNumber(), second + 1);
  const uint64_t latest = versions_.NewFileNumber();
  versions_.ReuseFileNumber(latest);
  EXPECT_EQ(versions_.NewFileNumber(), latest);
  EXPECT_EQ(versions_.NewFileNumber(), latest + 1);
}

TEST_F(VersionSetTest, ReuseCannotUndoRecoveryAdvancement) {
  const uint64_t first = versions_.NewFileNumber();
  versions_.MarkFileNumberUsed(1000);
  versions_.ReuseFileNumber(first);
  versions_.ReuseFileNumber(std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(versions_.NewFileNumber(), 1001U);
}

TEST_F(VersionSetTest, SequenceAdvancesIndependentlyAndAcceptsMaximum) {
  versions_.SetLastSequence(7);
  versions_.MarkFileNumberUsed(50);
  EXPECT_EQ(versions_.LastSequence(), 7U);
  EXPECT_EQ(versions_.NewFileNumber(), 51U);
  versions_.SetLastSequence(7);
  EXPECT_EQ(versions_.LastSequence(), 7U);
  versions_.SetLastSequence(kMaxSequenceNumber);
  EXPECT_EQ(versions_.LastSequence(), kMaxSequenceNumber);
}

TEST_F(VersionSetTest, QueriesTheCorrectStoredLogAndManifestNumber) {
  VersionSetTestPeer::SetStoredNumbers(&versions_, 100, 200, 300);
  EXPECT_EQ(versions_.LogNumber(), 100U);
  EXPECT_EQ(versions_.PrevLogNumber(), 200U);
  EXPECT_EQ(versions_.ManifestFileNumber(), 300U);
}

TEST_F(VersionSetTest, SumsOnlyCurrentVersionFilesAtRequestedLevel) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, 10, "a", "z"},
                   {0, 11, 1ULL << 40, "a", "z"},
                   {2, 20, 25, "a", "b"}, {2, 21, 50, "c", "d"}});
  EXPECT_EQ(versions_.NumLevelFiles(0), 2);
  EXPECT_EQ(versions_.NumLevelBytes(0), (1LL << 40) + 10);
  EXPECT_EQ(versions_.NumLevelFiles(1), 0);
  EXPECT_EQ(versions_.NumLevelBytes(1), 0);
  EXPECT_EQ(versions_.NumLevelFiles(2), 2);
  EXPECT_EQ(versions_.NumLevelBytes(2), 75);

  VersionSetTestPeer::InstallFiles(&versions_, {{1, 30, 100, "a", "b"}});
  EXPECT_EQ(versions_.NumLevelFiles(0), 0);
  EXPECT_EQ(versions_.NumLevelBytes(0), 0);
  EXPECT_EQ(versions_.NumLevelFiles(1), 1);
  EXPECT_EQ(versions_.NumLevelBytes(1), 100);
}

TEST_F(VersionSetTest, ByteTotalAcceptsLargestRepresentableResult) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
                    "a", "z"}});
  EXPECT_EQ(versions_.NumLevelBytes(0), std::numeric_limits<int64_t>::max());
}

TEST_F(VersionSetTest, LiveFilesIncludeRetainedOldVersionsWithoutClearingOutput) {
  VersionSetTestPeer::InstallFiles(&versions_, {{0, 10, 10, "a", "b"}});
  auto unref = [](Version* version) { version->Unref(); };
  Version* retained = versions_.current();
  retained->Ref();
  std::unique_ptr<Version, decltype(unref)> old(retained, unref);
  VersionSetTestPeer::InstallFiles(&versions_, {{1, 20, 20, "c", "d"}});

  std::set<uint64_t> live{999};
  versions_.AddLiveFiles(&live);
  EXPECT_EQ(live, (std::set<uint64_t>{10, 20, 999}));
  EXPECT_EQ(versions_.NumLevelFiles(0), 0);
  old.reset();
  live.clear();
  versions_.AddLiveFiles(&live);
  EXPECT_EQ(live, (std::set<uint64_t>{20}));
}

#ifndef NDEBUG
using VersionSetDeathTest = VersionSetTest;

TEST_F(VersionSetDeathTest, SequenceCannotMoveBackwardsOrExceed56Bits) {
  versions_.SetLastSequence(10);
  EXPECT_DEATH(versions_.SetLastSequence(9), "sequence");
  EXPECT_DEATH(versions_.SetLastSequence(kMaxSequenceNumber + 1), "sequence");
}

TEST_F(VersionSetDeathTest, ByteStatisticsRejectOutOfRangeLevels) {
  EXPECT_DEATH((void)versions_.NumLevelBytes(-1), "level");
  EXPECT_DEATH((void)versions_.NumLevelBytes(config::kNumLevels), "level");
}

TEST_F(VersionSetDeathTest, ByteStatisticsRejectUnrepresentableTotal) {
  VersionSetTestPeer::InstallFiles(
      &versions_, {{0, 10, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
                    "a", "b"}, {0, 11, 1, "c", "d"}});
  EXPECT_DEATH((void)versions_.NumLevelBytes(0), "file_size");
}

TEST_F(VersionSetDeathTest, FileNumbersMustNotWrapAround) {
  EXPECT_DEATH(versions_.MarkFileNumberUsed(std::numeric_limits<uint64_t>::max()),
               "number");
  versions_.MarkFileNumberUsed(std::numeric_limits<uint64_t>::max() - 1);
  EXPECT_DEATH((void)versions_.NewFileNumber(), "next_file_number_");
}

TEST_F(VersionSetDeathTest, LiveFileOutputMustNotBeNull) {
  EXPECT_DEATH(versions_.AddLiveFiles(nullptr), "live");
}
#endif

}  // namespace
}  // namespace LSMKV
