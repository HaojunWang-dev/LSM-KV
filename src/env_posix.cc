#include "env_posix.h"
#include "env.h"
#include "status.h"

#include <dirent.h>
#include <fcntl.h>
#include <memory>
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
#include <dirent.h>
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

namespace LSMKV {

namespace {

constexpr const int kWritableFileBufferSize = 65536;

Status PosixError(const std::string &context, int error_number) {
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
    const int close_error = errno;
    // close 即使报告错误也可能已释放句柄，不能让析构重试并误关复用后的 fd。
    fd_ = -1;
    if (close_result < 0 && status.ok()) {
      return PosixError(filename_, close_error);
    }
    return status;
  }

  Status Flush() override { return FlushBuffer(); }

  Status Sync() override {
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

  static Status SyncFd(int fd, const std::string &fd_path) {
    bool sync_success = ::fdatasync(fd) == 0;

    if (sync_success) {
      return Status::OK();
    } else {
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

class PosixSequentialFile final : public SequentialFile {
public:
  PosixSequentialFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}

  ~PosixSequentialFile() override { close(fd_); };

  Status Read(size_t n, Slice* result, char* scratch) override
  {
    Status status;

    while (true)
    {
      ::ssize_t read_size = ::read(fd_, scratch, n);
      if (read_size < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        status = PosixError(filename_, errno);
        break;
      }
      *result = Slice(scratch, read_size);
      break;
    }
    return status;
  }

  Status Skip(uint64_t n) override {
      if (n > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return Status::InvalidArgument(filename_, "skip offset exceeds off_t range");
      }

      if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) ==
          static_cast<off_t>(-1))
      {
        return PosixError(filename_, errno);
      }

      return Status::OK();
  }
private:
  const int fd_;
  const std::string filename_;
};

} // namespace

Status PosixEnv::NewWritableFile(const std::string &filename,
                                 WritableFile** result) {
  assert(result != nullptr);
  *result = nullptr;

  const int fd = ::open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);

  if (fd < 0) {
    return PosixError(filename, errno);
  }

  // 在对象构造完成前临时拥有 fd；构造抛出异常时仍能释放资源。
  struct CloseOnFailure {
    int fd;
    ~CloseOnFailure() { if (fd >= 0) ::close(fd); }
  } descriptor{fd};
  *result = new PosixWritableFile(filename, fd);
  descriptor.fd = -1;  // fd 的所有权已随文件对象交给调用方。
  return Status::OK();
}


Status PosixEnv::NewSequentialFile(const std::string &filename, SequentialFile** result)
{
  assert(result != nullptr);
  *result = nullptr;

  const int fd = ::open(filename.c_str(), O_RDONLY, 0664);

  if (fd < 0) {
    return PosixError(filename, errno);
  }

  // 分配或构造失败时关闭 fd，成功后由返回的文件对象接管。
  struct CloseOnFailure {
    int fd;
    ~CloseOnFailure() { if (fd >= 0) ::close(fd); }
  } descriptor{fd};
  *result = new PosixSequentialFile(filename, fd);
  descriptor.fd = -1;
  return Status::OK();
}

Status PosixEnv::NewAppendableFile(const std::string& filename,
                                  WritableFile** result) {
  if (result == nullptr) {
    return Status::InvalidArgument("NewAppendableFile", "null output");
  }
  *result = nullptr;
  if (filename.find('\0') != std::string::npos) {
    return Status::InvalidArgument("NewAppendableFile", "path contains NUL");
  }

  int fd;
  do {
    fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0664);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return PosixError(filename, errno);

  struct CloseOnFailure {
    int fd;
    ~CloseOnFailure() { if (fd >= 0) ::close(fd); }
  } descriptor{fd};
  *result = new PosixWritableFile(filename, fd);
  descriptor.fd = -1;
  return Status::OK();
}

Status PosixEnv::GetFileSize(const std::string& filename, uint64_t* size) {
  if (size == nullptr) {
    return Status::InvalidArgument("GetFileSize", "null output");
  }
  *size = 0;
  if (filename.find('\0') != std::string::npos) {
    return Status::InvalidArgument("GetFileSize", "path contains NUL");
  }
  struct stat info;
  int result;
  do {
    result = ::stat(filename.c_str(), &info);
  } while (result != 0 && errno == EINTR);
  if (result != 0) return PosixError(filename, errno);
  if (info.st_size < 0 || !S_ISREG(info.st_mode)) {
    return Status::IOError(filename, "not a regular file with a valid size");
  }
  *size = static_cast<uint64_t>(info.st_size);
  return Status::OK();
}

Status PosixEnv::FileExists(const std::string& filename, bool* exists) {
  assert(exists != nullptr);

  struct stat file_stat;
  if (::stat(filename.c_str(), &file_stat) == 0) {
    *exists = true;
    return Status::OK();
  }
  if (errno == ENOENT) {
    *exists = false;
    return Status::OK();
  }
  return PosixError(filename, errno);
}

Status PosixEnv::CreateDir(const std::string& dirname) {
  if (::mkdir(dirname.c_str(), 0755) != 0) {
    return PosixError(dirname, errno);
  }
  return Status::OK();
}

Status PosixEnv::RenameFile(const std::string& source, const std::string& target) {
  if (source.find('\0') != std::string::npos ||
      target.find('\0') != std::string::npos) {
    return Status::InvalidArgument("RenameFile", "path contains NUL");
  }
  if (::rename(source.c_str(), target.c_str()) != 0) {
    const int rename_error = errno;
    return PosixError(source + " -> " + target, rename_error);
  }
  return Status::OK();
}

Status PosixEnv::RemoveFile(const std::string& filename) {
  if (filename.find('\0') != std::string::npos) {
    return Status::InvalidArgument("RemoveFile", "path contains NUL");
  }
  if (::unlink(filename.c_str()) != 0) {
    return PosixError(filename, errno);
  }
  return Status::OK();
}

Status PosixEnv::SyncDir(const std::string& dirname) {
  if (dirname.find('\0') != std::string::npos) {
    return Status::InvalidArgument("SyncDir", "path contains NUL");
  }
  int fd;
  do {
    fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return PosixError(dirname, errno);

  int sync_result;
  do {
    sync_result = ::fsync(fd);
  } while (sync_result != 0 && errno == EINTR);
  const int sync_error = sync_result == 0 ? 0 : errno;
  // 无论 fsync 是否成功都释放句柄；close 出错后不重试已可能释放的 fd。
  const int close_result = ::close(fd);
  const int close_error = close_result == 0 ? 0 : errno;
  if (sync_error != 0) return PosixError(dirname, sync_error);
  if (close_error != 0) return PosixError(dirname, close_error);
  return Status::OK();
}

Env* Env::Default() {
  static PosixEnv default_env;
  return &default_env;
}
} // namespace LSMKV
