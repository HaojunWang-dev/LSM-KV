#pragma once

#include <algorithm>

#include "comparator.h"

namespace LSMKV {
namespace test {

class ReverseBytewiseComparator final : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    return b.Compare(a);
  }
  const char* Name() const override { return "test.ReverseBytewise"; }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

// 只折叠 ASCII 字母；NUL 和高位字节仍作为普通二进制字节比较。
class AsciiCaseInsensitiveComparator : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    const auto length = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < length; ++i) {
      const auto left = Fold(static_cast<unsigned char>(a.data()[i]));
      const auto right = Fold(static_cast<unsigned char>(b.data()[i]));
      if (left != right) {
        return left < right ? -1 : 1;
      }
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }
  const char* Name() const override { return "test.AsciiCaseInsensitive"; }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}

 private:
  static unsigned char Fold(unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
  }
};

// 先在折叠后的字节空间缩短，再用大写输出，确保调用方不能偷用字节顺序。
class AsciiCaseInsensitiveShorteningComparator final
    : public AsciiCaseInsensitiveComparator {
 public:
  const char* Name() const override { return "test.AsciiCaseInsensitiveShortening"; }
  void FindShortestSeparator(std::string* start, const Slice& limit) const override {
    std::string candidate = ChangeCase(Slice(*start), false);
    const std::string folded_limit = ChangeCase(limit, false);
    BytewiseComparator()->FindShortestSeparator(&candidate, Slice(folded_limit));
    *start = ChangeCase(Slice(candidate), true);
  }
  void FindShortSuccessor(std::string* key) const override {
    std::string candidate = ChangeCase(Slice(*key), false);
    BytewiseComparator()->FindShortSuccessor(&candidate);
    *key = ChangeCase(Slice(candidate), true);
  }

 private:
  static std::string ChangeCase(const Slice& key, bool uppercase) {
    std::string result = key.ToString();
    for (char& byte : result) {
      if (uppercase && byte >= 'a' && byte <= 'z') {
        byte -= 'a' - 'A';
      } else if (!uppercase && byte >= 'A' && byte <= 'Z') {
        byte += 'a' - 'A';
      }
    }
    return result;
  }
};

// 忽略尾部 padding，使不同长度的字节串可以表示同一个逻辑 key。
class TrailingPaddingComparator final : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    return Trim(a).Compare(Trim(b));
  }
  const char* Name() const override { return "test.TrailingPadding"; }
  void FindShortestSeparator(std::string* start, const Slice&) const override {
    *start = Trim(Slice(*start)).ToString();
  }
  void FindShortSuccessor(std::string* key) const override {
    *key = Trim(Slice(*key)).ToString();
  }

 private:
  static Slice Trim(const Slice& key) {
    std::size_t length = key.size();
    while (length > 0 && key.data()[length - 1] == '#') {
      --length;
    }
    return Slice(key.data(), length);
  }
};

}  // namespace test
}  // namespace LSMKV
