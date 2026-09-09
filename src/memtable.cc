#include "memtable.h"

#include <cassert>
#include <cstring>

#include "coding.h"
#include "internal_key.h"
#include "lookup_key.h"

namespace LSMKV {
namespace {

Slice DecodeInternalKey(const char* entry)
{
    // entry 的首字段是 varint32 internal_key_size，返回的 Slice 不包含该长度字段。
    uint32_t internal_key_size = 0;
    
    const char* key_ptr = DecodeVarint32(entry, &internal_key_size);

    assert(key_ptr != nullptr);

    return Slice(key_ptr, internal_key_size);
}
}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const
{
    // SkipList 的 key 是 entry 地址，真正参与排序的是其中的 InternalKey。
    Slice a_key = DecodeInternalKey(a);
    Slice b_key = DecodeInternalKey(b);

    InternalKeyComparator comparator;

    return comparator.Compare(a_key, b_key);
}

MemTable::MemTable()
    : arena_(),
      comparator_(),
      table_(comparator_, &arena_) {}


void MemTable::Add(SequenceNumber sequence, ValueType type, const Slice& key, const Slice& value)
{
    // entry 布局：varint32(internal_key_size) | user_key | fixed64(tag)
    //            | varint32(value_size) | value。
    const size_t key_size = key.size();

    const size_t value_size = value.size();

    const size_t internal_key_size = key_size + 8;

    const size_t encoded_size = VarintLength(internal_key_size) + internal_key_size + VarintLength(value_size) + value_size;

    char* buffer = arena_.Allocate(encoded_size);
    
    char* p = buffer;
    p = EncodeVarint32(p, static_cast<uint32_t> (internal_key_size));

    std::memcpy(p, key.data(), key.size());
    p += key.size();

    EncodeFixed64(p, PackSequenceAndType(sequence, type));
    p += 8;

    p = EncodeVarint32(p, static_cast<uint32_t>(value_size));

    std::memcpy(p, value.data(), value.size());

    p += value.size();

    assert(static_cast<size_t> (p - buffer) == encoded_size);

    table_.Insert(buffer);
}

MemTable::GetResult MemTable::Get(const LookupKey& key, std::string* value) const
{
    Iterator iter(this);

    // InternalKeyComparator 按 sequence 降序排列，因此 lower_bound 正好找到
    // 不晚于 snapshot 的最新候选版本。
    iter.Seek(key.InternalKey());

    if (!iter.Valid()) {
        return GetResult::kNotFound;
    }

    Slice internal_key = iter.key();

    Slice user_key = ExtractUserKey(internal_key);

    // Seek 可能越过目标 UserKey；必须再次确认用户键一致。
    if (user_key.Compare(key.UserKey()) != 0)
    {
        return GetResult::kNotFound;
    }

    const uint64_t tag = DecodeFixed64(internal_key.data() + internal_key.size() - 8);

    const ValueType type = static_cast<ValueType> (tag & 0xff);

    // Tombstone 屏蔽更旧版本，不能等同于 MemTable 未命中。
    if (type == ValueType::kDeletion) 
    {
        return GetResult::kDeleted;
    }

    assert (type == ValueType::kValue);

    if (value != nullptr) {
        *value = iter.value().ToString();
    }

    return GetResult::kFound;
}

void MemTable::Iterator::Seek(const Slice& internal_key)
{
    // Table 的比较器预期 [varint32 internal_key_size][internal_key] 格式，
    // 因此公开 Iterator 接收 InternalKey，并在这里补上私有编码前缀。
    const size_t encoded_size =
        VarintLength(internal_key.size()) + internal_key.size();
    seek_key_.resize(encoded_size);

    char* p = seek_key_.data();
    p = EncodeVarint32(p, static_cast<uint32_t>(internal_key.size()));
    std::memcpy(p, internal_key.data(), internal_key.size());

    iter_.Seek(seek_key_.data());
}

Slice MemTable::Iterator::key() const 
{
    assert(Valid());
    
    return DecodeInternalKey(iter_.key());
}

Slice MemTable::Iterator::value() const 
{
    assert(Valid());

    const char* entry = iter_.key();

    uint32_t internal_key_size = 0;
    const char* p = DecodeVarint32(entry, &internal_key_size);

    assert(p != nullptr);

    // 跳过 InternalKey 后，紧随其后的 varint32 才是 value 的长度。
    p += internal_key_size;

    uint32_t value_size = 0;
    p = DecodeVarint32(p, &value_size);

    assert(p != nullptr);

    return Slice(p, value_size);
}

Slice MemTable::GetInternalKey(const char* entry)
{
    return DecodeInternalKey(entry);
}

const char* MemTable::GetValuePointer(const char* entry)
{
    uint32_t internal_key_size = 0;

    const char* p = DecodeVarint32(entry, &internal_key_size);

    assert(p != nullptr);

    return p + internal_key_size;
}
}
