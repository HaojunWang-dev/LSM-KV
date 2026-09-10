#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <cassert>

namespace LSMKV {

class Slice {
public:
    Slice() : data_(""), size_(0) {}

    Slice(const char* data, size_t size)
        : data_(data), size_(size) {}

    Slice(const std::string& s)
        : data_(s.data()), size_(s.size()) {}

    const char* data() const
    {
        return data_;
    }

    size_t size() const
    {
        return size_;
    } 

    bool empty() const
    {
        return size_ == 0;
    }

    std::string ToString() const 
    {
        return std::string(data_, size_);
    }

    int Compare(const Slice& other) const
    {
        const size_t min_len = std::min(size_, other.size_);

        int r = std::memcmp(data_, other.data_, min_len);

        if (r == 0)
        {
            if (size_ < other.size_)
            {
                return -1;
            }

            if (size_ > other.size_)
            {
                return 1;
            }
        }

        return r;
    }

    char operator[](size_t n)
    {
        assert(n < size());
        return data_[n];
    }

    void clear()
    {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(size_t n)
    {
        assert(n <= size());
        data_ += n;
        size_ -= n;
    }

    bool start_with(const Slice& x)
    {
        return ((size_ >= x.size()) && (memcmp(data_, x.data(), x.size()) == 0));
    }
private:
    const char* data_;
    size_t size_;
};
}