#include "taskengine/concurrency/thread_pool.hpp"

#include "taskengine/core/task_result.hpp"

#include <stdexcept>
#include <utility>

namespace taskengine {

ThreadPool::ThreadPool(std::size_t worker_count, std::size_t queue_capacity)
    : queue_(queue_capacity) {
    if (worker_count == 0) {
        throw std::invalid_argument{"ThreadPool requires at least one worker"};
    }

    threads_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // Thread creation failed part way through. A destructor does not run
        // for an object whose constructor threw, so the workers that did start
        // have to be released here or they would outlive the pool and take its
        // queue with them.
        queue_.close();
        join_workers();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    // Drain shutdown, matching the documented destructor semantics. If this
    // were to fail, terminating is correct rather than unfortunate: the
    // alternative is worker threads left running against a queue that is about
    // to be destroyed.
    shutdown();
}

bool ThreadPool::submit(TaskEnvelope envelope) {
    if (queue_.push(std::move(envelope))) {
        return true;
    }

    // push leaves its argument untouched when it refuses, so this envelope and
    // the promise inside it are still ours. Rejecting it here is what keeps the
    // exactly-once guarantee: the caller gets a Rejected result rather than a
    // future broken by the envelope being destroyed unfulfilled.
    envelope.reject(TaskTimings::Clock::now());
    return false;
}

void ThreadPool::shutdown() {
    // Close first, join second. Always this order: joining while the queue is
    // still open would wait on workers that are still blocked waiting for work
    // that is never coming.
    queue_.close();
    join_workers();
}

void ThreadPool::shutdown_now() {
    // Closing and draining as one step, so nothing can be dequeued in between
    // and run after the caller asked for it to be abandoned.
    std::vector<TaskEnvelope> discarded = queue_.close_and_drain();

    const TaskTimings::TimePoint finished = TaskTimings::Clock::now();
    for (TaskEnvelope& envelope : discarded) {
        // Abandoned work still has a caller waiting on it.
        envelope.reject(finished);
    }

    join_workers();
}

void ThreadPool::worker_loop() {
    // pop() returns nullopt only once the queue is both closed and empty, so
    // this loop drains everything that was accepted before exiting.
    while (std::optional<TaskEnvelope> envelope = queue_.pop()) {
        envelope->run(TaskTimings::Clock::now());
    }
}

void ThreadPool::join_workers() {
    const std::lock_guard<std::mutex> lock{shutdown_mutex_};
    if (joined_) {
        return;
    }
    for (std::thread& worker : threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // Set only after every join has returned, so a second caller cannot see
    // joined_ as true while a thread is still being joined.
    joined_ = true;
}

}  // namespace taskengine
