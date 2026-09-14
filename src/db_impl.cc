#include "db.h"
#include "db_impl.h"
#include "env.h"
#include "log_format.h"
#include "log_reader.h"
#include "log_writer.h"
#include "memtable.h"
#include "write_batch.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <memory>
#include <set>
#include <string>
#include <mutex>
#include <vector>

namespace LSMKV {

struct DBImpl::Writer {
    Status status;
    WriteBatch* batch = nullptr;
    bool sync = false;
    bool done = false;
    std::condition_variable cv;
};

Status DBImpl::NewDB() {
}
}
