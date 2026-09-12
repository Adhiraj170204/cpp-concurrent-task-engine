#include "taskengine/tasks/sleep_task.hpp"

#include <thread>

namespace taskengine {

SleepTask::SleepTask(std::chrono::microseconds duration) noexcept : duration_(duration) {}

void SleepTask::execute() {
    // sleep_for guarantees at least the requested duration, never less. It may
    // overshoot, which is exactly how a real blocking call behaves and is why
    // any assertion about this task is a lower bound rather than an equality.
    std::this_thread::sleep_for(duration_);
}

}  // namespace taskengine
