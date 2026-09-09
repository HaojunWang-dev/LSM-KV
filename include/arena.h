#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <assert.h>

namespace LSMKV {


class Arena {
public:
    Arena();
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    char* Allocate(size_t bytes);
    char* AllocateAligned(size_t bytes);

    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    char* AllocateFallback(size_t bytes);

    char* AllocateNewBlock(size_t block_bytes);

private:
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;
    std::vector<char*> blocks_;
    std::atomic<size_t> memory_usage_;
};

inline char* Arena::Allocate(size_t bytes)
{
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_)
    {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    return AllocateFallback(bytes);
}
}