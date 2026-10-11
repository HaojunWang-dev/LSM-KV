#pragma once

#include <cstdint>

#include "iterator.h"
#include "table_format.h"

namespace LSMKV {

class Block;
class BlockHandle;
class Footer;
struct Options;
class RandomAccessFile;
struct ReadOptions;
class TableCache;

class Table {
public:

    static Status Open(const Options& options, RandomAccessFile* file, uint64_t file_size, Table** table);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    ~Table();

    Iterator* NewIterator(const ReadOptions&) const;
    
    uint64_t ApproximateOffset(const Slice& key) const;
    
private:
    friend class TableCache;
    friend class TableTestPeer;  // Tests access InternalGet without a public API.
    struct Rep;

    static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

    explicit Table(Rep* rep) : rep_(rep) {};

    Status InternalGet(const ReadOptions&, const Slice& key, void* arg, void (*handle_result)(void* arg, const Slice& k, const Slice& v));

    void ReadMeta(const Footer& footer);
    void ReadFilter(const Slice& filter_handle_value);

    Rep* const rep_;
};
}
