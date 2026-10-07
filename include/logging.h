#pragma once 

#include "env.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace LSMKV {

class Slice;
class WritableFile;

// 同步转发 printf 风格的诊断消息，不拥有 logger，也不替代操作的 Status。
// logger 为 nullptr 时直接返回；否则 format 及参数须符合 Logger::Logv 约定。
void Log(Logger* logger, const char* format, ...);

bool ConsumeDecimalNumber(Slice* in, uint64_t* value);
}
