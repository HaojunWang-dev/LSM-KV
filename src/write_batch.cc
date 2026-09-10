#include "write_batch.h"
#include "coding.h"
#include "internal_key.h"
#include "memtable.h"
#include "status.h"
#include "write_batch_internal.h"
#include <cstdint>

namespace LSMKV {

static const size_t kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::~WriteBatch() = default;

WriteBatch::Handler::~Handler() = default;

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

Status WriteBatch::Iterate(Handler *handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value;
  int found = 0;

  while (!input.empty()) {
    found++;
    char tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    switch (tag) {
    case static_cast<uint8_t>(ValueType::kValue):
      if (GetLengthPrefixedSlice(&input, &key) &&
          GetLengthPrefixedSlice(&input, &value)) {
        handler->Put(key, value);
      } else {
        return Status::Corruption("bad writebatch Put");
      }
      break;
    case static_cast<uint8_t>(ValueType::kDeletion):
      if (GetLengthPrefixedSlice(&input, &key)) {
        handler->Delete(key);
      } else {
        return Status::Corruption("bad writebatch Delete");
      }
      break;
    default:
      return Status::Corruption("unkown writebatch tag");
    }
  }

  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("writebatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch *batch) {
  return DecodeFixed32(batch->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch *batch, int count) {
  EncodeFixed32(&batch->rep_[8], count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch *batch) {
  return DecodeFixed64(batch->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch *batch,
                                     SequenceNumber sequence) {
  EncodeFixed64(batch->rep_.data(), sequence);
}

void WriteBatch::Put(const Slice &key, const Slice &value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice &key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch &source) {
  WriteBatchInternal::Append(this, &source);
}

namespace {

class MemTableInserter : public WriteBatch::Handler {
public:
  SequenceNumber sequence_;
  MemTable *mem_;

  void Put(const Slice &key, const Slice &value) override {
    mem_->Add(sequence_, ValueType::kValue, key, value);
    sequence_++;
  }

  void Delete(const Slice &key) override {
    mem_->Add(sequence_, ValueType::kDeletion, key, Slice());
    sequence_++;
  }
};
} // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch *batch,
                                      MemTable *memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(batch);
  inserter.mem_ = memtable;
  return batch->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch *batch, const Slice &contents) {
  assert(contents.size() >= kHeader);
  batch->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch *dst, const WriteBatch *src) {
  SetCount(dst, Count(src) + Count(dst));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}
} // namespace LSMKV
