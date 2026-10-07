#pragma once

#include <cstdint>
#include <string>

#include "status.h"

namespace LSMKV {

class Env;

// 根据数据库目录下的文件名识别用途；枚举值不属于磁盘编码格式。
enum class FileType {
  kLogFile,
  kDBLockFile,
  kTableFile,
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kInfoLogFile,
};

// 下列生成函数返回带 dbname 前缀的路径，不创建或打开文件。
// 带 number 参数的函数要求 number > 0；编号至少使用六位十进制数字，
// 更大的编号不会被截断。返回的字符串拥有路径字节。

// WAL，例如 dbname/000007.log。
std::string LogFileName(const std::string& dbname, std::uint64_t number);

// SSTable 使用 .ldb 后缀；.sst 名称用于兼容旧格式。
std::string TableFileName(const std::string& dbname, std::uint64_t number);
std::string SSTTableFileName(const std::string& dbname, std::uint64_t number);

// 版本元数据日志，例如 dbname/MANIFEST-000007。
std::string DescriptorFileName(const std::string& dbname, std::uint64_t number);

// CURRENT 保存当前 MANIFEST 的基本文件名及末尾换行。
std::string CurrentFileName(const std::string& dbname);

// 数据库目录锁文件：dbname/LOCK。
std::string LockFileName(const std::string& dbname);

// 数据库管理的临时文件，例如 dbname/000007.dbtmp。
std::string TempFileName(const std::string& dbname, std::uint64_t number);

// 诊断日志 LOG 和 LOG.old，不是保存写入记录的 WAL。
std::string InfoLogFileName(const std::string& dbname);
std::string OldInfoLogFileName(const std::string& dbname);

// 解析基本文件名（例如 "000007.log"），不接受包含目录的完整路径。
// number/type 必须非空。成功时填写类型和编号；无编号的文件返回编号 0。
// 未知名称、非法数字或溢出返回 false，且保持两个输出参数不变。
// .ldb 和 .sst 均识别为 kTableFile，LOG 和 LOG.old 均为 kInfoLogFile。
bool ParseFileName(const std::string& filename, std::uint64_t* number,
                   FileType* type);

// 使 CURRENT 指向指定 MANIFEST；descriptor_number 必须大于 0。
// 借用非空 env，其生命周期覆盖本次调用，调用方须串行化同一目录的更新。
// dbname 必须是非空且不含 NUL 的有效目录；调用方先同步 MANIFEST 的文件内容。
// 实现应通过同步临时文件并重命名来更新 CURRENT，传播所有 I/O 错误；
// 文件与目录项的持久化保证由 Env 实现提供，失败时不承诺磁盘状态未改变。
// 同步目录后再替换 CURRENT，并再次同步目录；失败清理只针对临时文件且尽力执行。
Status SetCurrentFile(Env* env, const std::string& dbname,
                      std::uint64_t descriptor_number);

}  // namespace LSMKV
