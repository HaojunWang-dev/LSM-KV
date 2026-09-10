#pragma once

#include <cstddef>

#include "internal_key.h"
#include "status.h"
#include "write_batch.h"

namespace LSMKV {

class MemTable;

// WriteBatchInternal 提供 DB 写路径、WAL 与恢复逻辑需要的内部操作。
// 这些函数不属于公开 WriteBatch API，避免调用方依赖 rep_ 的二进制布局。
class WriteBatchInternal {
public:
    // 返回 batch 中记录的逻辑操作数量。
    static int Count(const WriteBatch* batch);
    // 更新编码头中的操作数量。
    static void SetCount(WriteBatch* batch, int count);

    // 返回 batch 第一条操作使用的 sequence。
    static SequenceNumber Sequence(const WriteBatch* batch);
    // 设置 batch 的起始 sequence；连续操作依次使用 sequence + offset。
    static void SetSequence(WriteBatch* batch, SequenceNumber sequence);

    // 返回 WAL 可直接持久化的原始编码内容；Slice 借用 batch 的 rep_。
    static Slice Contents(const WriteBatch* batch) {
        return Slice(batch->rep_);
    }

    // 返回原始编码的字节数，包含 sequence、count 与所有操作记录。
    static size_t ByteSize(const WriteBatch* batch) {
        return batch->rep_.size();
    }

    // 用 WAL 读出的完整编码替换 batch 内容；格式验证由调用方或 Iterate 完成。
    static void SetContents(WriteBatch* batch, const Slice& contents);

    // 按 batch 顺序写入 MemTable，并为每条操作分配连续 sequence。
    static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

    // 高效拼接 src 操作到 dst，并同步调整 dst 的操作数量。
    static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace LSMKV
