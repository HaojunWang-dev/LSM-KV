#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logging.h"
#include "options.h"

namespace LSMKV {
namespace {

// 实际格式化并保存消息，不借用 Log() 的格式字符串或 va_list。
class StringLogger final : public Logger {
 public:
  explicit StringLogger(bool* destroyed = nullptr) : destroyed_(destroyed) {}
  ~StringLogger() override {
    if (destroyed_ != nullptr) *destroyed_ = true;
  }

  void Logv(const char* format, std::va_list args) override {
    std::va_list size_args;
    va_copy(size_args, args);
    const int length = std::vsnprintf(nullptr, 0, format, size_args);
    va_end(size_args);
    ASSERT_GE(length, 0);
    std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
    ASSERT_EQ(std::vsnprintf(buffer.data(), buffer.size(), format, args), length);
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.emplace_back(buffer.data(), static_cast<std::size_t>(length));
  }

  std::vector<std::string> Messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
  }

 private:
  bool* destroyed_;  // 借用测试标记，生命周期覆盖 Logger。
  mutable std::mutex mutex_;
  std::vector<std::string> messages_;
};

TEST(LoggingTest, DefaultOptionsAndNullLoggerDoNotProduceOutput) {
  Options options;
  EXPECT_EQ(options.info_log, nullptr);
  Log(options.info_log, "ignored: %s %d", "message", 7);
  Log(nullptr, nullptr);  // 无 Logger 时，不读取格式字符串。
}

TEST(LoggingTest, ForwardsFormattedArgumentsThroughConfiguredLogger) {
  StringLogger logger;
  Options options;
  options.info_log = &logger;
  Log(options.info_log, "%s:%d:%llu:%c:%%", "MANIFEST", -7,
      std::numeric_limits<unsigned long long>::max(), 'X');
  EXPECT_EQ(logger.Messages(),
            (std::vector<std::string>{"MANIFEST:-7:18446744073709551615:X:%"}));
}

TEST(LoggingTest, ForwardsEmptyAndLongMessagesWithoutTruncationOrDanglingBytes) {
  StringLogger logger;
  std::string message(4096, 'a');
  Log(&logger, "");
  Log(&logger, "prefix:%s", message.c_str());
  message.assign(4096, 'x');
  EXPECT_EQ(logger.Messages(),
            (std::vector<std::string>{"", "prefix:" + std::string(4096, 'a')}));
}

TEST(LoggingTest, OptionsBorrowLoggerAndBaseDeletionDestroysDerivedObject) {
  bool destroyed = false;
  auto logger = std::unique_ptr<Logger>(new StringLogger(&destroyed));
  {
    Options options;
    options.info_log = logger.get();
    Log(options.info_log, "borrowed logger");
  }
  EXPECT_FALSE(destroyed);
  logger.reset();
  EXPECT_TRUE(destroyed);
}

TEST(LoggingTest, SharedThreadSafeLoggerReceivesCompleteIndependentMessages) {
  StringLogger logger;
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&logger, worker] {
      for (int item = 0; item < 16; ++item) Log(&logger, "%d:%d", worker, item);
    });
  }
  for (auto& thread : threads) thread.join();
  std::vector<std::string> expected;
  for (int worker = 0; worker < 4; ++worker) {
    for (int item = 0; item < 16; ++item) {
      expected.push_back(std::to_string(worker) + ":" + std::to_string(item));
    }
  }
  auto actual = logger.Messages();
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
}

}  // namespace
}  // namespace LSMKV
