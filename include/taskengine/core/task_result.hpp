#ifndef TASKENGINE_CORE_TASK_RESULT_HPP
#define TASKENGINE_CORE_TASK_RESULT_HPP

#include "taskengine/core/task_id.hpp"
#include "taskengine/core/task_state.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace taskengine {

// Why a task failed. Empty for every result that is not Failed.
//
// Rule of zero: the converting constructor is the only user-declared one, so
// copy and move are compiler-generated and correct.
class TaskError {
public:
    TaskError() = default;

    // Takes by value and moves, so a caller passing an rvalue transfers its
    // buffer instead of copying it.
    explicit TaskError(std::string message) noexcept
        : message_(std::move(message)) {}

    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] bool empty() const noexcept { return message_.empty(); }

private:
    std::string message_;
};

// The three instants recorded for every task.
//
// steady_clock rather than system_clock: it is monotonic and cannot be stepped
// by NTP or by someone changing the wall clock, so a measured duration can
// never come out negative.
struct TaskTimings {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    TimePoint submitted{};
    TimePoint dequeued{};
    TimePoint finished{};

    [[nodiscard]] constexpr Duration queue_wait() const noexcept {
        return dequeued - submitted;
    }
    [[nodiscard]] constexpr Duration execution_time() const noexcept {
        return finished - dequeued;
    }
    [[nodiscard]] constexpr Duration total_latency() const noexcept {
        return finished - submitted;
    }
};

// The outcome of one submitted task.
//
// Constructed only through the three named factories, so a result cannot hold a
// combination the engine would never produce -- a Succeeded result carrying an
// error message, for instance. There is deliberately no default constructor: a
// TaskResult always describes a task that reached a terminal state.
class TaskResult {
public:
    [[nodiscard]] static TaskResult succeeded(TaskId id, TaskTimings timings);

    // The message is expected to be non-empty; the engine always supplies
    // either what() or a fixed string for an unknown exception type.
    [[nodiscard]] static TaskResult failed(TaskId id, TaskError error, TaskTimings timings);

    // A rejected task never reached a worker, so it has no dequeue instant. The
    // signature differs from the other two on purpose: the type should not let
    // a caller describe an execution window that never existed. dequeued is set
    // equal to finished, so execution_time() is zero rather than a meaningless
    // value derived from a default-constructed time point.
    [[nodiscard]] static TaskResult rejected(TaskId id,
                                             TaskTimings::TimePoint submitted,
                                             TaskTimings::TimePoint finished);

    [[nodiscard]] TaskId id() const noexcept { return id_; }
    [[nodiscard]] TaskState state() const noexcept { return state_; }
    [[nodiscard]] const TaskError& error() const noexcept { return error_; }
    [[nodiscard]] const TaskTimings& timings() const noexcept { return timings_; }

    [[nodiscard]] bool ok() const noexcept { return state_ == TaskState::Succeeded; }

private:
    TaskResult(TaskId id, TaskState state, TaskError error, TaskTimings timings) noexcept
        : id_(id), state_(state), error_(std::move(error)), timings_(timings) {}

    TaskId id_;
    TaskState state_;
    TaskError error_;
    TaskTimings timings_;
};

inline TaskResult TaskResult::succeeded(TaskId id, TaskTimings timings) {
    return TaskResult{id, TaskState::Succeeded, TaskError{}, timings};
}

inline TaskResult TaskResult::failed(TaskId id, TaskError error, TaskTimings timings) {
    return TaskResult{id, TaskState::Failed, std::move(error), timings};
}

inline TaskResult TaskResult::rejected(TaskId id,
                                       TaskTimings::TimePoint submitted,
                                       TaskTimings::TimePoint finished) {
    return TaskResult{id, TaskState::Rejected, TaskError{},
                      TaskTimings{submitted, finished, finished}};
}

}  // namespace taskengine

#endif  // TASKENGINE_CORE_TASK_RESULT_HPP
