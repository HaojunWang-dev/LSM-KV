#include "two_level_iterator.h"

#include "iterator.h"
#include "iterator_wrapper.h"
#include "options.h"
#include "table.h"
#include "table_block.h"
#include "table_format.h"

namespace LSMKV {

namespace {

typedef Iterator* (*BlockFunction) (void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
public:

    TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options);

    ~TwoLevelIterator() override;

    void Seek(const Slice& target) override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Next() override;
    void Prev() override;

    bool Valid() const override { return data_iter_.Valid(); }

    Status status() const override {
    // It'd be nice if status() returned a const Status& instead of a Status
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
      return data_iter_.status();
    } else {
      return status_;
    }
  }
private:
    void SaveError(const Status& s) {
        if (status_.ok() && !s.ok()) status_ = s;
    }

    void SkipEmptyDataBlocksForward();
    void SkipEmptyDataBlocksBackward();
    void SetDataIterator(Iterator* data_iter);
    void InitDataBlock(); 

    BlockFunction block_function_;
    void* arg_;
    const ReadOptions options_;
    Status status_;
    IteratorWrapper index_iter_;
    IteratorWrapper data_iter_;

    std::string data_block_handle_;
};
}
}