#pragma once

#include "env.h"

namespace LSMKV {

// 数据库打开选项。
struct Options {
  // Borrowed; a caller-supplied Env must outlive the DB.
  Env* env = Env::Default();

  // 缺失时创建数据库目录。
  bool create_if_missing = true;

  // 已存在时返回错误。
  bool error_if_exists = false;

  // 恢复 WAL 时校验 CRC。
  bool paranoid_checks = true;
};

// 单次写入选项。
struct WriteOptions {
  // 返回前同步 WAL。
  bool sync = false;
};

// 单次读取选项。
struct ReadOptions {
  // 校验读取路径上的 checksum。
  bool verify_checksums = false;
};

}  // namespace LSMKV
