#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "env_posix.h"

namespace LSMKV {
namespace {

class RandomAccessFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/lsmkv-random-XXXXXX";
    const int fd = ::mkstemp(pattern);
    ASSERT_GE(fd, 0);
    path_ = pattern;
    ASSERT_EQ(::close(fd), 0);
  }
  void TearDown() override {
    file_.reset();
    if (!directory_.empty()) ::rmdir(directory_.c_str());
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  void OpenWithContents(const std::string& contents) {
    WritableFile* output = nullptr;
    ASSERT_TRUE(env_.NewWritableFile(path_, &output).ok());
    std::unique_ptr<WritableFile> writer(output);
    ASSERT_TRUE(writer->Append(contents).ok());
    ASSERT_TRUE(writer->Close().ok());
    writer.reset();
    RandomAccessFile* input = nullptr;
    Env* env = &env_;
    const Status status = env->NewRandomAccessFile(path_, &input);
    file_.reset(input);
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_NE(file_, nullptr);
  }
  PosixEnv env_;
  std::string path_;
  std::string directory_;
  std::unique_ptr<RandomAccessFile> file_;
};

TEST_F(RandomAccessFileTest, ReadsBinaryBytesAtRequestedOffsetsThroughConstInterface) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents(std::string("ab\0cdefghijk", 12)));
  const RandomAccessFile* file = file_.get();
  struct Request { uint64_t offset; size_t count; std::string expected; };
  const Request requests[] = {{8, 3, "hij"}, {0, 4, std::string("ab\0c", 4)},
                              {3, 5, "cdefg"}, {2, 1, std::string(1, '\0')}, {0, 2, "ab"}};
  std::array<char, 8> scratch{};
  for (const auto& request : requests) {
    SCOPED_TRACE(request.offset);
    Slice result("stale");
    const Status status = file->Read(request.offset, request.count, &result, scratch.data());
    ASSERT_TRUE(status.ok()) << status.ToString();
    EXPECT_EQ(result.data(), scratch.data());
    EXPECT_EQ(result.ToString(), request.expected);
  }
}

TEST_F(RandomAccessFileTest, ShortReadAndPastEndAreSuccessful) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  std::array<char, 4> scratch{};
  Slice result("stale");
  ASSERT_TRUE(file_->Read(4, scratch.size(), &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "ef");
  for (uint64_t offset : {uint64_t{6}, uint64_t{100}}) {
    result = Slice("stale");
    EXPECT_TRUE(file_->Read(offset, scratch.size(), &result, scratch.data()).ok());
    EXPECT_TRUE(result.empty());
  }
}

TEST_F(RandomAccessFileTest, EmptyFileAndZeroLengthReadClearOldResult) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents(""));
  Slice result("stale");
  EXPECT_TRUE(file_->Read(0, 0, &result, nullptr).ok());
  EXPECT_TRUE(result.empty());
  std::array<char, 4> scratch{};
  result = Slice("stale");
  EXPECT_TRUE(file_->Read(0, scratch.size(), &result, scratch.data()).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(RandomAccessFileTest, RejectsNullResultAndBufferForNonemptyRead) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  char scratch = 'x';
  EXPECT_TRUE(file_->Read(0, 1, nullptr, &scratch).IsInvalidArgument());
  Slice result("stale");
  EXPECT_TRUE(file_->Read(0, 1, &result, nullptr).IsInvalidArgument());
  EXPECT_TRUE(result.empty());
}

TEST_F(RandomAccessFileTest, RejectsUnrepresentableOffsetsLengthsAndRanges) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
  char scratch = 'x';
  struct Request { uint64_t offset; size_t count; };
  const Request invalid[] = {{std::numeric_limits<uint64_t>::max(), 1},
                             {maximum - 1, 2}, {0, std::numeric_limits<size_t>::max()}};
  for (const auto& request : invalid) {
    SCOPED_TRACE(request.offset);
    Slice result("stale");
    EXPECT_TRUE(file_->Read(request.offset, request.count, &result, &scratch).IsInvalidArgument());
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(scratch, 'x');
  }
  Slice result("stale");
  EXPECT_TRUE(file_->Read(maximum, 0, &result, nullptr).ok());
  EXPECT_TRUE(result.empty());
  EXPECT_TRUE(file_->Read(maximum - 1, 1, &result, &scratch).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(RandomAccessFileTest, FactoryRejectsNullOutputAndEmbeddedNulPath) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("unchanged"));
  EXPECT_TRUE(env_.NewRandomAccessFile(path_, nullptr).IsInvalidArgument());
  RandomAccessFile* result = nullptr;
  EXPECT_TRUE(env_.NewRandomAccessFile(path_ + '\0' + "ignored", &result).IsInvalidArgument());
  EXPECT_EQ(result, nullptr);
}

TEST_F(RandomAccessFileTest, MissingFileReturnsNotFoundWithoutCreatingIt) {
  const std::string missing = path_ + "-missing";
  RandomAccessFile* result = nullptr;
  EXPECT_TRUE(env_.NewRandomAccessFile(missing, &result).IsNotFound());
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(::access(missing.c_str(), F_OK), -1);
}

TEST_F(RandomAccessFileTest, FactoryFailureClearsOutputWithoutDeletingPriorObject) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abc"));
  RandomAccessFile* result = file_.get();  // 输出槽借用旧对象；file_ 仍拥有它。
  EXPECT_TRUE(env_.NewRandomAccessFile(path_ + "/child", &result).IsIOError());
  EXPECT_EQ(result, nullptr);
  std::array<char, 3> scratch{};
  Slice read;
  ASSERT_TRUE(file_->Read(0, scratch.size(), &read, scratch.data()).ok());
  EXPECT_EQ(read.ToString(), "abc");
}

TEST_F(RandomAccessFileTest, ReturnedViewsBorrowIndependentCallerBuffers) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  std::array<char, 3> first_scratch{};
  std::array<char, 3> second_scratch{};
  Slice first;
  Slice second;
  ASSERT_TRUE(file_->Read(0, 3, &first, first_scratch.data()).ok());
  ASSERT_TRUE(file_->Read(3, 3, &second, second_scratch.data()).ok());
  file_.reset();
  EXPECT_EQ(first.data(), first_scratch.data());
  EXPECT_EQ(second.data(), second_scratch.data());
  EXPECT_EQ(first.ToString(), "abc");
  EXPECT_EQ(second.ToString(), "def");
}

TEST_F(RandomAccessFileTest, ConcurrentReadsUseIndependentOffsetsAndBuffers) {
  const std::array<std::string, 4> chunks = {"ABCDEFGH", "abcdefgh", "01234567", "!@#$%^&*"};
  ASSERT_NO_FATAL_FAILURE(OpenWithContents(chunks[0] + chunks[1] + chunks[2] + chunks[3]));
  const RandomAccessFile* file = file_.get();
  std::vector<std::thread> readers;
  for (size_t worker = 0; worker < chunks.size(); ++worker) {
    readers.emplace_back([&, worker] {
      std::array<char, 8> scratch{};
      for (int iteration = 0; iteration < 256; ++iteration) {
        Slice result;
        const Status status = file->Read(worker * 8, scratch.size(), &result, scratch.data());
        ASSERT_TRUE(status.ok()) << status.ToString();
        EXPECT_EQ(result.ToString(), chunks[worker]);
      }
    });
  }
  for (auto& reader : readers) reader.join();
}

#if defined(__linux__)
// 只用于检测本测试文件的游标和句柄生命周期，不为生产对象增加 fd 访问接口。
int FindOpenFileDescriptor(const std::string& path) {
  struct stat expected;
  if (::stat(path.c_str(), &expected) != 0) return -1;
  auto close_dir = [](DIR* directory) { ::closedir(directory); };
  std::unique_ptr<DIR, decltype(close_dir)> directory(::opendir("/proc/self/fd"), close_dir);
  if (directory == nullptr) return -1;
  while (dirent* entry = ::readdir(directory.get())) {
    char* end = nullptr;
    const long candidate = std::strtol(entry->d_name, &end, 10);
    if (*end != '\0' || candidate < 0 || candidate > std::numeric_limits<int>::max()) continue;
    struct stat actual;
    if (::fstat(static_cast<int>(candidate), &actual) == 0 &&
        actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino) return static_cast<int>(candidate);
  }
  return -1;
}

TEST_F(RandomAccessFileTest, ReadingDoesNotChangeDescriptorCursor) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  const int fd = FindOpenFileDescriptor(path_);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::lseek(fd, 2, SEEK_SET), 2);
  std::array<char, 2> scratch{};
  Slice result;
  ASSERT_TRUE(file_->Read(4, scratch.size(), &result, scratch.data()).ok());
  EXPECT_EQ(result.ToString(), "ef");
  EXPECT_EQ(::lseek(fd, 0, SEEK_CUR), 2);
}

TEST_F(RandomAccessFileTest, BasePointerDestructionClosesDescriptor) {
  ASSERT_NO_FATAL_FAILURE(OpenWithContents("abcdef"));
  const int fd = FindOpenFileDescriptor(path_);
  ASSERT_GE(fd, 0);
  file_.reset();
  errno = 0;
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST_F(RandomAccessFileTest, ReadIoErrorClearsOldResult) {
  directory_ = path_ + "-dir";
  ASSERT_EQ(::mkdir(directory_.c_str(), 0700), 0);
  RandomAccessFile* result = nullptr;
  ASSERT_TRUE(env_.NewRandomAccessFile(directory_, &result).ok());
  std::unique_ptr<RandomAccessFile> directory(result);
  std::array<char, 4> scratch{};
  Slice read("stale");
  EXPECT_TRUE(directory->Read(0, scratch.size(), &read, scratch.data()).IsIOError());
  EXPECT_TRUE(read.empty());
}
#endif

}  // namespace
}  // namespace LSMKV
