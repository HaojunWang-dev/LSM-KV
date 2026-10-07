#pragma once

#include <string>

#include "slice.h"

namespace LSMKV {

// 定义二进制 user key 的稳定全序；Compare == 0 表示同一个逻辑 key。
// 实现必须允许并发调用，且使用期间不得改变比较规则。
// 调用方拥有自定义比较器，必须保证它比所有借用它的对象活得更久。
class Comparator {
 public:
  virtual ~Comparator() = default;

  // 分别返回负数、零、正数，表示 a 在 b 之前、等价、之后。
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // 持久化的规则标识。排序或等价规则改变时必须更换名称；
  // 返回的字符串至少在本对象的生命周期内有效。
  virtual const char* Name() const = 0;

  // 当原 start < limit 时，可缩短为 [原 start, limit) 中的 key。
  // 不修改也是合法实现；仅用于索引边界，不用于改写用户数据。
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // 可缩短为不小于原 key 的 key；不修改也是合法实现。
  // 可变参数的并发访问由调用方同步。
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// 默认按无符号字节的字典序排序；返回库持有的实例，不得 delete。
const Comparator* BytewiseComparator();

}  // namespace LSMKV
