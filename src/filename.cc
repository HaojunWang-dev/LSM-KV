#include "filename.h"

#include <cassert>
#include <memory>
#include <utility>

#include "env.h"
#include "logging.h"

namespace LSMKV {
namespace {

std::string NumberDigits(std::uint64_t number) {
  assert(number > 0);
  std::string digits = std::to_string(number);
  if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
  return digits;
}

std::string NumberedFile(const std::string& dbname, std::uint64_t number,
                         const char* suffix) {
  return dbname + "/" + NumberDigits(number) + suffix;
}

}  // namespace

std::string LogFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedFile(dbname, number, ".log");
}

std::string TableFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedFile(dbname, number, ".ldb");
}

std::string SSTTableFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedFile(dbname, number, ".sst");
}

std::string DescriptorFileName(const std::string& dbname, std::uint64_t number) {
  return dbname + "/MANIFEST-" + NumberDigits(number);
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TempFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedFile(dbname, number, ".dbtmp");
}

std::string InfoLogFileName(const std::string& dbname) { return dbname + "/LOG"; }

std::string OldInfoLogFileName(const std::string& dbname) { return dbname + "/LOG.old"; }

bool ParseFileName(const std::string& filename, std::uint64_t* number,
                   FileType* type) {
  assert(number != nullptr && type != nullptr);
  if (number == nullptr || type == nullptr ||
      filename.find('/') != std::string::npos ||
      filename.find('\0') != std::string::npos) {
    return false;
  }

  std::uint64_t parsed_number = 0;
  FileType parsed_type;
  if (filename == "CURRENT") {
    parsed_type = FileType::kCurrentFile;
  } else if (filename == "LOCK") {
    parsed_type = FileType::kDBLockFile;
  } else if (filename == "LOG" || filename == "LOG.old") {
    parsed_type = FileType::kInfoLogFile;
  } else {
    // 仅在完整名称匹配后才发布结果，辅助解析的局部修改不会泄漏给调用方。
    Slice rest(filename);
    if (filename.compare(0, 9, "MANIFEST-") == 0) {
      rest.remove_prefix(9);
      if (!ConsumeDecimalNumber(&rest, &parsed_number) || !rest.empty()) return false;
      parsed_type = FileType::kDescriptorFile;
    } else {
      if (!ConsumeDecimalNumber(&rest, &parsed_number)) return false;
      if (rest.Compare(Slice(".log")) == 0) {
        parsed_type = FileType::kLogFile;
      } else if (rest.Compare(Slice(".ldb")) == 0 || rest.Compare(Slice(".sst")) == 0) {
        parsed_type = FileType::kTableFile;
      } else if (rest.Compare(Slice(".dbtmp")) == 0) {
        parsed_type = FileType::kTempFile;
      } else {
        return false;
      }
    }
  }
  *number = parsed_number;
  *type = parsed_type;
  return true;
}

Status SetCurrentFile(Env* env, const std::string& dbname,
                      std::uint64_t descriptor_number) {
  if (env == nullptr || descriptor_number == 0 || dbname.empty() ||
      dbname.find('\0') != std::string::npos) {
    return Status::InvalidArgument("SetCurrentFile", "invalid env, directory or file number");
  }

  const std::string temporary = TempFileName(dbname, descriptor_number);
  const std::string contents =
      DescriptorFileName(dbname, descriptor_number).substr(dbname.size() + 1) + "\n";
  WritableFile* result = nullptr;
  Status status = env->NewWritableFile(temporary, &result);
  std::unique_ptr<WritableFile> file(result);  // 调用方接管裸指针所有权。
  if (!status.ok()) return status;

  if (file == nullptr) {
    status = Status::IOError("SetCurrentFile", "Env returned a null WritableFile");
  } else {
    status = file->Append(Slice(contents));
    if (status.ok()) status = file->Sync();
    const Status close_status = file->Close();
    if (status.ok()) status = close_status;
    file.reset();
  }

  // 同步临时文件及既有 MANIFEST 的目录项，再切换 CURRENT，避免悬空引用。
  if (status.ok()) status = env->SyncDir(dbname);
  if (status.ok()) status = env->RenameFile(temporary, CurrentFileName(dbname));
  if (!status.ok()) {
    // 保留首个错误。清理失败最多留下临时文件，不能删除 CURRENT 或 MANIFEST。
    env->RemoveFile(temporary);
    return status;
  }

  // rename 已成功：后续同步失败仍返回错误，但不尝试撤销已可见的 CURRENT。
  return env->SyncDir(dbname);
}

}  // namespace LSMKV
