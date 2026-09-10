#pragma once

#include <string>
#include <utility>

#include "slice.h"

namespace LSMKV {
/*

state_

       4 bytes       1 byte        N bytes
┌────────────────┬───────────┬────────────────────┐
│ message length │ error code│ error message      │
└────────────────┴───────────┴────────────────────┘
0                4           5

*/

// Status 表示一次操作的结果：成功状态零分配，错误状态携带诊断消息。
class Status {
public:
    // 默认构造成功状态。nullptr 是 OK 的唯一表示。
    Status() noexcept
        : state_(nullptr) {}

    ~Status() {
        delete[] state_;
    }

    // 复制错误状态的紧凑状态块；OK 状态仍保持 nullptr。
    Status(const Status& rhs);
    Status& operator=(const Status& rhs);

    // 移动只转移状态块所有权，避免复制错误消息。
    Status(Status&& rhs) noexcept
        : state_(rhs.state_) {
        rhs.state_ = nullptr;
    }
    Status& operator=(Status&& rhs) noexcept;

    // 构造成功状态或指定类别的错误状态。msg2 非空时与 msg 组合为诊断消息。
    static Status OK() {
        return Status();
    }
    static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
        return Status(kNotFound, msg, msg2);
    }
    static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
        return Status(kCorruption, msg, msg2);
    }
    static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
        return Status(kNotSupported, msg, msg2);
    }
    static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
        return Status(kInvalidArgument, msg, msg2);
    }
    static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
        return Status(kIOError, msg, msg2);
    }

    // 返回当前状态是否成功。
    bool ok() const {
        return state_ == nullptr;
    }

    // 各错误类别的快捷判断。
    bool IsNotFound() const {
        return code() == kNotFound;
    }
    bool IsCorruption() const {
        return code() == kCorruption;
    }
    bool IsIOError() const {
        return code() == kIOError;
    }
    bool IsNotSupportedError() const {
        return code() == kNotSupported;
    }
    bool IsInvalidArgument() const {
        return code() == kInvalidArgument;
    }

    // 返回适合日志输出的文本，例如 "OK" 或 "NotFound: missing key"。
    std::string ToString() const;

private:
    // 错误码存放于 state_[4]，与 LevelDB 的磁盘无关，仅为内存状态布局。
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5,
    };

    Code code() const {
        return state_ == nullptr ? kOk : static_cast<Code>(state_[4]);
    }

    // 在 status.cc 中分配 [length: fixed32][code: byte][message] 状态块。
    Status(Code code, const Slice& msg, const Slice& msg2);
    static const char* CopyState(const char* state);

    // OK: nullptr。
    // Error: state_[0..3] 为消息长度，state_[4] 为 Code，state_[5..] 为消息字节。
    const char* state_;
};

inline Status::Status(const Status& rhs)
    : state_(rhs.state_ == nullptr ? nullptr : CopyState(rhs.state_)) {}

inline Status& Status::operator=(const Status& rhs) {
    // 同一状态块表示自赋值，或两个对象都处于 OK 状态。
    if (state_ != rhs.state_) {
        delete[] state_;
        state_ = rhs.state_ == nullptr ? nullptr : CopyState(rhs.state_);
    }
    return *this;
}

inline Status& Status::operator=(Status&& rhs) noexcept {
    std::swap(state_, rhs.state_);
    return *this;
}

}  // namespace LSMKV
