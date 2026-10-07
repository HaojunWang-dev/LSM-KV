#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <vector>

#include "VersionSet.h"

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
};

namespace {

class VersionSetTest : public ::testing::Test {
 protected:
  Options options_;
  InternalKeyComparator comparator_;
  VersionSet versions_{"metadata-test", &options_, nullptr, &comparator_};
};

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
