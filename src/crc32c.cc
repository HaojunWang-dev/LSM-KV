#include "crc32c.h"

#include <array>

namespace LSMKV {
namespace crc32c {
namespace {

// Castagnoli 多项式的反射表示，用于按最低有效位优先处理字节。
constexpr uint32_t kPolynomial = 0x82F63B78U;
constexpr uint32_t kMaskDelta = 0xA282EAD8U;

constexpr uint32_t MakeTableEntry(uint32_t value) {
    for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0 ? (value >> 1) ^ kPolynomial : value >> 1;
    }
    return value;
}

constexpr std::array<uint32_t, 256> MakeTable() {
    std::array<uint32_t, 256> table{};
    for (size_t index = 0; index < table.size(); ++index) {
        table[index] = MakeTableEntry(static_cast<uint32_t>(index));
    }
    return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Value(const char* data, size_t n) {
    return Extend(0, data, n);
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
    uint32_t state = ~crc;
    for (size_t index = 0; index < n; ++index) {
        const uint32_t byte = static_cast<unsigned char>(data[index]);
        state = kTable[(state ^ byte) & 0xffU] ^ (state >> 8);
    }
    return ~state;
}

uint32_t Mask(uint32_t crc) {
    return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

uint32_t Unmask(uint32_t masked_crc) {
    const uint32_t rotated = masked_crc - kMaskDelta;
    return (rotated >> 17) | (rotated << 15);
}

}  // namespace crc32c
}  // namespace LSMKV
