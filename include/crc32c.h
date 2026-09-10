#pragma once

#include <cstddef>
#include <cstdint>

namespace LSMKV {
namespace crc32c {

// 计算 data[0, n) 的 CRC32C（Castagnoli）校验和；空输入的校验和为 0。
uint32_t Value(const char* data, size_t n);

// 基于已有 crc 继续计算 data[0, n)，结果等于对两段数据拼接后计算 Value。
uint32_t Extend(uint32_t crc, const char* data, size_t n);

// 对 CRC 做可逆变换，避免 WAL header 中嵌入的校验和被误当作普通 payload CRC。
uint32_t Mask(uint32_t crc);
// 还原由 Mask() 产生的原始 CRC。
uint32_t Unmask(uint32_t masked_crc);

}  // namespace crc32c
}  // namespace LSMKV
