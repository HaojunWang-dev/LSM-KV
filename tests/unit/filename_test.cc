#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "env_posix.h"
#include "filename.h"

namespace LSMKV {
namespace {

TEST(FilenameTest, GeneratesEachKindOfDatabasePath) {
  EXPECT_EQ(LogFileName("db", 7), "db/000007.log");
  EXPECT_EQ(TableFileName("db", 7), "db/000007.ldb");
  EXPECT_EQ(SSTTableFileName("db", 7), "db/000007.sst");
  EXPECT_EQ(DescriptorFileName("db", 7), "db/MANIFEST-000007");
  EXPECT_EQ(CurrentFileName("db"), "db/CURRENT");
  EXPECT_EQ(LockFileName("db"), "db/LOCK");
  EXPECT_EQ(TempFileName("db", 7), "db/000007.dbtmp");
  EXPECT_EQ(InfoLogFileName("db"), "db/LOG");
  EXPECT_EQ(OldInfoLogFileName("db"), "db/LOG.old");
}

TEST(FilenameTest, FormatsLargeNumbersWithoutTruncatingThem) {
  EXPECT_EQ(LogFileName("dir", 1000000), "dir/1000000.log");
  EXPECT_EQ(DescriptorFileName("dir", std::numeric_limits<uint64_t>::max()),
            "dir/MANIFEST-18446744073709551615");
  EXPECT_EQ(TableFileName("db/", 1), "db//000001.ldb");
}

TEST(FilenameTest, ParsesFixedNumberedAndLegacyNames) {
  struct Entry { const char* name; uint64_t number; FileType type; };
  const Entry entries[] = {
      {"CURRENT", 0, FileType::kCurrentFile}, {"LOCK", 0, FileType::kDBLockFile},
      {"LOG", 0, FileType::kInfoLogFile}, {"LOG.old", 0, FileType::kInfoLogFile},
      {"000007.log", 7, FileType::kLogFile}, {"7.ldb", 7, FileType::kTableFile},
      {"7.sst", 7, FileType::kTableFile}, {"000007.dbtmp", 7, FileType::kTempFile},
      {"MANIFEST-000007", 7, FileType::kDescriptorFile},
      {"0.log", 0, FileType::kLogFile}, {"MANIFEST-0", 0, FileType::kDescriptorFile},
      {"18446744073709551615.log", std::numeric_limits<uint64_t>::max(), FileType::kLogFile},
      {"MANIFEST-18446744073709551615", std::numeric_limits<uint64_t>::max(), FileType::kDescriptorFile}};
  for (const auto& entry : entries) {
    SCOPED_TRACE(entry.name);
    uint64_t number = 999;
    FileType type = FileType::kTempFile;
    ASSERT_TRUE(ParseFileName(entry.name, &number, &type));
    EXPECT_EQ(number, entry.number);
    EXPECT_EQ(type, entry.type);
  }
}

TEST(FilenameTest, RejectsMalformedNamesWithoutChangingOutputs) {
  const std::vector<std::string> invalid = {
      "", "current", "LOG.old.extra", "MANIFEST-", "MANIFEST-x", "MANIFEST-1.log",
      "MANIFEST-18446744073709551616", "18446744073709551616.log", "+1.log",
      "-1.log", "1", ".log", "1.LOG", "1.log.extra", "1.log\n", " 1.log",
      "db/000007.log", "/CURRENT", "../LOCK", "1/2.log",
      std::string("7.log\0extra", 11), std::string("CURRENT\0", 8)};
  for (const auto& name : invalid) {
    SCOPED_TRACE(name);
    uint64_t number = 999;
    FileType type = FileType::kTempFile;
    EXPECT_FALSE(ParseFileName(name, &number, &type));
    EXPECT_EQ(number, 999U);
    EXPECT_EQ(type, FileType::kTempFile);
  }
}

// 内存文件系统模拟真实的创建、部分写入和重命名副作用，用于注入 I/O 错误。
class FaultEnv final : public Env {
 public:
  std::map<std::string, std::string> files{{"db/CURRENT", "MANIFEST-000001\n"},
      {"db/MANIFEST-000001", "old metadata"}, {"db/MANIFEST-000007", "new metadata"}};
  std::set<std::string> failures;
  std::vector<std::string> events;
  int close_calls = 0;
  int directory_syncs = 0;
  bool file_alive = false;
  bool return_null_file = false;

  Status Step(const std::string& name) {
    events.push_back(name);
    return failures.count(name) ? Status::IOError(name) : Status::OK();
  }

  class File final : public WritableFile {
   public:
    File(FaultEnv* env, std::string name) : env_(env), name_(std::move(name)) {
      env_->file_alive = true;
    }
    ~File() override { env_->file_alive = false; }
    Status Append(const Slice& bytes) override {
      Status status = env_->Step("append");
      env_->files[name_].append(bytes.data(), status.ok() ? bytes.size() : 3);
      return status;
    }
    Status Flush() override { return env_->Step("flush"); }
    Status Sync() override { return env_->Step("sync"); }
    Status Close() override {
      ++env_->close_calls;
      return env_->Step("close");
    }
   private:
    FaultEnv* env_;  // 借用，Env 比它创建的文件存活更久。
    std::string name_;
  };

  Status NewWritableFile(const std::string& name,
                         WritableFile** result) override {
    *result = nullptr;
    EXPECT_EQ(name, "db/000007.dbtmp");
    Status status = Step("create");
    if (status.ok()) {
      files[name].clear();
      if (!return_null_file) *result = new File(this, name);
    }
    return status;
  }
  Status NewSequentialFile(const std::string&, SequentialFile** result) override {
    *result = nullptr;
    return Status::NotSupported("unused");
  }
  Status FileExists(const std::string& name, bool* exists) override {
    *exists = files.count(name) != 0;
    return Status::OK();
  }
  Status CreateDir(const std::string&) override { return Status::NotSupported("unused"); }
  Status RenameFile(const std::string& source, const std::string& target) override {
    EXPECT_EQ(source, "db/000007.dbtmp");
    EXPECT_EQ(target, "db/CURRENT");
    EXPECT_FALSE(file_alive);
    Status status = Step("rename");
    if (status.ok()) {
      files[target] = files.at(source);
      files.erase(source);
    }
    return status;
  }
  Status RemoveFile(const std::string& name) override {
    EXPECT_EQ(name, "db/000007.dbtmp");
    Status status = Step("remove");
    if (status.ok()) files.erase(name);
    return status;
  }
  Status SyncDir(const std::string& name) override {
    EXPECT_EQ(name, "db");
    return Step(++directory_syncs == 1 ? "dir_before" : "dir_after");
  }
};

TEST(FilenameTest, CurrentPublishesOnlyAfterFileAndDirectorySync) {
  FaultEnv env;
  ASSERT_TRUE(SetCurrentFile(&env, "db", 7).ok());
  EXPECT_EQ(env.files.at("db/CURRENT"), "MANIFEST-000007\n");
  EXPECT_EQ(env.files.count("db/000007.dbtmp"), 0U);
  EXPECT_EQ(env.files.at("db/MANIFEST-000001"), "old metadata");
  EXPECT_EQ(env.files.at("db/MANIFEST-000007"), "new metadata");
  EXPECT_EQ(env.close_calls, 1);
  EXPECT_FALSE(env.file_alive);
  EXPECT_EQ(env.events, (std::vector<std::string>{
      "create", "append", "sync", "close", "dir_before", "rename", "dir_after"}));
}

TEST(FilenameTest, CurrentPropagatesEveryCommitStageFailure) {
  struct Entry { const char* failure; bool renamed; std::vector<std::string> events; };
  const std::vector<Entry> entries = {
      {"create", false, {"create"}},
      {"append", false, {"create", "append", "close", "remove"}},
      {"sync", false, {"create", "append", "sync", "close", "remove"}},
      {"close", false, {"create", "append", "sync", "close", "remove"}},
      {"dir_before", false, {"create", "append", "sync", "close", "dir_before", "remove"}},
      {"rename", false, {"create", "append", "sync", "close", "dir_before", "rename", "remove"}},
      {"dir_after", true, {"create", "append", "sync", "close", "dir_before", "rename", "dir_after"}}};
  for (const auto& entry : entries) {
    SCOPED_TRACE(entry.failure);
    FaultEnv env;
    env.failures.insert(entry.failure);
    const Status status = SetCurrentFile(&env, "db", 7);
    EXPECT_TRUE(status.IsIOError());
    EXPECT_EQ(status.ToString(), std::string("IO error: ") + entry.failure);
    EXPECT_EQ(env.files.at("db/CURRENT"),
              entry.renamed ? "MANIFEST-000007\n" : "MANIFEST-000001\n");
    EXPECT_EQ(env.files.count("db/000007.dbtmp"), 0U);
    EXPECT_FALSE(env.file_alive);
    EXPECT_EQ(env.events, entry.events);
  }
}

TEST(FilenameTest, CurrentPreservesFirstErrorWhenCloseAndCleanupAlsoFail) {
  FaultEnv env;
  env.failures = {"append", "close", "remove"};
  const Status status = SetCurrentFile(&env, "db", 7);
  EXPECT_EQ(status.ToString(), "IO error: append");
  EXPECT_EQ(env.files.at("db/CURRENT"), "MANIFEST-000001\n");
  EXPECT_EQ(env.files.at("db/000007.dbtmp"), "MAN");
  EXPECT_EQ(env.close_calls, 1);
  EXPECT_FALSE(env.file_alive);
  EXPECT_EQ(env.events, (std::vector<std::string>{"create", "append", "close", "remove"}));
}

TEST(FilenameTest, CurrentRejectsInvalidArgumentsBeforeAnyFileOperation) {
  FaultEnv env;
  EXPECT_TRUE(SetCurrentFile(nullptr, "db", 7).IsInvalidArgument());
  EXPECT_TRUE(SetCurrentFile(&env, "db", 0).IsInvalidArgument());
  EXPECT_TRUE(SetCurrentFile(&env, "", 7).IsInvalidArgument());
  EXPECT_TRUE(SetCurrentFile(&env, std::string("db\0other", 8), 7).IsInvalidArgument());
  EXPECT_TRUE(env.events.empty());
}

TEST(FilenameTest, CurrentRejectsAFactoryReturningSuccessWithoutAFile) {
  FaultEnv env;
  env.return_null_file = true;
  const Status status = SetCurrentFile(&env, "db", 7);
  EXPECT_TRUE(status.IsIOError());
  EXPECT_NE(status.ToString().find("null WritableFile"), std::string::npos);
  EXPECT_EQ(env.files.at("db/CURRENT"), "MANIFEST-000001\n");
  EXPECT_EQ(env.files.count("db/000007.dbtmp"), 0U);
  EXPECT_EQ(env.events, (std::vector<std::string>{"create", "remove"}));
}

class PosixFileManagementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::array<char, 64> pattern{};
    const std::string prefix = "/tmp/lsm-kv-filename-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char* directory = ::mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    path_ = directory;
  }
  void TearDown() override {
    if (path_.empty()) return;
    for (const char* name : {"source", "target", "CURRENT", "MANIFEST-000007", "000007.dbtmp"}) {
      ::unlink((path_ + "/" + name).c_str());
    }
    ::rmdir(path_.c_str());
  }
  void Put(const std::string& name, const std::string& contents) {
    WritableFile* result = nullptr;
    const Status status = env_.NewWritableFile(path_ + "/" + name, &result);
    std::unique_ptr<WritableFile> file(result);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(file->Append(Slice(contents)).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }
  std::string Read(const std::string& name) {
    std::ifstream file(path_ + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  PosixEnv env_;
  std::string path_;
};

TEST_F(PosixFileManagementTest, RenameAtomicallyReplacesTargetAndRemovesSource) {
  ASSERT_NO_FATAL_FAILURE(Put("source", "new"));
  ASSERT_NO_FATAL_FAILURE(Put("target", "old"));
  Env* env = &env_;
  ASSERT_TRUE(env->RenameFile(path_ + "/source", path_ + "/target").ok());
  EXPECT_EQ(Read("target"), "new");
  bool exists = true;
  ASSERT_TRUE(env->FileExists(path_ + "/source", &exists).ok());
  EXPECT_FALSE(exists);
}

TEST_F(PosixFileManagementTest, RenameErrorsLeaveExistingTargetUnchanged) {
  ASSERT_NO_FATAL_FAILURE(Put("target", "old"));
  EXPECT_TRUE(env_.RenameFile(path_ + "/missing", path_ + "/target").IsNotFound());
  EXPECT_EQ(Read("target"), "old");
  ASSERT_NO_FATAL_FAILURE(Put("source", "new"));
  EXPECT_TRUE(env_.RenameFile(path_ + "/source", path_).IsIOError());
  EXPECT_EQ(Read("source"), "new");
}

TEST_F(PosixFileManagementTest, RemoveUnlinksFilesAndReportsMissingFilesAndDirectories) {
  ASSERT_NO_FATAL_FAILURE(Put("source", "bytes"));
  ASSERT_TRUE(env_.RemoveFile(path_ + "/source").ok());
  EXPECT_TRUE(env_.RemoveFile(path_ + "/source").IsNotFound());
  EXPECT_TRUE(env_.RemoveFile(path_).IsIOError());
}

TEST_F(PosixFileManagementTest, DirectorySyncRequiresAnExistingDirectory) {
  EXPECT_TRUE(env_.SyncDir(path_).ok());
  EXPECT_TRUE(env_.SyncDir(path_ + "/missing").IsNotFound());
  ASSERT_NO_FATAL_FAILURE(Put("source", "bytes"));
  EXPECT_TRUE(env_.SyncDir(path_ + "/source").IsIOError());
}

TEST_F(PosixFileManagementTest, FileOperationsRejectEmbeddedNulPaths) {
  ASSERT_NO_FATAL_FAILURE(Put("source", "bytes"));
  const std::string nul(1, '\0');
  EXPECT_TRUE(env_.RemoveFile(path_ + "/source" + nul + "suffix").IsInvalidArgument());
  EXPECT_TRUE(env_.RenameFile(path_ + "/source" + nul, path_ + "/target").IsInvalidArgument());
  EXPECT_TRUE(env_.RenameFile(path_ + "/source", path_ + "/target" + nul).IsInvalidArgument());
  EXPECT_TRUE(env_.SyncDir(path_ + nul).IsInvalidArgument());
  EXPECT_EQ(Read("source"), "bytes");
}

TEST_F(PosixFileManagementTest, CurrentReplacementWritesOnlyManifestBasenameAndNewline) {
  ASSERT_NO_FATAL_FAILURE(Put("CURRENT", "MANIFEST-000001\n"));
  ASSERT_NO_FATAL_FAILURE(Put("MANIFEST-000007", "metadata"));
  ASSERT_TRUE(SetCurrentFile(&env_, path_, 7).ok());
  EXPECT_EQ(Read("CURRENT"), "MANIFEST-000007\n");
  EXPECT_EQ(Read("MANIFEST-000007"), "metadata");
  bool exists = true;
  ASSERT_TRUE(env_.FileExists(path_ + "/000007.dbtmp", &exists).ok());
  EXPECT_FALSE(exists);
}

#ifndef NDEBUG
TEST(FilenameDeathTest, NumberedGeneratorsRequireNonzeroNumbers) {
  using Generator = std::string (*)(const std::string&, uint64_t);
  for (Generator generate : {LogFileName, TableFileName, SSTTableFileName,
                             DescriptorFileName, TempFileName}) {
    EXPECT_DEATH((void)generate("db", 0), "number");
  }
}
#endif

}  // namespace
}  // namespace LSMKV
