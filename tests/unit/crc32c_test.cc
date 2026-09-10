#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "crc32c.h"

namespace LSMKV {
namespace crc32c {
namespace {

TEST(Crc32cTest, MatchesCastagnoliReferenceVectors) {
    // CRC32C 的标准检验向量；WAL checksum 必须与该 wire-format 一致。
    EXPECT_EQ(Value("", 0), 0U);
    EXPECT_EQ(Value("123456789", 9), 0xE3069283U);
    EXPECT_EQ(Value("The quick brown fox jumps over the lazy dog", 43),
              0x22620404U);
}

TEST(Crc32cTest, ExtendMatchesChecksumOfConcatenatedInput) {
    // 增量计算让 WAL Writer 可在不复制 payload 的情况下组合多个字节段。
    const std::string prefix = "write-batch:";
    const std::string suffix("a\0b", 3);
    const std::string complete = prefix + suffix;

    const uint32_t prefix_crc = Value(prefix.data(), prefix.size());
    EXPECT_EQ(Extend(prefix_crc, suffix.data(), suffix.size()),
              Value(complete.data(), complete.size()));
}

TEST(Crc32cTest, MaskRoundTripsAndAvoidsCommonChecksumValues) {
    // Mask 防止 WAL header 中的 checksum 与普通 payload checksum 发生误匹配。
    EXPECT_EQ(Mask(0), 0xA282EAD8U);
    EXPECT_EQ(Mask(0xFFFFFFFFU), 0xA282EAD7U);

    const uint32_t crc = Value("record", 6);
    EXPECT_NE(Mask(crc), crc);
    EXPECT_EQ(Unmask(Mask(crc)), crc);
}

}  // namespace
}  // namespace crc32c
}  // namespace LSMKV
