#include <array>
#include <cerrno>
#include <cstring>
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

// 新 WAL 必须截断旧内容；Flush 后数据应已提交给内核并可被其他文件描述符读取。
TEST_F(PosixWritableFileTest, FactoryTruncatesExistingFileAndFlushesAppendedData) {
  ASSERT_TRUE(WriteFile(path_, "stale-wal-data"));

  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewPosixWritableFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  EXPECT_TRUE(file->Append(Slice("new-record")).ok());
  EXPECT_TRUE(file->Flush().ok());
  EXPECT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), "new-record");
}

// 超过内部 64 KiB 缓冲区的数据会经历缓冲和非缓冲写入，字节序列不能丢失或重排。
TEST_F(PosixWritableFileTest, SyncPersistsPayloadThatCrossesTheInternalBuffer) {
  std::string payload(64 * 1024 + 37, 'x');
  payload[0] = 'A';
  payload[64 * 1024 - 1] = 'B';
  payload[64 * 1024] = '\0';
  payload.back() = 'Z';

  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewPosixWritableFile(path_, &file).ok());
  ASSERT_NE(file, nullptr);
  ASSERT_TRUE(file->Append(Slice(payload)).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(ReadFile(path_), payload);
}

// open() 因缺失父目录返回 ENOENT 时，工厂必须报告 NotFound 且不交付半初始化对象。
TEST(PosixWritableFileFactoryTest, MissingParentDirectoryReturnsNotFoundAndNoFile) {
  std::string missing_parent = CreateTemporaryPath();
  ASSERT_FALSE(missing_parent.empty());
  ASSERT_EQ(::unlink(missing_parent.c_str()), 0);

  std::unique_ptr<WritableFile> file;
  const Status status =
      NewPosixWritableFile(missing_parent + "/wal", &file);

  EXPECT_TRUE(status.IsNotFound());
  EXPECT_EQ(file, nullptr);
}

}  // namespace
}  // namespace LSMKV
