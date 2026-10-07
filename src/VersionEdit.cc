#include "VersionEdit.h"

#include <sstream>
#include <utility>

#include "coding.h"

namespace LSMKV {
namespace {

// LevelDB 版本编辑格式支持 level 0..6；编码 tag 是稳定的存储格式。
constexpr std::uint32_t kNumLevels = 7;

enum class Tag : std::uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
  // 8 was used for large value refs
  kPrevLogNumber = 9
};

bool GetInternalKey(Slice* input, InternalKey* dst) {
  Slice encoded;
  ParsedInternalKey parsed;
  if (!GetLengthPrefixedSlice(input, &encoded) ||
      !ParseInternalKey(encoded, &parsed)) {
    return false;
  }
  return dst->DecodeFrom(encoded);
}

bool GetLevel(Slice* input, int* level) {
  std::uint32_t decoded;
  if (!GetVarint32(input, &decoded) || decoded >= kNumLevels) {
    return false;
  }
  *level = static_cast<int>(decoded);
  return true;
}

// 二进制名称和 key 以转义文本输出，避免 NUL 或换行扰乱诊断。
std::string EscapeBytes(const Slice& bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto byte = static_cast<unsigned char>(bytes.data()[i]);
    if (byte >= 0x20 && byte <= 0x7e && byte != '\\' && byte != '\'') {
      result.push_back(static_cast<char>(byte));
    } else {
      result.append("\\x");
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 0x0f]);
    }
  }
  return result;
}

std::string KeyDebugString(const InternalKey& key) {
  ParsedInternalKey parsed;
  if (!ParseInternalKey(key.Encode(), &parsed)) {
    return "(bad)" + EscapeBytes(key.Encode());
  }
  return "'" + EscapeBytes(parsed.user_key) + "' @ " +
         std::to_string(parsed.sequence) + " : " +
         std::to_string(static_cast<unsigned>(parsed.type));
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kComparator));
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kLogNumber));
    PutVarint64(dst, log_number_);
  }
  if (has_prev_log_number_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kPrevLogNumber));
    PutVarint64(dst, prev_log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kNextFileNumber));
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kLastSequence));
    PutVarint64(dst, last_sequence_);
  }

  for (size_t i = 0; i < compact_pointers_.size(); i++) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kCompactPointer));
    PutVarint32(dst, compact_pointers_[i].first);  // level
    PutLengthPrefixedSlice(dst, compact_pointers_[i].second.Encode());
  }

  for (const auto& deleted_file_kvp : deleted_files_) {
    PutVarint32(dst, static_cast<uint32_t>(Tag::kDeletedFile));
    PutVarint32(dst, deleted_file_kvp.first);   // level
    PutVarint64(dst, deleted_file_kvp.second);  // file number
  }

  for (size_t i = 0; i < new_files_.size(); i++) {
    const FileMetaData& f = new_files_[i].second;
    PutVarint32(dst, static_cast<uint32_t>(Tag::kNewFile));
    PutVarint32(dst, new_files_[i].first);  // level
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode());
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  // 临时对象拥有已解析字节；仅在整条记录有效时发布结果。
  VersionEdit decoded;
  Slice input = src;
  while (!input.empty()) {
    std::uint32_t raw_tag;
    if (!GetVarint32(&input, &raw_tag)) {
      return Status::Corruption("VersionEdit", "invalid tag");
    }

    const char* error = nullptr;
    switch (static_cast<Tag>(raw_tag)) {
      case Tag::kComparator: {
        Slice name;
        if (!GetLengthPrefixedSlice(&input, &name)) {
          error = "comparator name";
        } else {
          decoded.SetComparatorName(name);
        }
        break;
      }
      case Tag::kLogNumber:
      case Tag::kPrevLogNumber:
      case Tag::kNextFileNumber:
      case Tag::kLastSequence: {
        std::uint64_t number;
        if (!GetVarint64(&input, &number)) {
          error = "invalid or truncated number";
          break;
        }
        switch (static_cast<Tag>(raw_tag)) {
          case Tag::kLogNumber:
            decoded.SetLogNumber(number);
            break;
          case Tag::kPrevLogNumber:
            decoded.SetPrevLogNumber(number);
            break;
          case Tag::kNextFileNumber:
            decoded.SetNextFile(number);
            break;
          case Tag::kLastSequence:
            if (number > kMaxSequenceNumber) {
              error = "last sequence exceeds 56 bits";
            } else {
              decoded.SetLastSequence(number);
            }
            break;
          default:
            break;
        }
        break;
      }
      case Tag::kCompactPointer: {
        int level;
        InternalKey key;
        if (!GetLevel(&input, &level) || !GetInternalKey(&input, &key)) {
          error = "compaction pointer";
        } else {
          decoded.SetCompactPointer(level, key);
        }
        break;
      }
      case Tag::kDeletedFile: {
        int level;
        std::uint64_t number;
        if (!GetLevel(&input, &level) || !GetVarint64(&input, &number)) {
          error = "deleted file";
        } else {
          decoded.RemoveFile(level, number);
        }
        break;
      }
      case Tag::kNewFile: {
        int level;
        FileMetaData file;
        if (!GetLevel(&input, &level) ||
            !GetVarint64(&input, &file.number) ||
            !GetVarint64(&input, &file.file_size) ||
            !GetInternalKey(&input, &file.smallest) ||
            !GetInternalKey(&input, &file.largest)) {
          error = "new-file entry";
        } else {
          // 顺序取决于数据库实际配置的 comparator；解码阶段只验证编码。
          decoded.AddFile(level, file.number, file.file_size,
                          file.smallest, file.largest);
        }
        break;
      }
      default:
        error = "unknown tag";
        break;
    }

    if (error != nullptr) {
      return Status::Corruption("VersionEdit", error);
    }
  }
  *this = std::move(decoded);
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::ostringstream out;
  out << "VersionEdit {";
  if (has_comparator_) {
    out << "\n  Comparator: " << EscapeBytes(Slice(comparator_));
  }
  if (has_log_number_) {
    out << "\n  LogNumber: " << log_number_;
  }
  if (has_prev_log_number_) {
    out << "\n  PrevLogNumber: " << prev_log_number_;
  }
  if (has_next_file_number_) {
    out << "\n  NextFile: " << next_file_number_;
  }
  if (has_last_sequence_) {
    out << "\n  LastSeq: " << last_sequence_;
  }
  for (const auto& pointer : compact_pointers_) {
    out << "\n  CompactPointer: " << pointer.first << ' '
        << KeyDebugString(pointer.second);
  }
  for (const auto& file : deleted_files_) {
    out << "\n  RemoveFile: " << file.first << ' ' << file.second;
  }
  for (const auto& entry : new_files_) {
    const FileMetaData& file = entry.second;
    out << "\n  AddFile: " << entry.first << ' ' << file.number << ' '
        << file.file_size << ' ' << KeyDebugString(file.smallest)
        << " .. " << KeyDebugString(file.largest);
  }
  out << "\n}\n";
  return out.str();
}

}  // namespace LSMKV
