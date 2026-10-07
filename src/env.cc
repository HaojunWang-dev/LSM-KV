#include "env.h"

#include <array>
#include <memory>

namespace LSMKV {

Status Env::NewAppendableFile(const std::string&, WritableFile** result) {
  if (result == nullptr) {
    return Status::InvalidArgument("NewAppendableFile", "null output");
  }
  *result = nullptr;
  return Status::NotSupported("NewAppendableFile");
}

Status Env::GetFileSize(const std::string&, uint64_t* size) {
  if (size == nullptr) {
    return Status::InvalidArgument("GetFileSize", "null output");
  }
  *size = 0;
  return Status::NotSupported("GetFileSize");
}

Status ReadFileToString(Env* env, const std::string& filename, std::string* data) {
  if (data == nullptr) {
    return Status::InvalidArgument("ReadFileToString", "null output");
  }
  data->clear();
  if (env == nullptr) {
    return Status::InvalidArgument("ReadFileToString", "null Env");
  }

  SequentialFile* result = nullptr;
  Status status = env->NewSequentialFile(filename, &result);
  std::unique_ptr<SequentialFile> file(result);
  if (!status.ok()) return status;
  if (file == nullptr) {
    return Status::IOError("ReadFileToString", "Env returned a null SequentialFile");
  }

  std::array<char, 8192> scratch;
  while (true) {
    Slice fragment;
    status = file->Read(scratch.size(), &fragment, scratch.data());
    if (!status.ok()) return status;
    if (fragment.empty()) return Status::OK();
    data->append(fragment.data(), fragment.size());
  }
}

}  // namespace LSMKV
