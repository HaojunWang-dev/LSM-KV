#pragma once

#include <cstddef>
#include <string>

#include "slice.h"
#include "status.h"

namespace LSMKV {

class WriteBatchInternal;

// WriteBatch 保存一组按添加顺序执行的更新。
// DB 写路径会将整个 batch 原子地写入 WAL，再按相同顺序应用到 MemTable。
class WriteBatch {
public:
    // Handler 将序列化 batch 解码为逻辑写操作；Iterate() 的调用方实现它。
    class Handler {
    public:
        virtual ~Handler();

        // 应用一个 key -> value 写入操作。
        virtual void Put(const Slice& key, const Slice& value) = 0;
        // 应用一个 key 的删除操作（即写入 tombstone）。
        virtual void Delete(const Slice& key) = 0;
    };

    // 创建空 batch；实现会预留 sequence 与操作数量的编码头。
    WriteBatch();
    ~WriteBatch();

    // rep_ 是值语义字符串，默认复制可安全复制整批操作。
    WriteBatch(const WriteBatch&) = default;
    WriteBatch& operator=(const WriteBatch&) = default;

    // 向 batch 末尾追加 Put，保留相同 key 的历史操作顺序。
    void Put(const Slice& key, const Slice& value);
    // 向 batch 末尾追加 Delete；不在 batch 构造阶段物理删除先前操作。
    void Delete(const Slice& key);

    // 清除全部逻辑操作，并恢复为空 batch 的编码头。
    void Clear();

    // 返回当前序列化表示的近似字节数，用于写路径统计。
    size_t ApproximateSize() const;

    // 将 source 的操作追加到当前 batch，目标 batch 保留自己的 sequence。
    void Append(const WriteBatch& source);

    // 按编码中的操作顺序回调 handler；格式错误时返回 Corruption Status。
    Status Iterate(Handler* handler) const;

private:
    friend class WriteBatchInternal;

    // 由 write_batch.cc 定义的二进制布局：
    // [fixed64 sequence][fixed32 count][records...]
    std::string rep_;
};

}  // namespace LSMKV
