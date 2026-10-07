#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "iterator.h"

namespace LSMKV {
namespace {

void RecordEvent(void* events, void* value) {
  static_cast<std::vector<int>*>(events)->push_back(*static_cast<int*>(value));
}

void CountCleanup(void* count, void*) {
  ++*static_cast<int*>(count);
}

void CountWithNullFirstArgument(void* first, void* second) {
  EXPECT_EQ(first, nullptr);
  ++*static_cast<int*>(second);
}

class DestructionProbeIterator final : public Iterator {
 public:
  explicit DestructionProbeIterator(std::vector<int>* events) : events_(events) {}
  ~DestructionProbeIterator() override { events_->push_back(0); }

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}
  void Next() override {}
  void Prev() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return Status::OK(); }

 private:
  std::vector<int>* events_;  // 借用，测试日志比迭代器活得更久。
};

TEST(IteratorTest, EmptyIteratorRemainsInvalidAfterEverySeek) {
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  ASSERT_NE(iterator, nullptr);
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());

  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  iterator->SeekToLast();
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek(Slice());
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek(Slice("a\0b", 3));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(IteratorTest, ErrorIteratorOwnsItsBinaryStatusAndPreservesItAcrossSeeks) {
  std::string message("a\0b", 3);
  Status original = Status::Corruption(Slice(message));
  std::unique_ptr<Iterator> iterator(NewErrorIterator(original));
  original = Status::OK();
  message.assign(3, 'x');

  iterator->SeekToFirst();
  iterator->SeekToLast();
  iterator->Seek(Slice("target"));
  EXPECT_FALSE(iterator->Valid());
  Status returned = iterator->status();
  EXPECT_TRUE(returned.IsCorruption());
  iterator.reset();
  EXPECT_EQ(returned.ToString(), std::string("Corruption: ") + std::string("a\0b", 3));
}

TEST(IteratorTest, ErrorIteratorRetainsIoErrorCategory) {
  std::unique_ptr<Iterator> iterator(NewErrorIterator(Status::IOError("read failed")));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().IsIOError());
  EXPECT_EQ(iterator->status().ToString(), "IO error: read failed");
}

TEST(IteratorTest, ErrorFactoryAlsoPreservesOkStatus) {
  std::unique_ptr<Iterator> iterator(NewErrorIterator(Status::OK()));
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
}

TEST(IteratorTest, CleanupRunsOnceAtDestructionRatherThanRegistration) {
  int calls = 0;
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  iterator->RegisterCleanup(&CountCleanup, &calls, nullptr);
  iterator->SeekToFirst();
  EXPECT_EQ(calls, 0);
  iterator.reset();
  EXPECT_EQ(calls, 1);
}

TEST(IteratorTest, MultipleCleanupsRunInRegistrationOrder) {
  std::vector<int> events;
  int first = 10;
  int second = 20;
  int third = 30;
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  iterator->RegisterCleanup(&RecordEvent, &events, &first);
  iterator->RegisterCleanup(&RecordEvent, &events, &second);
  iterator->RegisterCleanup(&RecordEvent, &events, &third);
  EXPECT_TRUE(events.empty());
  iterator.reset();
  EXPECT_EQ(events, (std::vector<int>{10, 20, 30}));
}

TEST(IteratorTest, CleanupAcceptsNullOpaqueArguments) {
  int calls = 0;
  std::unique_ptr<Iterator> iterator(NewErrorIterator(Status::IOError("failed")));
  iterator->RegisterCleanup(&CountWithNullFirstArgument, nullptr, &calls);
  iterator.reset();
  EXPECT_EQ(calls, 1);
}

TEST(IteratorTest, CleanupRunsAfterDerivedDestructorThroughBasePointer) {
  std::vector<int> events;
  int cleanup_event = 1;
  std::unique_ptr<Iterator> iterator(new DestructionProbeIterator(&events));
  iterator->RegisterCleanup(&RecordEvent, &events, &cleanup_event);
  iterator.reset();
  EXPECT_EQ(events, (std::vector<int>{0, 1}));
}

TEST(IteratorTest, LongCleanupListIsFullyReleasedWithoutRecursiveDestruction) {
  constexpr int kCleanups = 100000;
  int calls = 0;
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  for (int i = 0; i < kCleanups; ++i) {
    iterator->RegisterCleanup(&CountCleanup, &calls, nullptr);
  }
  iterator.reset();
  EXPECT_EQ(calls, kCleanups);
}

#ifndef NDEBUG
TEST(IteratorDeathTest, EmptyIteratorRejectsOperationsRequiringValidEntry) {
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  EXPECT_DEATH(iterator->Next(), "Valid");
  EXPECT_DEATH(iterator->Prev(), "Valid");
  EXPECT_DEATH((void)iterator->key(), "Valid");
  EXPECT_DEATH((void)iterator->value(), "Valid");
}

TEST(IteratorDeathTest, CleanupRejectsNullFunction) {
  std::unique_ptr<Iterator> iterator(NewEmptyIterator());
  EXPECT_DEATH(iterator->RegisterCleanup(nullptr, nullptr, nullptr), "function");
}
#endif

}  // namespace
}  // namespace LSMKV
