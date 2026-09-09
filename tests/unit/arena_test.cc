#include <gtest/gtest.h>

#include <cstdint>

#include "arena.h"

namespace LSMKV {
namespace {

TEST(ArenaTest, AllocateAlignedReturnsAnEightByteAlignedPointerAfterUnalignedUse) {
    Arena arena;

    ASSERT_NE(arena.Allocate(1), nullptr);
    char* const aligned = arena.AllocateAligned(8);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % 8, 0U);
}

}  // namespace
}  // namespace LSMKV
