#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "status.h"

namespace LSMKV {
namespace {

TEST(StatusTest, DefaultAndOkFactoryRepresentSuccess) {
    // OK 使用空 state_ 表示，不携带错误类别或诊断消息。
    const Status default_status;
    const Status factory_status = Status::OK();

    EXPECT_TRUE(default_status.ok());
    EXPECT_TRUE(factory_status.ok());
    EXPECT_EQ(default_status.ToString(), "OK");
    EXPECT_EQ(factory_status.ToString(), "OK");
    EXPECT_FALSE(default_status.IsNotFound());
    EXPECT_FALSE(default_status.IsCorruption());
    EXPECT_FALSE(default_status.IsIOError());
}

TEST(StatusTest, ErrorFactoriesPreserveCategoryAndCombinedMessage) {
    // msg 与非空 msg2 以 ": " 连接，便于调用方附加 key 或文件名等上下文。
    const Status not_found = Status::NotFound(Slice("missing", 7),
                                              Slice("user key", 8));
    const Status corruption = Status::Corruption(Slice("bad checksum", 12));
    const Status not_supported = Status::NotSupported(Slice("snappy", 6));
    const Status invalid_argument = Status::InvalidArgument(Slice("empty path", 10));
    const Status io_error = Status::IOError(Slice("disk full", 9));

    EXPECT_TRUE(not_found.IsNotFound());
    EXPECT_EQ(not_found.ToString(), "NotFound: missing: user key");
    EXPECT_TRUE(corruption.IsCorruption());
    EXPECT_EQ(corruption.ToString(), "Corruption: bad checksum");
    EXPECT_TRUE(not_supported.IsNotSupportedError());
    EXPECT_EQ(not_supported.ToString(), "Not implemented: snappy");
    EXPECT_TRUE(invalid_argument.IsInvalidArgument());
    EXPECT_EQ(invalid_argument.ToString(), "Invalid argument: empty path");
    EXPECT_TRUE(io_error.IsIOError());
    EXPECT_EQ(io_error.ToString(), "IO error: disk full");
}

TEST(StatusTest, CopyAndMovePreserveOrTransferErrorState) {
    // 错误状态拥有独立 state_；复制不能借用源对象的状态块，移动后源对象变为 OK。
    Status original = Status::Corruption(Slice("bad block", 9));
    Status copied = original;
    Status assigned;
    assigned = original;

    EXPECT_EQ(copied.ToString(), "Corruption: bad block");
    EXPECT_EQ(assigned.ToString(), "Corruption: bad block");

    Status moved(std::move(original));
    EXPECT_TRUE(original.ok());
    EXPECT_TRUE(moved.IsCorruption());
    EXPECT_EQ(moved.ToString(), "Corruption: bad block");

    Status move_assigned;
    move_assigned = std::move(copied);
    EXPECT_TRUE(copied.ok());
    EXPECT_TRUE(move_assigned.IsCorruption());
    EXPECT_EQ(move_assigned.ToString(), "Corruption: bad block");
}

TEST(StatusTest, ToStringPreservesBinaryMessageBytes) {
    // Status 消息不是 C 字符串；诊断内容可包含嵌入的零字节。
    const std::string message("a\0b", 3);
    const Status status = Status::InvalidArgument(Slice(message));

    EXPECT_EQ(status.ToString(), std::string("Invalid argument: a\0b", 21));
}

}  // namespace
}  // namespace LSMKV
