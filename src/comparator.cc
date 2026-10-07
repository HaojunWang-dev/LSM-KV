#include "comparator.h"

#include <algorithm>
#include <cassert>

namespace LSMKV {
namespace {

class BytewiseComparatorImpl final : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    return a.Compare(b);
  }

  const char* Name() const override { return "LSMKV.BytewiseComparator"; }

  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    assert(start != nullptr);
    const std::size_t common_length = std::min(start->size(), limit.size());
    std::size_t different = 0;
    while (different < common_length &&
           (*start)[different] == limit.data()[different]) {
      ++different;
    }
    // 相同或前缀关系时没有可增加的分歧字节。
    if (different == common_length) {
      return;
    }
    const auto byte = static_cast<unsigned char>((*start)[different]);
    const auto upper = static_cast<unsigned char>(limit.data()[different]);
    if (byte < 0xff && byte + 1 < upper) {
      (*start)[different] = static_cast<char>(byte + 1);
      start->resize(different + 1);
    }
  }

  void FindShortSuccessor(std::string* key) const override {
    assert(key != nullptr);
    for (std::size_t i = 0; i < key->size(); ++i) {
      const auto byte = static_cast<unsigned char>((*key)[i]);
      if (byte != 0xff) {
        (*key)[i] = static_cast<char>(byte + 1);
        key->resize(i + 1);
        return;
      }
    }
    // 空 key 或全 0xff 的 key 保持原样，避免溢出回绕。
  }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl comparator;
  return &comparator;
}

}  // namespace LSMKV
