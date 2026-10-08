#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "env.h"
#include "options.h"

namespace LSMKV {
namespace {

class RecordingWritableFile final : public WritableFile {
 public:
  explicit RecordingWritableFile(bool* destroyed) : destroyed_(destroyed) {}

  ~RecordingWritableFile() override { *destroyed_ = true; }

  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }

  Status Close() override {
    closed_ = true;
    return Status::OK();
  }

  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }

  const std::string& contents() const { return contents_; }
  bool closed() const { return closed_; }

 private:
  bool* destroyed_;
  std::string contents_;
  bool closed_ = false;
};

class RecordingEnv final : public Env {
 public:
  std::unique_ptr<SequentialFile> sequential_file;
  Status open_status = Status::NotSupported("unused");
  std::string opened_filename;

  Status NewWritableFile(const std::string&,
                         WritableFile** result) override {
    *result = nullptr;
    return Status::NotSupported("unused");
  }
  Status NewSequentialFile(const std::string& filename,
                           SequentialFile** result) override {
    opened_filename = filename;
    *result = open_status.ok() ? sequential_file.release() : nullptr;
    return open_status;
  }
  Status FileExists(const std::string&, bool* exists) override {
    *exists = true;
    return Status::OK();
  }
  Status CreateDir(const std::string&) override { return Status::OK(); }
  Status RenameFile(const std::string&, const std::string&) override {
    return Status::NotSupported("unused");
  }
  Status RemoveFile(const std::string&) override { return Status::NotSupported("unused"); }
  Status SyncDir(const std::string&) override { return Status::NotSupported("unused"); }
};

// 注入短读和 I/O 错误；每次读取覆盖 scratch，检测调用方是否及时复制字节。
class ChunkedSequentialFile final : public SequentialFile {
 public:
  ChunkedSequentialFile(std::string contents, size_t chunk_size,
                        size_t fail_at, bool* destroyed)
      : contents_(std::move(contents)), chunk_size_(chunk_size),
        fail_at_(fail_at), destroyed_(destroyed) {}
  ~ChunkedSequentialFile() override { *destroyed_ = true; }

  Status Read(size_t n, Slice* result, char* scratch) override {
    if (offset_ >= fail_at_) {
      *result = Slice("discard on error");
      return Status::IOError("injected read failure");
    }
    const size_t length = std::min({n, chunk_size_, contents_.size() - offset_});
    std::memcpy(scratch, contents_.data() + offset_, length);
    offset_ += length;
    *result = Slice(scratch, length);
    return Status::OK();
  }
  Status Skip(uint64_t) override { return Status::NotSupported("unused"); }

 private:
  std::string contents_;
  size_t chunk_size_;
  size_t fail_at_;
  bool* destroyed_;  // 借用，测试标记覆盖文件生命周期。
  size_t offset_ = 0;
};

TEST(EnvTest, ReadFileToStringContinuesAfterShortReadsAndCopiesBinaryBytes) {
  bool destroyed = false;
  RecordingEnv env;
  env.open_status = Status::OK();
  env.sequential_file = std::make_unique<ChunkedSequentialFile>(
      std::string("a\0b\nc\0d", 7), 2, 100, &destroyed);
  std::string output = "stale bytes";
  const Status status = ReadFileToString(&env, "db/CURRENT", &output);
  ASSERT_TRUE(status.ok()) << status.ToString();
  EXPECT_EQ(output, std::string("a\0b\nc\0d", 7));
  EXPECT_EQ(env.opened_filename, "db/CURRENT");
  EXPECT_TRUE(destroyed);
}

TEST(EnvTest, ReadFileToStringReturnsEmptyContentsForEmptyFile) {
  bool destroyed = false;
  RecordingEnv env;
  env.open_status = Status::OK();
  env.sequential_file = std::make_unique<ChunkedSequentialFile>("", 2, 100, &destroyed);
  std::string output = "stale bytes";
  EXPECT_TRUE(ReadFileToString(&env, "empty", &output).ok());
  EXPECT_TRUE(output.empty());
  EXPECT_TRUE(destroyed);
}

TEST(EnvTest, ReadFileToStringPropagatesOpenErrorAndClearsOldOutput) {
  RecordingEnv env;
  env.open_status = Status::NotFound("missing CURRENT");
  std::string output = "stale bytes";
  const Status status = ReadFileToString(&env, "missing", &output);
  EXPECT_TRUE(status.IsNotFound());
  EXPECT_EQ(status.ToString(), env.open_status.ToString());
  EXPECT_TRUE(output.empty());
}

TEST(EnvTest, ReadFileToStringPreservesSuccessfulPrefixAndReleasesFileOnReadError) {
  for (size_t fail_at : {size_t{0}, size_t{4}}) {
    SCOPED_TRACE(fail_at);
    bool destroyed = false;
    RecordingEnv env;
    env.open_status = Status::OK();
    env.sequential_file = std::make_unique<ChunkedSequentialFile>(
        "abcdefgh", 2, fail_at, &destroyed);
    std::string output = "stale bytes";
    const Status status = ReadFileToString(&env, "broken", &output);
    EXPECT_TRUE(status.IsIOError());
    EXPECT_EQ(status.ToString(), Status::IOError("injected read failure").ToString());
    EXPECT_EQ(output, fail_at == 0 ? "" : "abcd");
    EXPECT_TRUE(destroyed);
  }
}

TEST(EnvTest, ReadFileToStringRejectsNullFileReturnedOnSuccess) {
  RecordingEnv env;
  env.open_status = Status::OK();
  std::string output = "stale bytes";
  EXPECT_TRUE(ReadFileToString(&env, "broken factory", &output).IsIOError());
  EXPECT_TRUE(output.empty());
}

TEST(EnvTest, ReadFileToStringRejectsNullArguments) {
  RecordingEnv env;
  std::string output = "stale bytes";
  EXPECT_TRUE(ReadFileToString(nullptr, "file", &output).IsInvalidArgument());
  EXPECT_TRUE(output.empty());
  EXPECT_TRUE(ReadFileToString(&env, "file", nullptr).IsInvalidArgument());
  EXPECT_TRUE(env.opened_filename.empty());
}

TEST(EnvTest, FileExistsUsesStatusAndOutputParameter) {
  RecordingEnv env;
  bool exists = false;
  ASSERT_TRUE(env.FileExists("ignored", &exists).ok());
  EXPECT_TRUE(exists);
}

TEST(EnvTest, OptionsBorrowTheDefaultEnvironment) {
  Options options;
  EXPECT_EQ(options.env, Env::Default());
  EXPECT_TRUE(options.create_if_missing);
  EXPECT_FALSE(options.error_if_exists);
}

TEST(EnvTest, UnsupportedManifestReuseInterfacesClearOutputs) {
  RecordingEnv recording;
  Env* env = &recording;
  bool destroyed = false;
  auto owner = std::make_unique<RecordingWritableFile>(&destroyed);
  WritableFile* file = owner.get();
  EXPECT_TRUE(env->NewAppendableFile("manifest", &file).IsNotSupportedError());
  EXPECT_EQ(file, nullptr);
  EXPECT_FALSE(destroyed);
  uint64_t size = 999;
  EXPECT_TRUE(env->GetFileSize("manifest", &size).IsNotSupportedError());
  EXPECT_EQ(size, 0U);
  EXPECT_TRUE(env->NewAppendableFile("manifest", nullptr).IsInvalidArgument());
  EXPECT_TRUE(env->GetFileSize("manifest", nullptr).IsInvalidArgument());
}

class RecordingRandomAccessFile final : public RandomAccessFile {
 public:
  explicit RecordingRandomAccessFile(bool* destroyed) : destroyed_(destroyed) {}
  ~RecordingRandomAccessFile() override { *destroyed_ = true; }
  Status Read(uint64_t, size_t, Slice* result, char*) const override {
    result->clear();
    return Status::NotSupported("unused");
  }
 private:
  bool* destroyed_;  // 借用，测试标记覆盖文件生命周期。
};

TEST(EnvTest, UnsupportedRandomAccessFactoryDoesNotTakeOwnershipOfOldOutput) {
  RecordingEnv recording;
  Env* env = &recording;
  bool destroyed = false;
  std::unique_ptr<RandomAccessFile> owner(new RecordingRandomAccessFile(&destroyed));
  RandomAccessFile* output = owner.get();
  EXPECT_TRUE(env->NewRandomAccessFile("table", &output).IsNotSupportedError());
  EXPECT_EQ(output, nullptr);
  EXPECT_FALSE(destroyed);
  EXPECT_TRUE(env->NewRandomAccessFile("table", nullptr).IsInvalidArgument());
  owner.reset();
  EXPECT_TRUE(destroyed);
}

// WAL Writer 只能依赖 WritableFile 抽象，而不是某个具体的 POSIX 文件类型。
// 该测试也确保通过基类指针销毁派生文件对象是安全的。
TEST(EnvTest, WritableFileSupportsPolymorphicWalWriteOperations) {
  bool destroyed = false;
  WritableFile* file = new RecordingWritableFile(&destroyed);

  EXPECT_TRUE(file->Append(Slice("wal-record")).ok());
  EXPECT_TRUE(file->Flush().ok());
  EXPECT_TRUE(file->Sync().ok());
  EXPECT_TRUE(file->Close().ok());

  auto* recording_file = static_cast<RecordingWritableFile*>(file);
  EXPECT_EQ(recording_file->contents(), "wal-record");
  EXPECT_TRUE(recording_file->closed());

  delete file;
  EXPECT_TRUE(destroyed);
}

}  // namespace
}  // namespace LSMKV
