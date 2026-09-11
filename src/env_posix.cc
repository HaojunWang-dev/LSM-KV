#include "env_posix.h"
#include "env.h"
#include "status.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#ifndef __Fuchsia__
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <dirent.h>

namespace LSMKV {

namespace {

constexpr const int kWritableFileBufferSize = 65536;

Status PosixError(const std::string& context, int error_number) {
  if (error_number == ENOENT) {
    return Status::NotFound(context, std::strerror(error_number));
  } else {
    return Status::IOError(context, std::strerror(error_number));
  }
}



class PosixWritableFile final : public WritableFile {
public:
  PosixWritableFile(std::string filename, int fd)
      : pos_(0), fd_(fd), is_manifest_(false), filename_(std::move(filename)),
        dirname_(filename_) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      Close();
    }
  }

  Status Append(const Slice &data) override {
    size_t write_size = data.size();
    const char *write_data = data.data();

    size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_size -= copy_size;
    write_data += copy_size;
    pos_ += copy_size;

    if (write_size == 0) {
      return Status::OK();
    }

    Status status = FlushBuffer();
    if (!status.ok()) {
      return status;
    }

    if (write_size < kWritableFileBufferSize) {
      std::memcpy(buf_, write_data, write_size);
      pos_ = write_size;
      return Status::OK();
    }
    return WriteUnBuffered(write_data, write_size);
  }

  Status Close() override {
    Status status = FlushBuffer();
    const int close_result = ::close(fd_);
    if (close_result < 0 && status.ok()) {
        return PosixError(filename_, errno);
    }
    fd_ = -1;
    return status;
  }

  Status Flush() override { return FlushBuffer(); }

  Status Sync() override {
    /*todo*/

    Status status = FlushBuffer();
    if (!status.ok()) {
      return status;
    }

    return SyncFd(fd_, filename_);
  }

private:
  Status FlushBuffer() {
    Status status = WriteUnBuffered(buf_, pos_);
    pos_ = 0;
    return status;
  }

  Status WriteUnBuffered(const char *data, size_t size) {
    while (size > 0) {
      ssize_t write_result = ::write(fd_, data, size);
      if (write_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      data += write_result;
      size -= write_result;
    }
    return Status::OK();
  }

  static Status SyncFd(int fd, const std::string& fd_path)
  {
    bool sync_success = ::fdatasync(fd) == 0;

    if (sync_success)
    {
        return Status::OK();
    }
    else {
        return PosixError(fd_path, errno);
    }
  }
  static std::string Dirname(const std::string &filename) {
    std::string::size_type seprator_pos = filename.rfind('/');
    if (seprator_pos == std::string::npos) {
      return std::string(".");
    }
    assert(filename.find('/', seprator_pos + 1) == std::string::npos);

    return filename.substr(0, seprator_pos);
  }

  static Slice Basename(const std::string &filename) {
    std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
      return Slice(filename);
    }

    assert(filename.find('/', separator_pos + 1) == std::string::npos);

    return Slice(filename.data() + separator_pos + 1,
                 filename.length() - separator_pos - 1);
  }

private:
  char buf_[kWritableFileBufferSize];
  size_t pos_;
  int fd_;

  const bool is_manifest_;
  const std::string filename_;
  const std::string dirname_;
};
} // namespace

Status NewPosixWritableFile(const std::string &filename, std::unique_ptr<WritableFile> *result)
{
  assert (result != nullptr && *result == nullptr);

  const int fd = ::open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);

  if (fd < 0)
  {
    return PosixError(filename, errno);
  }

  *result = std::make_unique<PosixWritableFile> (filename, fd);
  return Status::OK();
}

} // namespace LSMKV
