#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "coding.h"

namespace LSMKV {
namespace {

TEST(CodingTest, Fixed32UsesLittleEndianAndAppendsToString) {
    // Fixed32 的字节序必须独立于主机字节序，Put 不能覆盖既有内容。
    std::array<char, 4> encoded{};
    EncodeFixed32(encoded.data(), 0x89ABCDEFU);

    EXPECT_EQ(static_cast<unsigned char>(encoded[0]), 0xEF);
    EXPECT_EQ(static_cast<unsigned char>(encoded[1]), 0xCD);
    EXPECT_EQ(static_cast<unsigned char>(encoded[2]), 0xAB);
    EXPECT_EQ(static_cast<unsigned char>(encoded[3]), 0x89);
    EXPECT_EQ(DecodeFixed32(encoded.data()), 0x89ABCDEFU);

    std::string output = "p";
    PutFixed32(&output, 0x01020304U);
    EXPECT_EQ(output, std::string("p\x04\x03\x02\x01", 5));
}

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

    std::string output;
    PutFixed64(&output, value);
    EXPECT_EQ(output, std::string(encoded.data(), encoded.size()));
}

TEST(CodingTest, Varint32RoundTripsBoundaryValues) {
    const std::array<uint32_t, 7> values{
        0, 1, 127, 128, 16383, 16384, std::numeric_limits<uint32_t>::max(),
    };

    for (uint32_t value : values) {
        std::array<char, 5> encoded{};
        char* const end = EncodeVarint32(encoded.data(), value);
        uint32_t decoded = 0;
        const char* const next = DecodeVarint32(encoded.data(), end, &decoded);

        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next, end);
        EXPECT_EQ(decoded, value);
        EXPECT_EQ(static_cast<size_t>(end - encoded.data()), VarintLength(value));
    }
}

TEST(CodingTest, Varint64RoundTripsBoundaryValues) {
    // 64 位编码最多使用 10 字节，最后一个字节只能存放最高 1 bit。
    const std::array<uint64_t, 7> values{
        0, 127, 128, 16384, (uint64_t{1} << 32) - 1,
        uint64_t{1} << 63, std::numeric_limits<uint64_t>::max(),
    };

    for (uint64_t value : values) {
        std::array<char, 10> encoded{};
        char* const end = EncodeVarint64(encoded.data(), value);
        uint64_t decoded = 0;
        const char* const next = DecodeVarint64(encoded.data(), end, &decoded);

        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next, end);
        EXPECT_EQ(decoded, value);
        EXPECT_EQ(static_cast<size_t>(end - encoded.data()), VarintLength(value));
    }
}

TEST(CodingTest, GetVarintsAdvanceSliceOnlyAfterSuccessfulDecode) {
    // Slice 是游标；失败不能消耗输入，否则上层无法决定如何处理坏数据。
    std::string encoded;
    PutVarint32(&encoded, 300);
    PutVarint64(&encoded, uint64_t{1} << 40);
    Slice input(encoded);
    uint32_t value32 = 0;
    uint64_t value64 = 0;

    ASSERT_TRUE(GetVarint32(&input, &value32));
    EXPECT_EQ(value32, 300U);
    ASSERT_TRUE(GetVarint64(&input, &value64));
    EXPECT_EQ(value64, uint64_t{1} << 40);
    EXPECT_TRUE(input.empty());

    const std::string truncated(1, static_cast<char>(0x80));
    Slice invalid(truncated);
    const Slice original = invalid;
    value32 = 123;
    EXPECT_FALSE(GetVarint32(&invalid, &value32));
    EXPECT_EQ(invalid.data(), original.data());
    EXPECT_EQ(invalid.size(), original.size());
    EXPECT_EQ(value32, 123U);
}

TEST(CodingTest, BoundedVarintDecodersRejectTruncatedAndOverflowEncodings) {
    // 解码器不得越过 limit，且要拒绝超出目标整数范围的终止字节。
    const std::array<char, 1> truncated{static_cast<char>(0x80)};
    uint32_t value32 = 0;
    EXPECT_EQ(DecodeVarint32(truncated.data(), truncated.data() + truncated.size(), &value32),
              nullptr);

    const std::array<char, 5> overflow32{
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x10)};
    EXPECT_EQ(DecodeVarint32(overflow32.data(), overflow32.data() + overflow32.size(), &value32),
              nullptr);

    const std::array<char, 10> overflow64{
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x02)};
    uint64_t value64 = 0;
    EXPECT_EQ(DecodeVarint64(overflow64.data(), overflow64.data() + overflow64.size(), &value64),
              nullptr);
}

TEST(CodingTest, LengthPrefixedSlicePreservesBinaryDataAndRejectsTruncation) {
    // Length-prefixed Slice 是后续 WAL/SSTable 记录可复用的字节字符串编码。
    const std::string value("a\0b", 3);
    std::string encoded;
    PutLengthPrefixedSlice(&encoded, Slice(value));
    Slice input(encoded);
    Slice decoded;

    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &decoded));
    EXPECT_EQ(decoded.ToString(), value);
    EXPECT_TRUE(input.empty());

    const std::string truncated("\x04xy", 3);
    Slice invalid(truncated);
    const Slice original = invalid;
    EXPECT_FALSE(GetLengthPrefixedSlice(&invalid, &decoded));
    EXPECT_EQ(invalid.data(), original.data());
    EXPECT_EQ(invalid.size(), original.size());
}

TEST(CodingTest, VarintLengthChangesAtSevenBitBoundaries) {
    EXPECT_EQ(VarintLength(0), 1U);
    EXPECT_EQ(VarintLength(127), 1U);
    EXPECT_EQ(VarintLength(128), 2U);
    EXPECT_EQ(VarintLength(16383), 2U);
    EXPECT_EQ(VarintLength(16384), 3U);
    EXPECT_EQ(VarintLength(std::numeric_limits<uint32_t>::max()), 5U);
    EXPECT_EQ(VarintLength(std::numeric_limits<uint64_t>::max()), 10U);
}

}  // namespace
}  // namespace LSMKV
