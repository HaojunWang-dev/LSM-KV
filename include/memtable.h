#pragma once

#include <cstddef>
#include <string>

#include "arena.h"
#include "skiplist.h"
#include "internal_key.h"
#include "lookup_key.h"
#include "slice.h"

namespace LSMKV {

// MemTable 将编码后的 entry 存入 Arena，并由 SkipList 按 InternalKey 顺序索引。
class MemTable {
private:
    // SkipList 保存 entry 地址；比较时先解码 entry 中的 InternalKey。
    struct KeyComparator {
        int operator()(const char* a, const char* b) const;
    };

    using Table = SkipList<const char*, KeyComparator>;

public:
    // kDeleted 表示当前 MemTable 已命中 tombstone，调用方不应继续查找旧版本。
    enum class GetResult {
        kNotFound,
        kFound,
        kDeleted,
    };

    MemTable();

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // 写入一个不可变版本；Deletion 同样作为 entry 插入，而不是物理删除旧版本。
    void Add(SequenceNumber number, ValueType type, const Slice& key, const Slice& value);

    // 查找 snapshot 可见的最新版本，并区分未命中、值与 tombstone。
    GetResult Get(const LookupKey& key, std::string* value) const;

    size_t ApproxiamateMemoryUsage() const
    {
        return arena_.MemoryUsage();
    }

    // 将底层 entry 解析为 InternalKey 与 value，供扫描和后续 flush 使用。
    class Iterator 
    {
    public:
        explicit Iterator(const MemTable* memtable)
            : iter_(&memtable->table_) {}

        bool Valid() const {
            return iter_.Valid();
        }

        void SeekToFirst() {
            iter_.SeekToFirst();
        }

        // lower_bound 查找：定位第一个不小于 internal_key 的 MemTable entry。
        void Seek(const Slice& internal_key);

        void Next() {
            iter_.Next();
        }

        // 返回 entry 中的 InternalKey；Slice 指向 MemTable Arena 管理的内存。
        Slice key() const;

        // 返回 entry 中的 value；其生命周期同样不超过所属 MemTable。
        Slice value() const;

    private:
        typename Table::Iterator iter_;
        // 底层 SkipList 需要带长度前缀的 entry key，临时缓冲区由 Iterator 持有。
        std::string seek_key_;
    };

private:
    static Slice GetInternalKey(const char* entry);

    static const char* GetValuePointer(const char* entry);

private:
    Arena arena_;
    KeyComparator comparator_;
    Table table_;
};
}
