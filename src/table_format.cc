#include "table_format.h"

#include "env.h"
#include "options.h"
#include "coding.h"
#include "crc32c.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>

namespace LSMKV {

void BlockHandle::EncodeTo(std::string* dst) const{
    assert(offset_ != ~static_cast<uint64_t>(0));
    assert(size_ != ~static_cast<uint64_t>(0));

    PutVarint64(dst, offset_);
    PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
    Slice remaining = *input;
    uint64_t offset, size;
    if (!GetVarint64(&remaining, &offset) || !GetVarint64(&remaining, &size)) {
        return Status::Corruption("bad block handle");
    }
    offset_ = offset;
    size_ = size;
    *input = remaining;
    return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
    const size_t original_size = dst->size();
    metaindex_handle_.EncodeTo(dst);
    index_handle_.EncodeTo(dst);

    dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength);
    PutFixed64(dst, kTableMagicNumber);
    assert(dst->size() == original_size + kEncodedLength);
    (void) original_size;
}

Status Footer::DecodeFrom(Slice* input) {
    if (input->size() < kEncodedLength) {
        return Status::Corruption("not an sstable (footer too short)");
    }

    const char* magic_ptr = input->data() + kEncodedLength - 8;
    const uint32_t magic_lo = DecodeFixed32(magic_ptr);
    const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
    const uint64_t magic = (static_cast<uint64_t>(magic_hi) << 32 | static_cast<uint64_t> (magic_lo));
    if (magic != kTableMagicNumber) {
        return Status::Corruption("not an sstable (bad magic number)");
    }

    // Parse into temporary handles within the handle area, then commit once.
    Slice handles(input->data(), 2 * BlockHandle::kMaxEncodedLength);
    BlockHandle metaindex, index;
    Status result = metaindex.DecodeFrom(&handles);
    if (!result.ok()) return result;
    result = index.DecodeFrom(&handles);
    if (!result.ok()) return result;

    metaindex_handle_ = metaindex;
    index_handle_ = index;
    input->remove_prefix(kEncodedLength);
    return Status::OK();
}

/*
+----------------+
| contents       | n bytes
+----------------+
                 ↑ data+n
| type           | 1 byte
+----------------+
                 ↑ data+n+1
| crc            | 4 bytes
*/

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle, BlockContents* result) {
    assert(file != nullptr && result != nullptr);
    // Validate before narrowing, allocating, or calling the file implementation.
    if (handle.size() > std::numeric_limits<size_t>::max() - kBlockTrailerSize ||
        handle.size() > std::numeric_limits<uint64_t>::max() - kBlockTrailerSize ||
        handle.size() > static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max()) -
                            kBlockTrailerSize) {
        return Status::Corruption("block size overflow");
    }
    const uint64_t read_size = handle.size() + kBlockTrailerSize;
    if (handle.offset() > std::numeric_limits<uint64_t>::max() - read_size) {
        return Status::Corruption("block range overflow");
    }
    const size_t n = static_cast<size_t>(handle.size());
    std::unique_ptr<char[]> buf(new char[n + kBlockTrailerSize]);
    Slice contents;
    Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf.get());
    if (!s.ok()) {
        return s;
    }
    if (contents.size() != n + kBlockTrailerSize) {
        return Status::Corruption("truncated block read");
    }

    const char* data = contents.data();
    if (options.verify_checksums) {
        //type and the block contents
        const uint32_t crc = crc32c::Unmask((DecodeFixed32(data + n + 1)));
        const uint32_t actual = crc32c::Value(data, n + 1);
        if (actual != crc) {
            return Status::Corruption("block checksum mismatch");
        }
    }

    BlockContents decoded;
    switch (static_cast<CompressionType>(static_cast<std::uint8_t>(data[n]))) {
        case CompressionType::kNoCompression:
            if (data != buf.get()) {
                decoded.data = Slice(data, n);
            } else {
                decoded.data = Slice(buf.release(), n);
                decoded.heap_allocated = true;
                decoded.cachable = true;
            }
            break;
        case CompressionType::kSnappyCompression:
        case CompressionType::kZstdCompression:
            return Status::NotSupported("block compression is not implemented");
        default:
            return Status::Corruption("bad block compression type");
    }

    *result = decoded;
    return Status::OK();
}
}
