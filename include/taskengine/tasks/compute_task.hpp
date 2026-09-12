#ifndef TASKENGINE_TASKS_COMPUTE_TASK_HPP
#define TASKENGINE_TASKS_COMPUTE_TASK_HPP

#include "taskengine/core/task.hpp"

#include <cstdint>

namespace taskengine {

// A CPU-bound task of adjustable size.
//
// Deterministic: the same iteration count and seed always produce the same
// checksum, on any machine. That is what makes it usable as a benchmark
// workload -- a run whose result varies is measuring the workload rather than
// the engine.
//
// The mixing step is a splitmix64 round. It is used because it is cheap, has no
// closed form the optimiser can fold the loop into, and carries every bit of
// each iteration into the next, so the work cannot be partially eliminated. The
// checksum accumulates into a member rather than a local, so the store is
// observable and the loop cannot be discarded as dead code.
//
// The same type covers two of the three benchmark profiles: a large iteration
// count is the CPU-heavy case where real work dominates, and a very small one
// is the lightweight case where the engine overhead does.
class ComputeTask final : public Task {
public:
    explicit ComputeTask(std::uint64_t iterations, std::uint64_t seed = 0) noexcept;

    void execute() override;

    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }
    [[nodiscard]] std::uint64_t iterations() const noexcept { return iterations_; }

private:
    std::uint64_t iterations_;
    std::uint64_t seed_;
    std::uint64_t checksum_{0};
};

}  // namespace taskengine

#endif  // TASKENGINE_TASKS_COMPUTE_TASK_HPP
