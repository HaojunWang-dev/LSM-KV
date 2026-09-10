#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "log_writer.h"

namespace LSMKV {
namespace log {
namespace {

// Writer 的公开接口必须允许 DB 写路径传入一个仍由调用方持有的文件对象，
// 并可从空文件或已存在的 WAL 末尾开始写入。
TEST(LogWriterTest, ExposesLevelDBStyleConstructionAndAppendApi) {
  static_assert(std::is_constructible_v<Writer, WritableFile*>);
  static_assert(
      std::is_constructible_v<Writer, WritableFile*, std::uint64_t>);
  static_assert(std::is_same_v<decltype(&Writer::AddRecord),
                               Status (Writer::*)(const Slice&)>);
}

}  // namespace
}  // namespace log
}  // namespace LSMKV
