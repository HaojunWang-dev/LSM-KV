#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "coding.h"

namespace LSMKV {
namespace {

TEST(CodingTest, Fixed64UsesLittleEndianAndPreservesAllBits) {
    // 固定宽度编码必须保留完整 64 位，而不是只写入低位字节。
    constexpr uint64_t value = 0x0123456789ABCDEFULL;
    std::array<char, 8> encoded{};

    EncodeFixed64(encoded.data(), value);

    const std::array<unsigned char, 8> expected{
        0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    for (size_t index = 0; index < encoded.size(); ++index) {
        EXPECT_EQ(static_cast<unsigned char>(encoded[index]), expected[index]);
    }
    EXPECT_EQ(DecodeFixed64(encoded.data()), value);
}

TEST(CodingTest, Varint32RoundTripsBoundaryValues) {
    const std::array<uint32_t, 7> values{
        0,
        1,
        127,
        128,
        16383,
        16384,
        std::numeric_limits<uint32_t>::max(),
    };

    for (uint32_t value : values) {
        std::array<char, 5> encoded{};
        char* const end = EncodeVarint32(encoded.data(), value);
        uint32_t decoded = 0;
        const char* const next = DecodeVarint32(encoded.data(), &decoded);

        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next, end);
        EXPECT_EQ(decoded, value);
        EXPECT_EQ(static_cast<size_t>(end - encoded.data()), VarintLength(value));
    }
}

TEST(CodingTest, Varint32RejectsAnUnterminatedFiveByteEncoding) {
    const std::array<char, 5> malformed{
        static_cast<char>(0x80),
        static_cast<char>(0x80),
        static_cast<char>(0x80),
        static_cast<char>(0x80),
        static_cast<char>(0x80),
    };
    uint32_t decoded = 0;

    EXPECT_EQ(DecodeVarint32(malformed.data(), &decoded), nullptr);
}

TEST(CodingTest, VarintLengthChangesAtSevenBitBoundaries) {
    EXPECT_EQ(VarintLength(0), 1U);
    EXPECT_EQ(VarintLength(127), 1U);
    EXPECT_EQ(VarintLength(128), 2U);
    EXPECT_EQ(VarintLength(16383), 2U);
    EXPECT_EQ(VarintLength(16384), 3U);
    EXPECT_EQ(VarintLength(std::numeric_limits<uint32_t>::max()), 5U);
}

}  // namespace
}  // namespace LSMKV
