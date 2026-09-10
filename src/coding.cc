#include "coding.h"

namespace LSMKV {

void EncodeFixed32(char* dst, uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        dst[index] = static_cast<char>(value & 0xffU);
        value >>= 8;
    }
}

uint32_t DecodeFixed32(const char* ptr) {
    uint32_t result = 0;
    for (int index = 3; index >= 0; --index) {
        result <<= 8;
        result |= static_cast<unsigned char>(ptr[index]);
    }
    return result;
}

void EncodeFixed64(char* dst, uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        dst[index] = static_cast<char>(value & 0xffU);
        value >>= 8;
    }
}

uint64_t DecodeFixed64(const char* ptr) {
    uint64_t result = 0;
    for (int index = 7; index >= 0; --index) {
        result <<= 8;
        result |= static_cast<unsigned char>(ptr[index]);
    }
    return result;
}

void PutFixed32(std::string* dst, uint32_t value) {
    char buffer[4];
    EncodeFixed32(buffer, value);
    dst->append(buffer, sizeof(buffer));
}

void PutFixed64(std::string* dst, uint64_t value) {
    char buffer[8];
    EncodeFixed64(buffer, value);
    dst->append(buffer, sizeof(buffer));
}

char* EncodeVarint32(char* dst, uint32_t value) {
    char* ptr = dst;
    while (value >= 0x80U) {
        *ptr++ = static_cast<char>((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *ptr++ = static_cast<char>(value);
    return ptr;
}

char* EncodeVarint64(char* dst, uint64_t value) {
    char* ptr = dst;
    while (value >= 0x80U) {
        *ptr++ = static_cast<char>((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *ptr++ = static_cast<char>(value);
    return ptr;
}

void PutVarint32(std::string* dst, uint32_t value) {
    char buffer[5];
    const char* const end = EncodeVarint32(buffer, value);
    dst->append(buffer, end - buffer);
}

void PutVarint64(std::string* dst, uint64_t value) {
    char buffer[10];
    const char* const end = EncodeVarint64(buffer, value);
    dst->append(buffer, end - buffer);
}

size_t VarintLength(uint64_t value) {
    size_t length = 1;
    while (value >= 0x80U) {
        value >>= 7;
        ++length;
    }
    return length;
}

const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                    uint32_t* value) {
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        const uint32_t byte = static_cast<unsigned char>(*p++);
        if ((byte & 0x80U) == 0) {
            if (shift == 28 && byte > 0x0fU) {
                return nullptr;
            }
            *value = result | (byte << shift);
            return p;
        }
        result |= (byte & 0x7fU) << shift;
    }
    return nullptr;
}

const char* DecodeVarint32(const char* ptr, const char* limit,
                            uint32_t* value) {
    return GetVarint32Ptr(ptr, limit, value);
}

const char* GetVarint64PtrFallback(const char* p, const char* limit,
                                    uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        const uint64_t byte = static_cast<unsigned char>(*p++);
        if ((byte & 0x80U) == 0) {
            if (shift == 63 && byte > 0x01U) {
                return nullptr;
            }
            *value = result | (byte << shift);
            return p;
        }
        if (shift == 63) {
            return nullptr;
        }
        result |= (byte & 0x7fU) << shift;
    }
    return nullptr;
}

const char* DecodeVarint64(const char* ptr, const char* limit,
                            uint64_t* value) {
    return GetVarint64PtrFallback(ptr, limit, value);
}

bool GetVarint32(Slice* input, uint32_t* value) {
    uint32_t decoded = 0;
    const char* const begin = input->data();
    const char* const limit = begin + input->size();
    const char* const next = GetVarint32Ptr(begin, limit, &decoded);
    if (next == nullptr) {
        return false;
    }
    *input = Slice(next, limit - next);
    *value = decoded;
    return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
    uint64_t decoded = 0;
    const char* const begin = input->data();
    const char* const limit = begin + input->size();
    const char* const next = GetVarint64PtrFallback(begin, limit, &decoded);
    if (next == nullptr) {
        return false;
    }
    *input = Slice(next, limit - next);
    *value = decoded;
    return true;
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    Slice remaining = *input;
    uint32_t length = 0;
    if (!GetVarint32(&remaining, &length) || remaining.size() < length) {
        return false;
    }
    *result = Slice(remaining.data(), length);
    remaining.remove_prefix(length);
    *input = remaining;
    return true;
}

}  // namespace LSMKV
