#include "log_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "coding.h"
#include "crc32c.h"
#include "env.h"
#include "log_format.h"

namespace LSMKV {

namespace log {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               std::uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      skipped_initial_block_(false),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }

bool Reader::SkipToInitialBlock() {
  if (skipped_initial_block_) {
    return true;
  }

  const size_t offset_in_block = initial_offset_ % kBlockSize;
  std::uint64_t block_start_location = initial_offset_ - offset_in_block;

  // block 尾部不足一个 header 时，从下一个 block 开始恢复。
  if (offset_in_block > kBlockSize - kHeaderSize) {
    block_start_location += kBlockSize;
  }

  end_of_buffer_offset_ = block_start_location;

  if (block_start_location > 0) {
    Status status = file_->Skip(block_start_location);
    if (!status.ok()) {
      ReportDrop(block_start_location, status);
      return false;
    }
  }
  skipped_initial_block_ = true;
  return true;
}

void Reader::ReportCorruption(std::uint64_t bytes, const char *reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status &reason) {
  if (reporter_ != nullptr &&
      end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

std::uint64_t Reader::LastRecordOffset() { return last_record_offset_; }

bool Reader::ReadRecord(Slice *record, std::string *scratch) {
  if (!SkipToInitialBlock()) {
    return false;
  }

  scratch->clear();
  record->clear();

  bool in_fragmented_record = false;
  std::uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    const auto record_type = ReadPhysicalRecord(&fragment);

    const bool is_disk_record_type =
        record_type >= RecordType::kFullType &&
        record_type <= RecordType::kLastType;
    const std::uint64_t physical_record_offset =
        is_disk_record_type
        ? end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size()
        : 0;

    if (resyncing_) {
      // 跳过恢复位置之前遗留的分片。
      if (record_type == RecordType::kMiddleType) {
        continue;
      }
      if (record_type == RecordType::kLastType) {
        resyncing_ = false;
        continue;
      }
      resyncing_ = false;
    }

    switch (record_type) {
    case RecordType::kFullType:
      if (in_fragmented_record && !scratch->empty()) {
        ReportCorruption(scratch->size(), "partial record without end(1)");
      }

      prospective_record_offset = physical_record_offset;
      scratch->clear();
      *record = fragment;
      last_record_offset_ = prospective_record_offset;
      return true;

    case RecordType::kFirstType:
      if (in_fragmented_record && !scratch->empty()) {
        ReportCorruption(scratch->size(), "partial record without end(2)");
      }

      prospective_record_offset = physical_record_offset;
      scratch->assign(fragment.data(), fragment.size());
      in_fragmented_record = true;
      break;

    case RecordType::kMiddleType:
      if (!in_fragmented_record) {
        ReportCorruption(fragment.size(),
                         "missing start of fragmented record(1)");
      } else {
        scratch->append(fragment.data(), fragment.size());
      }
      break;

    case RecordType::kLastType:
      if (!in_fragmented_record) {
        ReportCorruption(fragment.size(),
                         "missing start of fragmented record(2)");
      } else {
        scratch->append(fragment.data(), fragment.size());
        *record = Slice(*scratch);
        last_record_offset_ = prospective_record_offset;
        return true;
      }
      break;

    case RecordType::kEof:
      if (in_fragmented_record) {
        scratch->clear();
      }
      return false;

    case RecordType::kBadRecord:
      if (in_fragmented_record) {
        ReportCorruption(scratch->size(), "error in middle of record");
        in_fragmented_record = false;
        scratch->clear();
      }
      break;

    default:
      ReportCorruption(
          fragment.size() + (in_fragmented_record ? scratch->size() : 0),
          "unknown record type");
      in_fragmented_record = false;
      scratch->clear();
      break;
    }
  }

  return false;
}
RecordType Reader::ReadPhysicalRecord(Slice *result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        buffer_.clear();

        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return RecordType::kEof;
        }
        if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      }

      buffer_.clear();
      return RecordType::kEof;
    }

    const char *header = buffer_.data();
    const uint32_t low = static_cast<uint32_t>(header[4]) & 0xffU;
    const uint32_t high = static_cast<uint32_t>(header[5]) & 0xffU;
    const RecordType type =
        static_cast<RecordType>(static_cast<uint8_t>(header[6]));
    const uint32_t length = low | (high << 8U);

    if (kHeaderSize + length > buffer_.size()) {
      const size_t drop_size = buffer_.size();
      buffer_.clear();

      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return RecordType::kBadRecord;
      }

      return RecordType::kEof;
    }

    if (type < RecordType::kFullType || type > RecordType::kLastType) {
      const size_t drop_size = buffer_.size();
      buffer_.clear();
      ReportCorruption(drop_size, "invalid record type");
      return RecordType::kBadRecord;
    }

    if (checksum_) {
      // WAL checksum 覆盖 record type 和 payload。
      const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      const uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        const size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return RecordType::kBadRecord;
      }
    }

    *result = Slice(header + kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);
    return type;
  }
}
} // namespace log
} // namespace LSMKV
