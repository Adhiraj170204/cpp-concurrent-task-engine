// Single-threaded behaviour of BlockingQueue: ordering, capacity accounting,
// close semantics and move-only support. The threaded properties -- blocking,
// wakeups, conservation -- live in tests/concurrency.

#include "taskengine/concurrency/blocking_queue.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using taskengine::BlockingQueue;

// Counts copies so a copy made anywhere in the queue is visible. Move-only
// types already fail to compile if copied, but this catches a copy of a type
// that merely *could* be copied.
struct CopyCounter {
    static inline int copies = 0;
    static inline int moves = 0;

    int value{0};

    explicit CopyCounter(int v) noexcept : value(v) {}
    CopyCounter(const CopyCounter& other) : value(other.value) { ++copies; }
    CopyCounter(CopyCounter&& other) noexcept : value(other.value) { ++moves; }
    CopyCounter& operator=(const CopyCounter& other) {
        value = other.value;
        ++copies;
        return *this;
    }
    CopyCounter& operator=(CopyCounter&& other) noexcept {
        value = other.value;
        ++moves;
        return *this;
    }
    ~CopyCounter() = default;

    static void reset() noexcept {
        copies = 0;
        moves = 0;
    }
};

// A queue owns a mutex and condition variables and blocked threads hold
// references to it, so it must be pinned in place.
static_assert(!std::is_copy_constructible_v<BlockingQueue<int>>);
static_assert(!std::is_move_constructible_v<BlockingQueue<int>>);
static_assert(!std::is_copy_assignable_v<BlockingQueue<int>>);
static_assert(!std::is_move_assignable_v<BlockingQueue<int>>);

TEST(BlockingQueue, RejectsZeroCapacity) {
    // A zero-capacity queue could never accept a push, so every producer would
    // block forever. That has to be an error, not a silent deadlock.
    EXPECT_THROW(BlockingQueue<int>{0}, std::invalid_argument);
}

TEST(BlockingQueue, StartsEmptyAndOpen) {
    const BlockingQueue<int> queue{4};
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(queue.capacity(), 4u);
    EXPECT_FALSE(queue.is_closed());
}

TEST(BlockingQueue, PopsInFifoOrder) {
    BlockingQueue<int> queue{8};
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.push(std::move(i)));
    }

    for (int expected = 0; expected < 5; ++expected) {
        const auto item = queue.pop();
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(*item, expected);
    }
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueue, SizeTracksPushesAndPops) {
    BlockingQueue<int> queue{3};
    EXPECT_EQ(queue.size(), 0u);

    queue.push(1);
    queue.push(2);
    EXPECT_EQ(queue.size(), 2u);

    (void)queue.pop();
    EXPECT_EQ(queue.size(), 1u);

    (void)queue.pop();
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueue, PushAfterCloseIsRejectedAndDoesNotBlock) {
    BlockingQueue<int> queue{4};
    queue.close();

    EXPECT_TRUE(queue.is_closed());
    // The queue is closed but not full, so this exercises the closed check
    // rather than the capacity wait: it must return rather than block.
    EXPECT_FALSE(queue.push(1));
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueue, ClosingDoesNotDiscardQueuedItems) {
    BlockingQueue<int> queue{8};
    queue.push(10);
    queue.push(20);
    queue.push(30);

    queue.close();

    // A consumer looping until nullopt must still see everything that was
    // successfully pushed. Closing stops intake, it does not drop work.
    EXPECT_EQ(queue.pop().value(), 10);
    EXPECT_EQ(queue.pop().value(), 20);
    EXPECT_EQ(queue.pop().value(), 30);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(BlockingQueue, PopOnAClosedEmptyQueueReturnsNulloptWithoutBlocking) {
    BlockingQueue<int> queue{4};
    queue.close();
    EXPECT_FALSE(queue.pop().has_value());
    // Repeatable: nullopt is a stable terminal answer, not a one-shot signal.
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(BlockingQueue, CloseIsIdempotent) {
    BlockingQueue<int> queue{4};
    queue.push(1);

    queue.close();
    queue.close();
    queue.close();

    EXPECT_TRUE(queue.is_closed());
    EXPECT_EQ(queue.pop().value(), 1);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(BlockingQueue, HoldsMoveOnlyTypes) {
    // unique_ptr cannot be copied, so this fails to compile if the queue ever
    // copies an element. That is the strongest possible check: a compile error
    // rather than a test assertion.
    BlockingQueue<std::unique_ptr<int>> queue{4};
    EXPECT_TRUE(queue.push(std::make_unique<int>(42)));

    const auto item = queue.pop();
    ASSERT_TRUE(item.has_value());
    ASSERT_NE(*item, nullptr);
    EXPECT_EQ(**item, 42);
}

TEST(BlockingQueue, MovesElementsAndNeverCopiesThem) {
    CopyCounter::reset();
    {
        BlockingQueue<CopyCounter> queue{4};
        queue.push(CopyCounter{1});
        queue.push(CopyCounter{2});

        const auto first = queue.pop();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->value, 1);
    }
    // Copies would show up here even for a type that permits them.
    EXPECT_EQ(CopyCounter::copies, 0);
    EXPECT_GT(CopyCounter::moves, 0);
}

TEST(BlockingQueue, HoldsTypesThatAreNotDefaultConstructible) {
    // pop() emplaces into an optional rather than default-constructing and
    // assigning, so T has no default-constructibility requirement. The engine
    // envelope this queue will eventually carry is exactly such a type.
    struct NoDefault {
        explicit NoDefault(std::string initial) : text(std::move(initial)) {}
        std::string text;
    };
    static_assert(!std::is_default_constructible_v<NoDefault>);

    BlockingQueue<NoDefault> queue{2};
    queue.push(NoDefault{"envelope"});

    const auto item = queue.pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->text, "envelope");
}

}  // namespace
