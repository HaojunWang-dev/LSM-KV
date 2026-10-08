/*
Data Block:
+-----------------------------------+
| Entry 0                           |
+-----------------------------------+
| Entry 1                           |
+-----------------------------------+
| Entry 2                           |
+-----------------------------------+
| ...                               |
+-----------------------------------+
| restart offset 0        fixed32   |
+-----------------------------------+
| restart offset 1        fixed32   |
+-----------------------------------+
| restart offset 2        fixed32   |
+-----------------------------------+
| num_restarts            fixed32   |
+-----------------------------------+

Entry:
[shared]
[non_shared]
[value_length]
[non_shared_key_bytes]
[value]

*/

#pragma once 

#include <cstddef>
#include <cstdint>

#include "comparator.h"
#include "iterator.h"
#include "table_format.h"

namespace LSMKV {

struct BlockContents;
struct Comparator;

class Block {

public:
    explicit Block(const BlockContents& contents);

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    ~Block();

    size_t size() const { return size_; }
    Iterator* NewIterato(const Comparator* comparator);

private:
    class Iter;

    uint32_t NumRestarts() const;

    const char* data_;
    size_t size_;
    uint32_t restart_offset_;
    bool owned_;
};
}