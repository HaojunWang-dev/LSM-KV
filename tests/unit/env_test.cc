#include <string>

#include <gtest/gtest.h>

#include "env.h"

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
