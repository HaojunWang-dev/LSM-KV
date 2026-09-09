#pragma once

#include <cstring>
#include <string>

#include "coding.h"
#include "internal_key.h"
#include "slice.h"

namespace LSMKV {

// LookupKey 为一次 snapshot 查询预编码 InternalKey 和 MemTableKey。
class LookupKey {
public:
    LookupKey(const Slice& user_key, SequenceNumber sequence) 
    {
        const size_t internal_key_size = user_key.size() + 8;

        const size_t total_size = VarintLength(internal_key_size) + internal_key_size;

        space_.resize(total_size);

        char* start = space_.data();

        char* p = EncodeVarint32(start, static_cast<uint32_t> (internal_key_size));

        internal_key_offset_ = static_cast<size_t> (p - start);

        std::memcpy(p, user_key.data(), user_key.size());

        p += user_key.size();

        // Seek 使用 snapshot 与 kValue 构造 tag，使 lower_bound 跳过未来版本。
        EncodeFixed64(p, PackSequenceAndType(sequence, kValueTypeForSeek));

        user_key_size_ = user_key.size();
    }

    Slice MemTableKey() const 
    {
        // 包含长度前缀，直接作为底层 SkipList 查找目标。
        return Slice(space_.data(), space_.size());
    }

    Slice InternalKey() const
    {
        // 不含长度前缀：UserKey 后跟 8 字节 sequence/type tag。
        return Slice(space_.data() + internal_key_offset_, user_key_size_ + 8);
    }

    Slice UserKey() const
    {
        // 仅借用 InternalKey 的 UserKey 前缀。
        return Slice(space_.data() + internal_key_offset_ , user_key_size_);
    }

private:
    std::string space_;

    size_t internal_key_offset_;
    size_t user_key_size_;
};
}
