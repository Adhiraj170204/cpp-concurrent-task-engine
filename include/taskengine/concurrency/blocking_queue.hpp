#ifndef TASKENGINE_CONCURRENCY_BLOCKING_QUEUE_HPP
#define TASKENGINE_CONCURRENCY_BLOCKING_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace taskengine {

// A bounded, thread-safe producer/consumer queue.
//
// Bounded on purpose. An unbounded queue turns a producer/consumer rate
// mismatch into unbounded memory growth and hides it; a full queue blocks its
// producers instead, which is the honest behaviour for something that will
// later be fed by a message broker, and it is what makes a queue-wait
// measurement mean anything.
//
// Shared mutable state is the deque and the closed flag. Both are guarded by
// mutex_, which is the only mutex in the class, so a lock-order inversion is
// not possible here. Two condition variables share it: not_empty_ is waited on
// by consumers, not_full_ by producers. Every wait uses the predicate overload,
// so a spurious wakeup re-checks rather than proceeding, and there is no
// polling anywhere.
//
// Close semantics. close() is the only way to stop the queue. It releases every
// blocked thread of both kinds: a waiting producer returns false, and a waiting
// consumer drains whatever is still queued before pop() starts reporting
// nullopt. Closing therefore never discards items -- a consumer that keeps
// calling pop() until it gets nullopt has seen everything that was pushed.
//
// Not supported: calling push() from a thread that the same queue feeds. If the
// queue is full, that thread blocks in push() waiting for a consumer, and if
// every consumer is blocked the same way nothing can make progress. The
// engine documents this as unsupported rather than defending against it.
//
// Lifetime: destroying a queue while any thread is still inside push() or pop()
// is undefined behaviour, because destroying a condition variable with waiters
// is. Owners must close() and join every user before the queue goes out of
// scope. The thread pool that owns this queue is ordered to do exactly that.
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            // A zero-capacity queue could never accept a push, so every
            // producer would block forever. Rejecting it here turns a silent
            // deadlock into an immediate, diagnosable error.
            throw std::invalid_argument{"BlockingQueue capacity must be at least 1"};
        }
    }

    // Holds a mutex and condition variables, which are neither copyable nor
    // movable, and blocked threads hold references to this object.
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;
    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    ~BlockingQueue() = default;

    // Takes ownership of value, blocking while the queue is full.
    //
    // Returns false if the queue is closed, either on entry or while waiting
    // for space; in that case value is left untouched and nothing is queued.
    // Takes an rvalue reference rather than a value so that a caller cannot
    // hand over a copy by accident: T is moved in, never copied.
    bool push(T&& value) {
        {
            std::unique_lock<std::mutex> lock{mutex_};
            not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
            if (closed_) {
                return false;
            }
            items_.push_back(std::move(value));
        }
        // Notify after releasing the lock, so the woken consumer does not wake
        // only to block immediately on a mutex this thread still holds.
        not_empty_.notify_one();
        return true;
    }

    // Removes and returns the oldest item, blocking while the queue is empty.
    //
    // Returns nullopt only when the queue is both closed and drained, so a
    // consumer looping until nullopt is guaranteed to have seen every item that
    // was successfully pushed.
    std::optional<T> pop() {
        std::optional<T> item;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
            if (items_.empty()) {
                // The predicate passed and there is nothing here, so the queue
                // must be closed. Drained and finished.
                return std::nullopt;
            }
            // emplace rather than assignment: T need not be default
            // constructible, and this move-constructs straight into place.
            item.emplace(std::move(items_.front()));
            items_.pop_front();
        }
        not_full_.notify_one();
        return item;
    }

    // Stops the queue. Idempotent, callable from any thread, and safe to call
    // while other threads are blocked in push() or pop(): every one of them is
    // released. Already-queued items are still delivered to consumers.
    void close() {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (closed_) {
                return;
            }
            closed_ = true;
        }
        // notify_all, not notify_one: every blocked thread of both kinds has to
        // re-check the predicate, not just one of them.
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // Closes the queue and removes everything still in it, as one indivisible
    // step, returning the removed items to the caller.
    //
    // Closing and draining separately would leave a window in which a consumer
    // could dequeue an item after the close but before the drain, so that item
    // would run when the caller had asked for it to be discarded. Doing both
    // under one lock removes the window.
    //
    // The items are handed back rather than destroyed because the caller owns
    // whatever obligations they carry: an abandoned task still has a caller
    // waiting on its result, and dropping it silently would break that promise.
    std::vector<T> close_and_drain() {
        std::vector<T> drained;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            closed_ = true;
            drained.reserve(items_.size());
            for (auto& item : items_) {
                drained.push_back(std::move(item));
            }
            items_.clear();
        }
        // Producers waiting for space and consumers waiting for work both have
        // to re-check: the queue is now closed and empty.
        not_empty_.notify_all();
        not_full_.notify_all();
        return drained;
    }

    [[nodiscard]] bool is_closed() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return closed_;
    }

    // A momentary snapshot. With other threads running it is stale the instant
    // it is returned, so it is for diagnostics and invariant checks such as
    // "never above capacity", not for control flow.
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    bool closed_{false};
    const std::size_t capacity_;
};

}  // namespace taskengine

#endif  // TASKENGINE_CONCURRENCY_BLOCKING_QUEUE_HPP
