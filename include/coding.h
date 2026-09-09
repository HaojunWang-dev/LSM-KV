#pragma once

#include <cstddef>
#include <cstdint>

namespace LSMKV {

// 将 value 以小端序写入 dst 指向的连续 8 字节。
// 调用方必须保证 dst 至少有 8 字节可写空间；所有 64 位都会被保留。
inline void EncodeFixed64(char* dst, uint64_t value)
{
    uint8_t* const buffer = reinterpret_cast<uint8_t*> (dst);
    for (int i = 0; i < 8; i++)
    {
        buffer[i] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
}

// 从 ptr 指向的连续 8 字节按小端序解码一个无符号 64 位整数。
// 调用方必须保证 ptr 至少有 8 字节可读空间。
inline uint64_t DecodeFixed64(const char* ptr)
{
    uint64_t result = 0;

    for (int i = 7; i >= 0; i--)
    {
        result <<= 8;
        result |= static_cast<unsigned char> (ptr[i]);
    }

    return result;
}

// 将 value 编码为可变长度的 base-128 整数并写入 dst。
// 每个字节的最高位表示后续是否还有数据；返回值指向最后一个已写字节的后一个位置。
// 调用方需保证 dst 有足够空间（uint32_t 最多需要 5 字节）。
inline char* EncodeVarint32(char* dst, uint32_t value)
{
    unsigned char* ptr = reinterpret_cast<unsigned char*>(dst);

    while (value >= 128)
    {
        *ptr++ = static_cast<unsigned char> (value | 0x80);
        
        value >>= 7;
    }

    *ptr++ = static_cast<unsigned char> (value);

    return reinterpret_cast<char*>(ptr);
}

// 从 ptr 解码一个 varint32，并在成功时写入 *value。
// 成功返回编码末尾的后一个位置；若前 5 字节中不存在终止字节则返回 nullptr。
// 由于接口不携带长度，调用方必须保证 ptr 至少有 5 字节可读空间。
inline const char* DecodeVarint32(const char* ptr, uint32_t* value)
{
    uint32_t result = 0;

    for (uint32_t shift = 0; shift <= 28; shift+= 7)
    {
        uint32_t byte = static_cast<unsigned char> (*ptr++);

        if (byte & 128)
        {
            result |= (byte & 127) << shift;
        }
        else 
        {
            result |= (byte) << shift;

            *value = result;

            return ptr;
        }
    }

    return nullptr;
}

// 返回 value 采用 base-128 varint 编码时所需的最小字节数。
inline size_t VarintLength(uint64_t value)
{
    size_t len = 1;

    while (value >= 128)
    {
        value >>= 7;
        ++len;
    }

    return len;
}
}
