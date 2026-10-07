#include "logging.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "env.h"
#include "slice.h"

namespace LSMKV {

bool ConsumeDecimalNumber(Slice *in, uint64_t *val) {
    constexpr const uint64_t kMaxInt64 = std::numeric_limits<uint64_t>::max();
    constexpr const char kLastDigitOfMaxUint64 = '0' + static_cast<char>(kMaxInt64 % 10);

    uint64_t value = 0;

    const uint8_t* start = reinterpret_cast<const uint8_t*>(in->data());

    const uint8_t* end = start + in->size();
    const uint8_t* current = start;

    for (; current != end; current++) {
        const uint8_t ch = *current;

        if (ch < '0' || ch > '9') break;

        if (value > kMaxInt64 / 10 || (value == kMaxInt64 / 10 && ch > kLastDigitOfMaxUint64)){
            return false;
        }

        value = value * 10 + (ch - '0');
    }

    *val = value;

    const size_t digits_consumed = current - start;
    in->remove_prefix(digits_consumed);
    return (digits_consumed != 0);
}
}
