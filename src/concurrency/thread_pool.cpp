#include "taskengine/concurrency/thread_pool.hpp"

#include "taskengine/core/task_result.hpp"

#include <stdexcept>
#include <utility>

namespace taskengine {

ThreadPool::ThreadPool(std::size_t worker_count, std::size_t queue_capacity)
    : queue_(queue_capacity), samples_(worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument{"ThreadPool requires at least one worker"};
    }

    threads_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            threads_.emplace_back([this, i] { worker_loop(i); });
        }
    } catch (...) {
        // Thread creation failed part way through. A destructor does not run
        // for an object whose constructor threw, so the workers that did start
        // have to be released here or they would outlive the pool and take its
        // queue and sample buffers with them.
        queue_.close();
        join_workers();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    // Drain shutdown, matching the documented destructor semantics. If this
    // were to fail, terminating is correct rather than unfortunate: the
    // alternative is worker threads still running against a queue that is about
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
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void ThreadPool::shutdown() {
    // Close first, join second. Always this order: joining while the queue is
    // still open would wait on workers that are still blocked for work that is
    // never coming.
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
    rejected_.fetch_add(discarded.size(), std::memory_order_relaxed);

    join_workers();
}

std::vector<Sample> ThreadPool::collect_samples() const {
    if (!workers_joined()) {
        // Reading the buffers while workers are still appending to them would
        // be a data race. Returning what happens to be there would look like an
        // answer, which is worse than refusing to give one.
        throw std::logic_error{
            "ThreadPool::collect_samples requires the workers to be joined; "
            "call shutdown() or shutdown_now() first"};
    }

    std::size_t total = 0;
    for (const std::vector<Sample>& buffer : samples_) {
        total += buffer.size();
    }

    std::vector<Sample> merged;
    merged.reserve(total);
    for (const std::vector<Sample>& buffer : samples_) {
        merged.insert(merged.end(), buffer.begin(), buffer.end());
    }
    return merged;
}

std::uint64_t ThreadPool::rejected_count() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
}

void ThreadPool::worker_loop(std::size_t worker_index) {
    // pop() returns nullopt only once the queue is both closed and empty, so
    // this loop drains everything that was accepted before exiting.
    std::vector<Sample>& samples = samples_[worker_index];
    while (std::optional<TaskEnvelope> envelope = queue_.pop()) {
        // Appending to this worker's own buffer. No other thread touches it
        // until every worker has been joined, so no lock is needed and none is
        // taken on the path that measures the thing being measured.
        samples.push_back(envelope->run(TaskTimings::Clock::now()));
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
    // joined_ as true while a thread is still being joined. This is also what
    // makes collect_samples safe: joined_ being true happens-after every
    // worker finished writing.
    joined_ = true;
}

bool ThreadPool::workers_joined() const {
    const std::lock_guard<std::mutex> lock{shutdown_mutex_};
    return joined_;
}

}  // namespace taskengine
