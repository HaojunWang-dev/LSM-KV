#pragma once

#include "env.h"
#include "status.h"

#include <cerrno>
#include <fcntl.h>
#include <memory.h>
#include <memory>
#include <string>

namespace LSMKV {
class PosixEnv final : public Env {
 public:
  Status NewWritableFile(const std::string& filename,
                         std::unique_ptr<WritableFile>* result) override;
  Status NewSequentialFile(
      const std::string& filename,
      std::unique_ptr<SequentialFile>* result) override;
  Status FileExists(const std::string& filename, bool* exists) override;
  Status CreateDir(const std::string& dirname) override;
};
}  // namespace LSMKV
