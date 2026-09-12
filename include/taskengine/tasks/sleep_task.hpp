#ifndef TASKENGINE_TASKS_SLEEP_TASK_HPP
#define TASKENGINE_TASKS_SLEEP_TASK_HPP

#include "taskengine/core/task.hpp"

#include <chrono>

namespace taskengine {

// A task that occupies a worker without consuming a core, standing in for
// blocking I/O.
//
// This is the reason the Task interface is polymorphic rather than a single
// parameterised type: it differs from ComputeTask in kind, not in size. A
// ComputeTask scales with the number of cores, and adding workers beyond that
// buys nothing. A SleepTask scales with the number of workers, far past the
// core count, because a sleeping worker is not competing for a core. The
// benchmark needs both to show why worker count is configurable at all.
//
// The sleep here is the workload, not a synchronisation device. Tests are
// forbidden from sleeping to coordinate; a task whose whole purpose is to model
// a blocking call is a different thing.
class SleepTask final : public Task {
public:
    explicit SleepTask(std::chrono::microseconds duration) noexcept;

    void execute() override;

    [[nodiscard]] std::chrono::microseconds duration() const noexcept { return duration_; }

private:
    std::chrono::microseconds duration_;
};

}  // namespace taskengine

#endif  // TASKENGINE_TASKS_SLEEP_TASK_HPP
