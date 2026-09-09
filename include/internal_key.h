#pragma once 

#include <cassert>
#include <cstdint>

#include "coding.h"
#include "slice.h"

namespace LSMKV {


/*
 [varint32 internal_key_size]
 [user_key]
 [fixed64 seq<<8 | type]
 [varint32 value_size]
 [value]
*/
using SequenceNumber = uint64_t;

enum class ValueType : uint8_t {
    kDeletion = 0,
    kValue = 1,
};

constexpr SequenceNumber kMaxSequenceNumber = (static_cast<uint64_t>(1) << 56) - 1;

constexpr ValueType kValueTypeForSeek = ValueType::kValue;

inline uint64_t PackSequenceAndType(SequenceNumber sequence, ValueType type)
{
    assert(sequence <= kMaxSequenceNumber);

    return (sequence << 8) | static_cast<uint8_t> (type);
}

inline Slice ExtractUserKey(const Slice& internal_key) 
{
    assert(internal_key.size() >= 8);

    return Slice(internal_key.data(), internal_key.size() - 8);
}

class InternalKeyComparator {
public:
    int Compare(const Slice& a, const Slice& b) const
    {
        Slice user_a = ExtractUserKey(a);
        Slice user_b = ExtractUserKey(b);

        int r = user_a.Compare(user_b);

        if (r != 0)
        {
            return r;
        }

        uint64_t tag_a = DecodeFixed64(a.data() + a.size() - 8);
        uint64_t tag_b = DecodeFixed64(b.data() + b.size() - 8);

        if (tag_a > tag_b)
        {
            return -1;
        }

        if (tag_a < tag_b)
        {
            return 1;
        }

        return 0;
    }
};
}