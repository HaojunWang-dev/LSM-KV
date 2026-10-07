#pragma once 

#include "env.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace LSMKV {

class Slice;
class WritableFile;

bool ConsumeDecimalNumber(Slice* in, uint64_t* value);
}