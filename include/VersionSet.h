#pragma once 

#include "env.h"
#include "format.h"
#include "VersionEdit.h"
#include "log_writer.h"
#include "memtable.h"
#include "slice.h"
#include "options.h"

#include <map>
#include <set>
#include <vector>
#include <mutex>

namespace LSMKV {

namespace log {
class Writer;
}

class Compaction;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;


int findFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, const Slice& key);

bool SomeFileOverlapsRange( const InternalKeyComparator& icmp,
                            bool disjoint_sorted_files,
                            const std::vector<FileMetaData*>& files,
                            const Slice* smallest_user_key,
                            const Slice* largest_user_key);

class Version {

public:
    struct GetStats {
        FileMetaData* seek_file;
        int seek_file_level;
    };

    void AddIterator(const ReadOptions&, std::vector<Iterator*>* iters);


    Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

    bool UpdateStats(const GetStats& stats);

    bool RecordReadSample(Slice key);

    void Ref();
    void Unref();


    void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs);
    
    bool OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key);

    int PickLevelForMemtableOutput(const Slice& smallest_user_key, const Slice& largest_user_key);

    int NumFiles(int level) const {return files_[level].size();}

    std::string DebugString() const;

private:
    friend class Compaction;
    friend class VersionSet;
    friend class VersionSetTestPeer;

    class LevelFileNumIterator;

    explicit Version(VersionSet* vset)
        : vset_(vset),
          next_(this),
          prev_(this),
          refs_(0),
          file_to_compact_(nullptr),
          file_to_compact_level_(-1),
          compaction_score_(-1),
          compaction_level_(-1) {}
   
    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    ~Version();

    Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

    void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (*func)(void*, int, FileMetaData*));

    VersionSet* vset_;
    Version* next_;
    Version* prev_;
    int refs_;

    std::vector<FileMetaData*> files_[config::kNumLevels];
    FileMetaData* file_to_compact_;
    int file_to_compact_level_;

    double compaction_score_;
    int compaction_level_;
};

// 管理版本链表和基础元数据；对同一实例的访问需要调用方同步。
class VersionSet {
public:
    VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache, const InternalKeyComparator*);
    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    ~VersionSet();

    Status LogAndApply(VersionEdit* edit, std::mutex* mu);

    // 返回借用的当前版本；跨版本切换继续使用时，调用方须 Ref()/Unref()。
    Version* current() { return current_; }

    // 分配新的文件编号；不同文件不能复用仍在使用的编号。
    // Debug contract：next_file_number_ 必须小于 UINT64_MAX，避免递增回绕。
    uint64_t NewFileNumber();

    // 仅回收最近一次分配且之后未发生新分配的编号。
    void ReuseFileNumber(uint64_t file_number);

    // 恢复时标记已使用的编号，使后续分配的编号大于它；编号须小于 UINT64_MAX。
    void MarkFileNumberUsed(uint64_t file_number);

    // sequence 必须单调不减，并且不超过 kMaxSequenceNumber。
    SequenceNumber LastSequence() const;
    void SetLastSequence(SequenceNumber sequence);

    // 查询当前 WAL、前一个尚需保留的 WAL 和 MANIFEST 的文件编号。
    uint64_t LogNumber() const;
    uint64_t PrevLogNumber() const;
    uint64_t ManifestFileNumber() const;

    // 查询当前版本的文件数量和总字节数；level 必须在 [0, kNumLevels) 内。
    // 总字节数必须能用 int64_t 表示；非法参数或溢出违反 Debug contract。
    int NumLevelFiles(int level) const;
    int64_t NumLevelBytes(int level) const;

    // 向 live 追加所有存活版本引用的文件编号，不清空已有内容；live 不能为 nullptr。
    // 这里只收集编号，不删除文件；旧版本仍在使用时，其文件也必须保留。
    void AddLiveFiles(std::set<uint64_t>* live);

private:
    class Builder;
    
    friend class Version;
    friend class Compaction;
    friend class VersionSetTestPeer;

    void Finalize(Version* v);

    Status WriteSnapshot(log::Writer* log);

    // 安装已验证的新版本，维护版本链表和 current_ 的引用；不负责持久化。
    void AppendVersion(Version* version);

    Env* const env_;
    const std::string dbname_;
    const Options* const options_;
    TableCache* const table_cache_;
    const InternalKeyComparator icmp_;
    uint64_t next_file_number_;
    uint64_t manifest_file_number_;
    uint64_t last_sequence_;
    uint64_t log_number_;
    uint64_t prev_log_number_;

    WritableFile* descriptor_file_;
    log::Writer* descriptor_log_;
    Version dummy_versions_;
    Version* current_;

    std::string compact_pointer_[config::kNumLevels];
};
}
