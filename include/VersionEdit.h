#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "format.h"
#include "status.h"

namespace LSMKV {

class VersionSet;

struct FileMetaData {
  int refs = 0;
  int allowed_seeks = 1 << 30;  // 后续用于基于 seek 的 compaction 统计。
  std::uint64_t number = 0;
  std::uint64_t file_size = 0;  // 文件大小，单位为字节。
  InternalKey smallest;        // 文件覆盖的最小 internal key（拥有编码）。
  InternalKey largest;         // 文件覆盖的最大 internal key（拥有编码）。
};

// 描述一次版本元数据变更；记录变更本身，不负责应用或持久化。
// 持有 comparator 名称、key 边界和文件元数据的副本。
// 修改同一实例需要调用方同步。
class VersionEdit {
 public:
  VersionEdit() = default;
  ~VersionEdit() = default;

  // 重置为不含任何变更的状态；has_* 区分“未设置”和“显式设置为零”。
  void Clear() {
    comparator_.clear();
    log_number_ = 0;
    prev_log_number_ = 0;
    next_file_number_ = 0;
    last_sequence_ = 0;
    has_comparator_ = false;
    has_log_number_ = false;
    has_prev_log_number_ = false;
    has_next_file_number_ = false;
    has_last_sequence_ = false;
    compact_pointers_.clear();
    deleted_files_.clear();
    new_files_.clear();
  }

  void SetComparatorName(const Slice& name) {
    comparator_ = name.ToString();
    has_comparator_ = true;
  }

  void SetLogNumber(std::uint64_t number) {
    log_number_ = number;
    has_log_number_ = true;
  }

  void SetPrevLogNumber(std::uint64_t number) {
    prev_log_number_ = number;
    has_prev_log_number_ = true;
  }

  void SetNextFile(std::uint64_t number) {
    next_file_number_ = number;
    has_next_file_number_ = true;
  }

  // sequence 必须不超过 kMaxSequenceNumber。
  void SetLastSequence(SequenceNumber sequence) {
    last_sequence_ = sequence;
    has_last_sequence_ = true;
  }

  // level 必须在 [0, 6] 范围内，key 必须是有效 internal key。
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.emplace_back(level, key);
  }

  // smallest/largest 为该文件的真实 key 边界，按 InternalKeyComparator 排序。
  void AddFile(int level, std::uint64_t number, std::uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    FileMetaData metadata;
    metadata.number = number;
    metadata.file_size = file_size;
    metadata.smallest = smallest;
    metadata.largest = largest;
    new_files_.emplace_back(level, std::move(metadata));
  }

  // 相同 (level, number) 的删除只记录一次；此方法不删除实际文件。
  void RemoveFile(int level, std::uint64_t number) {
    deleted_files_.emplace(level, number);
  }

  // 将 LevelDB 格式的变更记录追加到 dst，不清空 dst。
  void EncodeTo(std::string* dst) const;
  // 替换已有变更；输入损坏时返回 Corruption 且对象保持为空。
  // 仅检查编码合法性。应用元数据时须用实际比较器验证文件边界顺序。
  Status DecodeFrom(const Slice& src);
  std::string DebugString() const;

 private:
  friend class VersionSet;

  using DeletedFileSet = std::set<std::pair<int, std::uint64_t>>;

  std::string comparator_;
  std::uint64_t log_number_ = 0;
  std::uint64_t prev_log_number_ = 0;
  std::uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_prev_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace LSMKV
