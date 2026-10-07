#pragma once

#include "env.h"

#include <cstddef>

namespace LSMKV {

// 数据库打开选项。
struct Options {
  // Borrowed; a caller-supplied Env must outlive the DB.
  Env* env = Env::Default();

  // 借用的诊断 Logger；调用方拥有它，须覆盖 DB 及所有日志调用的生命周期。
  // nullptr 表示不输出诊断日志；目前不自动创建日志文件。
  Logger* info_log = nullptr;

  // 缺失时创建数据库目录。
  bool create_if_missing = true;

  // 已存在时返回错误。
  bool error_if_exists = false;

  // 恢复 WAL 时校验 CRC。
  bool paranoid_checks = true;

  // 当前控制恢复后是否尝试复用 MANIFEST；不承诺已实现 WAL 复用。
  bool reuse_logs = false;

  // MANIFEST 达到此字节数就不复用；目前仅用于此判断。
  std::size_t max_file_size = 2 * 1024 * 1024;
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
