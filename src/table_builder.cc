#include "table_builder.h"

#include <cassert>
#include <cstdint>
#include <unistd.h>

#include "comparator.h"
#include "env.h"
#include "options.h"
#include "block_builder.h"
#include "format.h"
#include "coding.h"
#include "crc32c.h"
#include "table_format.h"

namespace LSMKV {

struct TableBuilder::Rep {
    Rep(const Options& opt, WritableFile* f) 
    : options(opt),
      index_block_options(opt),
      file(f),
      offset(0),
      data_block(&options),
      index_block(&index_block_options),
      num_entries(0),
      closed(false),
      pending_index_entry(false)
    {
        index_block_options.block_restart_interval = 1;
    }

Options options;
Options index_block_options;
WritableFile* file;
uint64_t offset;
Status status;
BlockBuilder data_block;
BlockBuilder index_block;
std::string last_key;
int64_t num_entries;
bool closed;

bool pending_index_entry;
BlockHandle pending_handle;

std::string compressed_output;
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {}

TableBuilder::~TableBuilder() {
    assert(rep_->closed);
    delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
    if (options.comparator != rep_->options.comparator) {
        return Status::InvalidArgument("changing comparator while building table");
    }

    rep_->options = options;
    rep_->index_block_options = options;
    rep_->index_block_options.block_restart_interval = 1;
    return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;
    if (r->num_entries > 0) {
        assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
    }

    if (r->pending_index_entry) {
        assert(r->data_block.empty());
        r->options.comparator->FindShortestSeparator(&r->last_key, key);
        std::string handle_encoding;
        r->pending_handle.EncodeTo(&handle_encoding);
        r->index_block.Add(r->last_key, Slice(handle_encoding));
        r->pending_index_entry = false;
    }   

    r->last_key.assign(key.data(), key.size());
    r->num_entries++;
    r->data_block.Add(key, value);

    const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
    if (estimated_block_size >= r->options.block_size) {
        Flush();
    }
}

void TableBuilder::Flush() {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;
    if (r->data_block.empty()) return;
    assert(!r->pending_index_entry);
    WriteBlock(&r->data_block, &r->pending_handle);
    if (ok()) {
        r->pending_index_entry = true;
        r->status = r->file->Flush();
    }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle)
{
    assert(ok());
    Rep* r = rep_;
    Slice raw = block->Finish();

    Slice block_contents;
    CompressionType type = r->options.compression;

    block_contents = raw;
    type  = CompressionType::kNoCompression;
    WriteRawBlock(block_contents, type, handle);
    r->compressed_output.clear();
    block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents, CompressionType type, BlockHandle* handle) {
    Rep* r = rep_;
    handle->set_offset(r->offset);
    handle->set_size(block_contents.size());
    r->status = r->file->Append(block_contents);

    if (r->status.ok()) {
        char trailer
    }
}
}