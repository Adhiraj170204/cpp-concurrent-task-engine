// Threaded behaviour of BlockingQueue: blocking, wakeups, capacity under load,
// and conservation of items across concurrent producers and consumers.
//
// No test here synchronises with a sleep. Ordering is established with joins,
// atomics and the queue itself. A test that passes because a sleep happened to
// be long enough is a test that fails on a loaded CI machine.
//
// What these tests prove on their own is limited: passing once means one
// interleaving worked. ThreadSanitizer is the actual evidence, because it
// reasons about the happens-before graph rather than sampling schedules. These
// tests exist to *create* interleavings; TSan judges them.
//
// Several tests would hang rather than fail if the queue stopped waking blocked
// threads. That is deliberate: CTest reports the timeout, and a hang localised
// to one named test is a clearer signal than a silently wrong count.

#include "taskengine/concurrency/blocking_queue.hpp"

#include "support/threads.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <thread>
#include <vector>

namespace {

using taskengine::BlockingQueue;
using taskengine::test::Gate;

// std::atomic is not copyable, so these vectors are sized once and then filled
// in place. Zeroing explicitly rather than relying on value-initialisation of
// std::atomic, whose default constructor does not initialise before C++20.
std::vector<std::atomic<int>> zeroed_counters(std::size_t count) {
    std::vector<std::atomic<int>> counters(count);
    for (auto& counter : counters) {
        counter.store(0, std::memory_order_relaxed);
    }
    return counters;
}

TEST(BlockingQueueConcurrency, ConsumerBlocksOnAnEmptyQueueUntilAnItemArrives) {
    BlockingQueue<int> queue{4};
    std::optional<int> received;

    std::thread consumer{[&] { received = queue.pop(); }};

    // pop() on an open, empty queue has to wait. If it returned early it could
    // only return nullopt, which would be wrong -- the queue is not closed.
    queue.push(99);
    consumer.join();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 99);
}

TEST(BlockingQueueConcurrency, ProducerBlocksOnAFullQueueUntilAnItemIsPopped) {
    BlockingQueue<int> queue{2};
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    ASSERT_EQ(queue.size(), 2u);

    std::atomic<bool> accepted{false};
    std::thread producer{[&] {
        accepted.store(queue.push(3), std::memory_order_release);
    }};

    // Nothing can let the producer in except space appearing.
    EXPECT_EQ(queue.pop().value(), 1);
    producer.join();

    EXPECT_TRUE(accepted.load(std::memory_order_acquire));
    EXPECT_EQ(queue.pop().value(), 2);
    EXPECT_EQ(queue.pop().value(), 3);
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueueConcurrency, CloseReleasesEveryBlockedConsumer) {
    // PLAN acceptance criterion: blocked consumers are released during
    // shutdown. If close() notified only one waiter, this hangs.
    constexpr int kConsumers = 8;
    BlockingQueue<int> queue{4};
    std::atomic<int> released{0};

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            if (!queue.pop().has_value()) {
                released.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    queue.close();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    // The queue is empty and never receives anything, so every consumer must
    // come back with nullopt whether it had already blocked or not.
    EXPECT_EQ(released.load(), kConsumers);
}

TEST(BlockingQueueConcurrency, CloseReleasesEveryBlockedProducer) {
    constexpr int kProducers = 8;
    BlockingQueue<int> queue{2};
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));  // full, and nothing will ever consume

    std::atomic<int> rejected{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i] {
            int value = 100 + i;
            if (!queue.push(std::move(value))) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    queue.close();
    for (auto& producer : producers) {
        producer.join();
    }

    // No consumer ever runs, so no space can appear: every producer must be
    // turned away rather than left waiting.
    EXPECT_EQ(rejected.load(), kProducers);
    EXPECT_EQ(queue.size(), 2u);
}

TEST(BlockingQueueConcurrency, CapacityIsNeverExceededUnderLoad) {
    constexpr std::size_t kCapacity = 4;
    constexpr int kProducers = 6;
    constexpr int kPerProducer = 500;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> queue{kCapacity};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                int value = i;
                queue.push(std::move(value));
            }
        });
    }

    std::size_t observed_max = 0;
    for (int i = 0; i < kTotal; ++i) {
        observed_max = std::max(observed_max, queue.size());
        (void)queue.pop();
    }
    for (auto& producer : producers) {
        producer.join();
    }

    // The bound is the whole point of a bounded queue. A push that ignored
    // capacity would show up here.
    EXPECT_LE(observed_max, kCapacity);
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueueConcurrency, EveryItemIsDeliveredExactlyOnce) {
    // Conservation, checked per item rather than by a total: a total would be
    // satisfied by one item lost and another delivered twice.
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> queue{16};
    std::vector<std::atomic<int>> seen = zeroed_counters(kTotal);
    std::atomic<int> push_failures{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                int value = p * kPerProducer + i;  // unique across all producers
                if (!queue.push(std::move(value))) {
                    push_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (const std::optional<int> item = queue.pop()) {
                seen[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    queue.close();  // only after every producer is done, so nothing is refused
    for (auto& consumer : consumers) {
        consumer.join();
    }

    EXPECT_EQ(push_failures.load(), 0);

    int missing = 0;
    int duplicated = 0;
    for (int i = 0; i < kTotal; ++i) {
        const int count = seen[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        if (count == 0) {
            ++missing;
        } else if (count > 1) {
            ++duplicated;
        }
    }
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(duplicated, 0);
}

TEST(BlockingQueueConcurrency, ClosingMidStreamDeliversExactlyWhatWasAccepted) {
    // The property that makes close() safe to call at any moment: an item whose
    // push returned true is always delivered, and an item whose push returned
    // false never appears. Nothing is lost and nothing is invented.
    //
    // Deterministic by construction. The consumer threads wait for the close,
    // so before it the main thread is the only consumer, and that bounds
    // exactly how far the producers can get: at most kDrainedByMainBeforeClosing
    // items popped by main plus kCapacity items still queued are accepted, and
    // each producer takes at most one further ticket, for the push the close
    // refuses. The close still lands on live producers, most of them blocked in
    // push against a full queue, and the consumers then drain what remains.
    //
    // An earlier version let consumers run from the start. How much traffic
    // passed before the close then depended on scheduling, so any bound on the
    // producers was a race. Fixed at M7.
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr std::size_t kCapacity = 8;
    constexpr int kTicketSpace = 200000;
    constexpr int kDrainedByMainBeforeClosing = 500;
    constexpr int kMaxAccepted = kDrainedByMainBeforeClosing + static_cast<int>(kCapacity);

    BlockingQueue<int> queue{kCapacity};
    std::vector<std::atomic<int>> accepted = zeroed_counters(kTicketSpace);
    std::vector<std::atomic<int>> seen = zeroed_counters(kTicketSpace);
    std::atomic<int> next_ticket{0};
    Gate closed;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (;;) {
                const int value = next_ticket.fetch_add(1, std::memory_order_relaxed);
                if (value >= kTicketSpace) {
                    return;  // array bound; unreachable given the arithmetic above
                }
                int payload = value;
                if (!queue.push(std::move(payload))) {
                    return;  // the queue closed: this is how producers finish
                }
                accepted[static_cast<std::size_t>(value)].store(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            closed.wait();
            while (const std::optional<int> item = queue.pop()) {
                seen[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < kDrainedByMainBeforeClosing; ++i) {
        const std::optional<int> item = queue.pop();
        ASSERT_TRUE(item.has_value());
        seen[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
    }

    queue.close();
    closed.open();

    for (auto& producer : producers) {
        producer.join();
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }

    int mismatches = 0;
    int accepted_count = 0;
    for (int i = 0; i < kTicketSpace; ++i) {
        const int was_accepted = accepted[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        const int times_seen = seen[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        accepted_count += was_accepted;
        // Delivered exactly once if accepted, never seen if refused.
        if (times_seen != was_accepted) {
            ++mismatches;
        }
    }

    EXPECT_EQ(mismatches, 0);
    EXPECT_GE(accepted_count, kDrainedByMainBeforeClosing);
    EXPECT_LE(accepted_count, kMaxAccepted);
    EXPECT_LE(next_ticket.load(), kMaxAccepted + kProducers);
    EXPECT_TRUE(queue.is_closed());
}

}  // namespace
