#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "slice.h"

namespace LSMKV {

// 将 32 位整数以小端序写入 dst；用于稳定的定长磁盘/内存格式。
void EncodeFixed32(char* dst, uint32_t value);
// 从 ptr 指向的 4 字节小端序数据还原 32 位整数。
uint32_t DecodeFixed32(const char* ptr);
// 将 64 位整数以小端序写入 dst；InternalKey 的 sequence/type tag 依赖此格式。
void EncodeFixed64(char* dst, uint64_t value);
// 从 ptr 指向的 8 字节小端序数据还原 64 位整数。
uint64_t DecodeFixed64(const char* ptr);

// 将固定 32 位整数追加到字符串缓冲区，保留 dst 原有内容。
void PutFixed32(std::string* dst, uint32_t value);
// 将固定 64 位整数追加到字符串缓冲区，保留 dst 原有内容。
void PutFixed64(std::string* dst, uint64_t value);

// 将 32 位整数编码为 base-128 varint；调用方需提供最多 5 字节空间。
char* EncodeVarint32(char* dst, uint32_t value);
// 将 64 位整数编码为 base-128 varint；调用方需提供最多 10 字节空间。
char* EncodeVarint64(char* dst, uint64_t value);
// 将 32 位 varint 追加到字符串缓冲区。
void PutVarint32(std::string* dst, uint32_t value);
// 将 64 位 varint 追加到字符串缓冲区。
void PutVarint64(std::string* dst, uint64_t value);

// 在 [ptr, limit) 内解码 32 位 varint；失败时不会修改 *value。
const char* DecodeVarint32(const char* ptr, const char* limit, uint32_t* value);
// 在 [ptr, limit) 内解码 64 位 varint；失败时不会修改 *value。
const char* DecodeVarint64(const char* ptr, const char* limit, uint64_t* value);

// 返回 value 采用 varint 格式所需的最小字节数，用于预先计算编码空间。
size_t VarintLength(uint64_t value);

// 处理多字节或边界不足的 32 位 varint 解码慢路径。
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                    uint32_t* value);
// 处理 64 位 varint 的带边界解码路径。
const char* GetVarint64PtrFallback(const char* p, const char* limit,
                                    uint64_t* value);

// 单字节 varint32 的常见路径，其他情况交给带边界检查的 fallback。
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
    if (p < limit) {
        const uint32_t result = static_cast<unsigned char>(*p);
        if ((result & 0x80U) == 0) {
            *value = result;
            return p + 1;
        }
    }
    return GetVarint32PtrFallback(p, limit, value);
}

// 从 input 前端读取 32 位 varint；成功时前移 input，失败时 input 保持不变。
bool GetVarint32(Slice* input, uint32_t* value);
// 从 input 前端读取 64 位 varint；成功时前移 input，失败时 input 保持不变。
bool GetVarint64(Slice* input, uint64_t* value);

// 追加 [varint32 length][bytes] 格式的 Slice，适用于二进制 key 和 value。
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
// 读取一个长度前缀 Slice；成功时 result 借用输入字节且 input 前进。
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

}  // namespace LSMKV
