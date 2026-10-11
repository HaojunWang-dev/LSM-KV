#pragma once 

#include "iterator.h"
#include "options.h"

namespace LSMKV {

Iterator* NewTwoLevelIterator(Iterator* index_iter, Iterator* (block_function)(void* arg, const ReadOptions& options, const Slice& index_value));
}