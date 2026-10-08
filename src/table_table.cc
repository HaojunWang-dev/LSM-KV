#include "table_table.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "comparator.h"
#include "format.h"
#include "coding.h"
#include "iterator.h"
#include "logging.h"
#include "table_format.h"

namespace LSMKV {

inline uint32_t Block::NumRestarts() const {
    assert(size_ >= sizeof(uint32_t));
    return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_((contents.heap_allocated)) {
        if (size_ < sizeof(uint32_t)) {
            size_ = 0;
        } else {
            size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
            if (NumRestarts() > max_restarts_allowed) {
                size_ = 0; 
            } else {
                restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);
            }
        }
      }
Block::~Block() {
    if (owned_) {
        delete[] data_;
    }
}

static inline const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared, uint32_t* value_length) {
    if (limit - p < 3) return nullptr;
    *shared = reinterpret_cast<const uint8_t*>(p)[0];
    *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
    *value_length = reinterpret_cast<const uint8_t*>(p)[2];

    if (*shared | *non_shared | *value_length < 128) {
        p += 3;
    } else {
        if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
        if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
        if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
    }

    if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
        return nullptr;
    }

    return p;
}

class Block::Iter : public Iterator {
private:
    const Comparator* const comparator_;
    const char* const data_;
    uint32_t const restarts_;
    uint32_t const num_restarts_;

    uint32_t current_;
    uint32_t restart_index_;
    std::string key;
    Slice value_;
    Status status_;

    inline int Compare(const Slice& a, const Slice& b) const {
        return comparator_->Compare(a, b);
    }

    inline uint32_t NextEntryOffset() const  {
        return (value_.data() + value_.size()) - data_;
    }

    uint32_t GetRestartPoint(uint32_t index) {
        assert(index  < num_restarts_);
        return DecodeFixed32(data_ + + restarts_ + restart_index_ * sizeof(uint32_t));
    }
};
}

