#pragma once

#include "env.h"
#include "status.h"

#include <cerrno>
#include <fcntl.h>
#include <memory.h>
#include <memory>
#include <string>

namespace LSMKV {
    Status NewPosixWritableFile(const std::string& filename, std::unique_ptr<WritableFile>* result)
    {
        assert(result != nullptr);
        assert(*result == nullptr);

        int fd;
        do {
            fd = ::open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
        } while (fd < 0 && errno == EINTR);

        if (fd < 0) 
        {
        }
    }    
}