#include <string>

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
  Status NewWritableFile(const std::string&,
                         std::unique_ptr<WritableFile>*) override {
    return Status::OK();
  }
  Status NewSequentialFile(const std::string&,
                           std::unique_ptr<SequentialFile>*) override {
    return Status::OK();
  }
  Status FileExists(const std::string&, bool* exists) override {
    *exists = true;
    return Status::OK();
  }
  Status CreateDir(const std::string&) override { return Status::OK(); }
};

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
