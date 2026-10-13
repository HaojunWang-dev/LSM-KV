#pragma once

#include "comparator.h"
#include "env.h"

#include <cstddef>
#include <cstdint>

namespace LSMKV {

// SSTable block trailer 中的压缩类型字节；编码值属于磁盘格式，不能重新编号。
// 声明这些类型不代表已经提供对应的压缩或解压实现。
enum class CompressionType : std::uint8_t {
  kNoCompression = 0,
  kSnappyCompression = 1,
  kZstdCompression = 2,
};

// 数据库打开选项。
struct Options {
  // Borrowed; a caller-supplied Env must outlive the DB.
  Env* env = Env::Default();

  // key 的排序规则；默认实例由库持有，调用方不得 delete。
  // 自定义 comparator 为借用对象，须覆盖所有使用它的 DB/Table/BlockBuilder。
  // 重开数据库时，Name() 和排序规则必须与创建时一致；使用期间不可改变规则。
  const Comparator* comparator = BytewiseComparator();

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

  // 未压缩 data block 的目标字节数，不是硬上限；单条大 entry 可以超过它。
  // 要求大于零；供后续 TableBuilder 决定何时结束一个 block，当前尚未接入。
  std::size_t block_size = 4 * 1024;

  // 前缀压缩中每个 restart 分组最多包含的 entry 数，要求大于零。
  // 每组首条 entry 保存完整 key；设为 1 时每条 entry 都是 restart。
  // 供 BlockBuilder 使用；当前仅声明接口，尚未实现构建逻辑。
  int block_restart_interval = 16;

  // 写入 block 时选用的压缩方式；不改变已有文件的读取格式。
  // 当前只支持不压缩，Snappy/Zstd 的枚举声明不代表已实现编解码。
  // 供后续 TableBuilder 使用，当前尚未接入写入路径。
  CompressionType compression = CompressionType::kNoCompression;
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
