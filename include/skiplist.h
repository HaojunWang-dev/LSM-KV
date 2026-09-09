#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "arena.h"

namespace LSMKV {

template <typename Key, class Comparator>
class SkipList {
private:
    struct Node;

public:
    explicit SkipList(
        Comparator comparator,
        Arena* arena);

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    void Insert(const Key& key);

    bool Contains(const Key& key) const;

    class Iterator {
    public:
        explicit Iterator(const SkipList* list)
            : list_(list),
              node_(nullptr) {}

        bool Valid() const {
            return node_ != nullptr;
        }

        const Key& key() const {
            assert(Valid());

            return node_->key;
        }

        void Next() {
            assert(Valid());

            node_ = node_->Next(0);
        }

        void Seek(const Key& target) {
            node_ =
                list_->FindGreaterOrEqual(
                    target,
                    nullptr);
        }

        void SeekToFirst() {
            node_ = list_->head_->Next(0);
        }

    private:
        const SkipList* list_;
        Node* node_;
    };

private:
    static constexpr int kMaxHeight = 12;

    // RandomHeight 中每次有 1 / kBranching
    // 的概率继续升高一层。
    static constexpr unsigned kBranching = 4;

    struct Node {
        explicit Node(const Key& k)
            : key(k),
              next_(nullptr),
              height_(0) {}

        const Key key;

        Node* Next(int level) const {
            assert(level >= 0);
            assert(level < height_);

            return next_[level].load(
                std::memory_order_acquire);
        }

        void SetNext(
            int level,
            Node* node) {

            assert(level >= 0);
            assert(level < height_);

            next_[level].store(
                node,
                std::memory_order_release);
        }

        Node* NoBarrierNext(int level) const {
            assert(level >= 0);
            assert(level < height_);

            return next_[level].load(
                std::memory_order_relaxed);
        }

        void NoBarrierSetNext(
            int level,
            Node* node) {

            assert(level >= 0);
            assert(level < height_);

            next_[level].store(
                node,
                std::memory_order_relaxed);
        }

        std::atomic<Node*>* next_;
        int height_;
    };

private:
    Node* NewNode(
        const Key& key,
        int height);

    Node* FindGreaterOrEqual(
        const Key& key,
        Node** prev) const;

    bool Equal(
        const Key& a,
        const Key& b) const {

        return compare_(a, b) == 0;
    }

    bool KeyIsAfterNode(
        const Key& key,
        Node* node) const {

        return node != nullptr &&
               compare_(node->key, key) < 0;
    }

    int RandomHeight();

    uint32_t NextRandom();

    static size_t AlignUp(
        size_t value,
        size_t alignment) {

        return
            (value + alignment - 1) &
            ~(alignment - 1);
    }

private:
    Comparator compare_;

    Arena* const arena_;

    Node* const head_;

    std::atomic<int> max_height_;

    uint32_t random_seed_;
};



template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(
    Comparator comparator,
    Arena* arena)
    : compare_(comparator),
      arena_(arena),
      head_(NewNode(Key{}, kMaxHeight)),
      max_height_(1),
      random_seed_(0xdeadbeef) {

    assert(arena_ != nullptr);

    for (int i = 0; i < kMaxHeight; ++i) {
        head_->NoBarrierSetNext(i, nullptr);
    }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::NewNode(const Key& key, int height)
{
    assert(height > 0);
    assert(height <= kMaxHeight);

    static_assert(std::is_trivially_destructible_v<Key>, "Arena-backed SkipList currently requires"
        "a trivially destructible Key");

    constexpr size_t next_alignent = alignof(std::atomic<Node*>);

    const size_t next_offset = AlignUp(sizeof(Node), next_alignent);

    const size_t total_size = next_offset + sizeof(std::atomic<Node*>) * static_cast<size_t> (height);

    char* memory = arena_->AllocateAligned(total_size);

    Node* node = new (memory) Node(key);

    node->height_ = height;

    char* next_memory = memory + next_offset;

    node->next_ = reinterpret_cast<std::atomic<Node*>*>(next_memory);

    for (int i = 0;  i < height; i++)
    {
        new (&node->next_[i]) std::atomic<Node*> (nullptr);
    }

    return node;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Node** prev) const
{
    Node* x = head_;

    int level = max_height_.load(std::memory_order_relaxed) - 1;

    while(true)
    {
        Node* next = x->Next(level);

        if(KeyIsAfterNode(key, next))
        {
            x = next;
        }
        else 
        {
            if (prev != nullptr)
            {
                prev[level] = x;
            }

            if (level == 0)
            {
                return next;
            }
            
            --level;
        }
    }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const
{
    Node* x = FindGreaterOrEqual(key, nullptr);

    return x != nullptr && Equal(key, x->key);
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key)
{
    Node* prev[kMaxHeight];

    Node* x = FindGreaterOrEqual(key, prev);

    assert(x == nullptr || !Equal(key, x->key));

    const int height = RandomHeight();

    int current_max_height = max_height_.load(std::memory_order_relaxed);

    if (height > current_max_height)
    {
        for (int i = current_max_height; i < height; i++)
        {
            prev[i] = head_;
        }

        max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);

    for (int i = 0; i < height; i++)
    {
        x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));

        prev[i]->NoBarrierSetNext(i, x);
    }
}

template <typename Key, class Comparator>
uint32_t SkipList<Key, Comparator>::NextRandom() {
    uint32_t x = random_seed_;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    random_seed_ = x;

    return x;
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight()
{
    int height = 1;

    while(height < kMaxHeight && NextRandom() % kBranching == 0)
    {
        height++;
    }

    return height;
}

}