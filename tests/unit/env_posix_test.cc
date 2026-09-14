#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "env_posix.h"

namespace LSMKV {
namespace {

std::string CreateTemporaryPath() {
  std::array<char, 64> path{};
  std::strcpy(path.data(), "/tmp/lsm-kv-env-XXXXXX");
  const int fd = ::mkstemp(path.data());
  if (fd < 0) {
    return {};
  }
  ::close(fd);
  return path.data();
}

bool WriteFile(const std::string& path, const std::string& contents) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
  if (fd < 0) {
    return false;
  }

  const ssize_t written = ::write(fd, contents.data(), contents.size());
  const bool succeeded = written == static_cast<ssize_t>(contents.size());
  ::close(fd);
  return succeeded;
}

std::string ReadFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return {};
  }

  std::string contents;
  std::array<char, 4096> buffer;
  while (true) {
    const ssize_t read_size = ::read(fd, buffer.data(), buffer.size());
    if (read_size <= 0) {
      break;
    }
    contents.append(buffer.data(), static_cast<size_t>(read_size));
  }
  ::close(fd);
  return contents;
}

class PosixWritableFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = CreateTemporaryPath();
    ASSERT_FALSE(path_.empty()) << std::strerror(errno);
  }

  void TearDown() override {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }

  std::string path_;
};

class PosixSequentialFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = CreateTemporaryPath();
    ASSERT_FALSE(path_.empty()) << std::strerror(errno);
  }

  void TearDown() override {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }

  std::string path_;
};

TEST_F(PosixWritableFileTest, FactoryTruncatesExistingFileAndFlushesAppendedData) {
  ASSERT_TRUE(WriteFile(path_, "stale-wal-data"));

  PosixEnv env;
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env.NewWritableFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  EXPECT_TRUE(file->Append(Slice("new-record")).ok());
  EXPECT_TRUE(file->Flush().ok());
  EXPECT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), "new-record");
}

TEST_F(PosixWritableFileTest, SyncPersistsPayloadThatCrossesTheInternalBuffer) {
  std::string payload(64 * 1024 + 37, 'x');
  payload[0] = 'A';
  payload[64 * 1024 - 1] = 'B';
  payload[64 * 1024] = '\0';
  payload.back() = 'Z';

  PosixEnv env;
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env.NewWritableFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice(payload)).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), payload);
}

TEST_F(PosixWritableFileTest, OpensUsableFileThroughEnvInterface) {
  PosixEnv posix_env;
  Env* env = &posix_env;
  std::unique_ptr<WritableFile> file;

  ASSERT_TRUE(env->NewWritableFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice("virtual-record")).ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), "virtual-record");
}

TEST(PosixWritableFileFactoryTest, MissingParentDirectoryReturnsNotFoundAndNoFile) {
  std::string missing_parent = CreateTemporaryPath();
  ASSERT_FALSE(missing_parent.empty());
  ASSERT_EQ(::unlink(missing_parent.c_str()), 0);

  PosixEnv env;
  std::unique_ptr<WritableFile> file;
  const Status status =
      env.NewWritableFile(missing_parent + "/wal", &file);

  EXPECT_TRUE(status.IsNotFound());
  EXPECT_EQ(file, nullptr);
}

TEST_F(PosixSequentialFileTest, ReadsSequentiallyAndReturnsShortReadAtEnd) {
  ASSERT_TRUE(WriteFile(path_, "abcdef"));

  PosixEnv env;
  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(env.NewSequentialFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);

  std::array<char, 4> scratch{};
  Slice result;
  ASSERT_TRUE(file->Read(3, &result, scratch.data()).ok());
  EXPECT_EQ(result.data(), scratch.data());
  EXPECT_EQ(result.ToString(), "abc");

  ASSERT_TRUE(file->Read(scratch.size(), &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "def");

  ASSERT_TRUE(file->Read(scratch.size(), &result, scratch.data()).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(PosixSequentialFileTest, SkipChangesReadOffsetAndMayPassEndOfFile) {
  ASSERT_TRUE(WriteFile(path_, "abcdef"));

  PosixEnv env;
  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(env.NewSequentialFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Skip(2).ok());

  std::array<char, 4> scratch{};
  Slice result;
  ASSERT_TRUE(file->Read(2, &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "cd");

  ASSERT_TRUE(file->Skip(100).ok());
  ASSERT_TRUE(file->Read(scratch.size(), &result, scratch.data()).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(PosixSequentialFileTest,
       SkipRejectsUnrepresentableOffsetWithoutChangingPosition) {
  ASSERT_TRUE(WriteFile(path_, "abcdef"));

  PosixEnv env;
  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(env.NewSequentialFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);

  std::array<char, 2> scratch{};
  Slice result;
  ASSERT_TRUE(file->Read(1, &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "a");

  EXPECT_TRUE(file->Skip(std::numeric_limits<uint64_t>::max())
                  .IsInvalidArgument());

  ASSERT_TRUE(file->Read(1, &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "b");
}

TEST(PosixSequentialFileFactoryTest, MissingFileReturnsNotFoundAndNoFile) {
  std::string missing_path = CreateTemporaryPath();
  ASSERT_FALSE(missing_path.empty());
  ASSERT_EQ(::unlink(missing_path.c_str()), 0);

  PosixEnv env;
  std::unique_ptr<SequentialFile> file;
  const Status status = env.NewSequentialFile(missing_path, &file);

  EXPECT_TRUE(status.IsNotFound());
  EXPECT_EQ(file, nullptr);
  EXPECT_EQ(::access(missing_path.c_str(), F_OK), -1);
}

TEST(PosixEnvTest, FileExistsDistinguishesMissingAndExistingPaths) {
  PosixEnv env;
  const std::string path = CreateTemporaryPath();
  ASSERT_FALSE(path.empty());
  bool exists = false;
  ASSERT_TRUE(env.FileExists(path, &exists).ok());
  EXPECT_TRUE(exists);
  ASSERT_EQ(::unlink(path.c_str()), 0);
  ASSERT_TRUE(env.FileExists(path, &exists).ok());
  EXPECT_FALSE(exists);
}

TEST(PosixEnvTest, FileExistsPropagatesNotADirectoryError) {
  PosixEnv env;
  const std::string path = CreateTemporaryPath();
  ASSERT_FALSE(path.empty());

  bool exists = false;
  const Status status = env.FileExists(path + "/child", &exists);

  EXPECT_TRUE(status.IsIOError());
}

TEST(PosixEnvTest, CreateDirCreatesOneDirectory) {
  PosixEnv env;
  std::string path = CreateTemporaryPath();
  ASSERT_EQ(::unlink(path.c_str()), 0);
  ASSERT_TRUE(env.CreateDir(path).ok());
  bool exists = false;
  ASSERT_TRUE(env.FileExists(path, &exists).ok());
  EXPECT_TRUE(exists);
  ASSERT_EQ(::rmdir(path.c_str()), 0);
}

TEST(PosixEnvTest, CreateDirPropagatesAlreadyExistsError) {
  PosixEnv env;
  std::string path = CreateTemporaryPath();
  ASSERT_FALSE(path.empty());
  ASSERT_EQ(::unlink(path.c_str()), 0);
  ASSERT_TRUE(env.CreateDir(path).ok());

  const Status status = env.CreateDir(path);
  EXPECT_TRUE(status.IsIOError());

  ASSERT_EQ(::rmdir(path.c_str()), 0);
}

TEST(PosixEnvTest, DefaultReturnsStableEnvironment) {
  EXPECT_NE(Env::Default(), nullptr);
  EXPECT_EQ(Env::Default(), Env::Default());
}

}  // namespace
}  // namespace LSMKV
