#ifndef TASKENGINE_CORE_TASK_STATE_HPP
#define TASKENGINE_CORE_TASK_STATE_HPP

#include <cstdint>
#include <string_view>

namespace taskengine {

// Lifecycle states a task can be observed in.
//
// Queued and Running are transient and belong to the engine. Succeeded, Failed
// and Rejected are terminal: a task reaching one of them never changes state
// again, and exactly one of them is reported to whoever submitted it.
enum class TaskState : std::uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    Rejected,
};

// True for the three states from which no further transition is possible.
[[nodiscard]] constexpr bool is_terminal(TaskState state) noexcept {
    return state == TaskState::Succeeded
        || state == TaskState::Failed
        || state == TaskState::Rejected;
}

// Written without a default label on purpose: adding a state to the
// enumeration then fails this build under -Wswitch (from -Wall) plus -Werror,
// so the mapping cannot silently fall out of date. The trailing return is
// reachable only for a value outside the enumeration.
[[nodiscard]] constexpr std::string_view to_string(TaskState state) noexcept {
    switch (state) {
        case TaskState::Queued:    return "Queued";
        case TaskState::Running:   return "Running";
        case TaskState::Succeeded: return "Succeeded";
        case TaskState::Failed:    return "Failed";
        case TaskState::Rejected:  return "Rejected";
    }
    return "Invalid";
}

}  // namespace taskengine

#endif  // TASKENGINE_CORE_TASK_STATE_HPP
