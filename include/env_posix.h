#pragma once

#include "env.h"
#include "status.h"

#include <cerrno>
#include <fcntl.h>
#include <memory.h>
#include <memory>
#include <string>

namespace LSMKV {
    Status NewPosixWritableFile(const std::string& filename, std::unique_ptr<WritableFile>* result);
}