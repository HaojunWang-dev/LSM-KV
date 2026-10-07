#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

#include <fcntl.h>
#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
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

TEST_F(PosixSequentialFileTest, ReadFileToStringReadsWholeBinaryFileAcrossBuffers) {
  PosixEnv env;
  for (size_t size : {size_t{0}, size_t{8192}, size_t{10003}}) {
    SCOPED_TRACE(size);
    std::string contents(size, 'x');
    if (size != 0) {
      contents[0] = '\0';
      contents[4096] = '\0';
      contents.back() = 'z';
    }
    ASSERT_TRUE(WriteFile(path_, contents));
    std::string output = "stale bytes";
    const Status status = ReadFileToString(&env, path_, &output);
    ASSERT_TRUE(status.ok()) << status.ToString();
    EXPECT_EQ(output, contents);
  }
}

TEST_F(PosixSequentialFileTest, ReadFileToStringReportsMissingFile) {
  PosixEnv env;
  std::string output = "stale bytes";
  EXPECT_TRUE(ReadFileToString(&env, path_ + "-missing", &output).IsNotFound());
  EXPECT_TRUE(output.empty());
}

TEST_F(PosixWritableFileTest, FactoryTruncatesExistingFileAndFlushesAppendedData) {
  ASSERT_TRUE(WriteFile(path_, "stale-wal-data"));

  PosixEnv env;
  WritableFile* result = nullptr;
  const Status status = env.NewWritableFile(path_, &result);
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(status.ok());
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
  WritableFile* result = nullptr;
  const Status status = env.NewWritableFile(path_, &result);
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(status.ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice(payload)).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), payload);
}

TEST_F(PosixWritableFileTest, OpensUsableFileThroughEnvInterface) {
  PosixEnv posix_env;
  Env* env = &posix_env;
  WritableFile* result = nullptr;
  const Status status = env->NewWritableFile(path_, &result);
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(status.ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice("virtual-record")).ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), "virtual-record");
}

TEST_F(PosixWritableFileTest, RawFactoryTransfersOwnershipToCaller) {
  PosixEnv posix_env;
  Env* env = &posix_env;
  WritableFile* result = nullptr;
  const Status status = env->NewWritableFile(path_, &result);
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(status.ok());
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->Append(Slice("caller-owned")).ok());
  ASSERT_TRUE(result->Sync().ok());
  file.reset();
  EXPECT_EQ(ReadFile(path_), "caller-owned");
}

TEST_F(PosixWritableFileTest, AppendableFilePreservesExistingBinaryContents) {
  ASSERT_TRUE(WriteFile(path_, std::string("old\0", 4)));
  PosixEnv posix;
  Env* env = &posix;
  WritableFile* result = nullptr;
  ASSERT_TRUE(env->NewAppendableFile(path_, &result).ok());
  std::unique_ptr<WritableFile> file(result);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(ReadFile(path_), std::string("old\0", 4));
  ASSERT_TRUE(file->Append(Slice("new\0", 4)).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  EXPECT_EQ(ReadFile(path_), std::string("old\0new\0", 8));
  uint64_t size = 999;
  ASSERT_TRUE(env->GetFileSize(path_, &size).ok());
  EXPECT_EQ(size, 8U);
}

TEST_F(PosixWritableFileTest, AppendableFileCreatesMissingFileWithoutTruncation) {
  ASSERT_EQ(::unlink(path_.c_str()), 0);
  PosixEnv env;
  WritableFile* result = nullptr;
  ASSERT_TRUE(env.NewAppendableFile(path_, &result).ok());
  std::unique_ptr<WritableFile> file(result);
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice("created")).ok());
  ASSERT_TRUE(file->Close().ok());
  EXPECT_EQ(ReadFile(path_), "created");
}

TEST_F(PosixWritableFileTest, AppendableFailureClearsOutputWithoutDeletingOldObject) {
  PosixEnv env;
  WritableFile* original = nullptr;
  ASSERT_TRUE(env.NewWritableFile(path_, &original).ok());
  std::unique_ptr<WritableFile> owner(original);
  WritableFile* output = original;
  EXPECT_TRUE(env.NewAppendableFile(path_ + "/child", &output).IsIOError());
  EXPECT_EQ(output, nullptr);
  ASSERT_TRUE(owner->Append(Slice("still-owned")).ok());
  ASSERT_TRUE(owner->Sync().ok());
  EXPECT_EQ(ReadFile(path_), "still-owned");
}

TEST_F(PosixWritableFileTest, NewFileInterfacesRejectNullOutputsAndNulPaths) {
  PosixEnv env;
  ASSERT_TRUE(WriteFile(path_, "unchanged"));
  EXPECT_TRUE(env.NewAppendableFile(path_, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env.GetFileSize(path_, nullptr).IsInvalidArgument());
  WritableFile* file = nullptr;
  const std::string bad_path = path_ + std::string("\0suffix", 7);
  EXPECT_TRUE(env.NewAppendableFile(bad_path, &file).IsInvalidArgument());
  EXPECT_EQ(file, nullptr);
  uint64_t size = 999;
  EXPECT_TRUE(env.GetFileSize(bad_path, &size).IsInvalidArgument());
  EXPECT_EQ(size, 0U);
  EXPECT_TRUE(env.GetFileSize(path_ + "-missing", &size).IsNotFound());
  EXPECT_EQ(size, 0U);
  EXPECT_EQ(ReadFile(path_), "unchanged");
}

TEST_F(PosixWritableFileTest, RawFactoryFailureClearsOutputWithoutDeletingPriorObject) {
  PosixEnv env;
  WritableFile* original = nullptr;
  const Status created = env.NewWritableFile(path_, &original);
  std::unique_ptr<WritableFile> owner(original);
  ASSERT_TRUE(created.ok());
  WritableFile* output = original;  // 借用已有对象，owner 仍保留其所有权。
  const Status failed = env.NewWritableFile(path_ + "/child", &output);
  EXPECT_TRUE(failed.IsIOError());
  EXPECT_EQ(output, nullptr);
  ASSERT_TRUE(owner->Append(Slice("still-owned")).ok());
  ASSERT_TRUE(owner->Sync().ok());
  owner.reset();
  EXPECT_EQ(ReadFile(path_), "still-owned");
}

#if defined(__linux__)
TEST_F(PosixWritableFileTest, FailedCloseCannotCloseAReusedDescriptorOnDestruction) {
  PosixEnv env;
  WritableFile* result = nullptr;
  const Status status = env.NewWritableFile(path_, &result);
  std::unique_ptr<WritableFile> file(result);
  ASSERT_TRUE(status.ok());

  // 对真实文件注入 EBADF：识别其句柄并使内核将它释放，模拟 close 失败。
  struct stat expected;
  ASSERT_EQ(::stat(path_.c_str(), &expected), 0);
  auto close_dir = [](DIR* dir) { ::closedir(dir); };
  std::unique_ptr<DIR, decltype(close_dir)> directory(::opendir("/proc/self/fd"), close_dir);
  ASSERT_NE(directory, nullptr);
  int file_fd = -1;
  while (dirent* entry = ::readdir(directory.get())) {
    char* end = nullptr;
    const long candidate = std::strtol(entry->d_name, &end, 10);
    if (*end != '\0' || candidate < 0 || candidate > std::numeric_limits<int>::max()) continue;
    struct stat actual;
    if (::fstat(static_cast<int>(candidate), &actual) == 0 &&
        actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino) {
      file_fd = static_cast<int>(candidate);
      break;
    }
  }
  directory.reset();
  ASSERT_GE(file_fd, 0);
  ASSERT_EQ(::close(file_fd), 0);
  EXPECT_TRUE(file->Close().IsIOError());

  const int replacement = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(replacement, 0);
  EXPECT_EQ(replacement, file_fd);
  file.reset();
  EXPECT_NE(::fcntl(replacement, F_GETFD), -1);
  ::close(replacement);
}
#endif

TEST(PosixWritableFileFactoryTest, MissingParentDirectoryReturnsNotFoundAndNoFile) {
  std::string missing_parent = CreateTemporaryPath();
  ASSERT_FALSE(missing_parent.empty());
  ASSERT_EQ(::unlink(missing_parent.c_str()), 0);

  PosixEnv env;
  WritableFile* result = nullptr;
  const Status status =
      env.NewWritableFile(missing_parent + "/wal", &result);
  std::unique_ptr<WritableFile> file(result);

  EXPECT_TRUE(status.IsNotFound());
  EXPECT_EQ(file, nullptr);
}

TEST_F(PosixSequentialFileTest, ReadsSequentiallyAndReturnsShortReadAtEnd) {
  ASSERT_TRUE(WriteFile(path_, "abcdef"));

  PosixEnv env;
  SequentialFile* created = nullptr;
  const Status status = env.NewSequentialFile(path_, &created);
  std::unique_ptr<SequentialFile> file(created);
  ASSERT_TRUE(status.ok());
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

TEST_F(PosixSequentialFileTest, RawFactoryTransfersReadableObjectToCaller) {
  ASSERT_TRUE(WriteFile(path_, std::string("a\0b", 3)));
  PosixEnv posix_env;
  Env* env = &posix_env;
  SequentialFile* result = nullptr;
  const Status status = env->NewSequentialFile(path_, &result);
  std::unique_ptr<SequentialFile> file(result);
  ASSERT_TRUE(status.ok());
  ASSERT_NE(result, nullptr);
  std::array<char, 3> scratch{};
  Slice read;
  ASSERT_TRUE(result->Read(scratch.size(), &read, scratch.data()).ok());
  file.reset();
  EXPECT_EQ(read.ToString(), std::string("a\0b", 3));
}

TEST_F(PosixSequentialFileTest, RawFactoryFailureClearsOutputWithoutDeletingPriorObject) {
  ASSERT_TRUE(WriteFile(path_, "abc"));
  PosixEnv env;
  SequentialFile* original = nullptr;
  const Status created = env.NewSequentialFile(path_, &original);
  std::unique_ptr<SequentialFile> owner(original);
  ASSERT_TRUE(created.ok());
  SequentialFile* output = original;  // 只借用原对象；owner 保留所有权。
  const Status failed = env.NewSequentialFile(path_ + "/child", &output);
  EXPECT_TRUE(failed.IsIOError());
  EXPECT_EQ(output, nullptr);
  std::array<char, 3> scratch{};
  Slice read;
  ASSERT_TRUE(owner->Read(scratch.size(), &read, scratch.data()).ok());
  EXPECT_EQ(read.ToString(), "abc");
}

TEST_F(PosixSequentialFileTest, SkipChangesReadOffsetAndMayPassEndOfFile) {
  ASSERT_TRUE(WriteFile(path_, "abcdef"));

  PosixEnv env;
  SequentialFile* created = nullptr;
  const Status status = env.NewSequentialFile(path_, &created);
  std::unique_ptr<SequentialFile> file(created);
  ASSERT_TRUE(status.ok());
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
  SequentialFile* created = nullptr;
  const Status status = env.NewSequentialFile(path_, &created);
  std::unique_ptr<SequentialFile> file(created);
  ASSERT_TRUE(status.ok());
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
  SequentialFile* created = nullptr;
  const Status status = env.NewSequentialFile(missing_path, &created);
  std::unique_ptr<SequentialFile> file(created);

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
