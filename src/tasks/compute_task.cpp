#include "taskengine/tasks/compute_task.hpp"

namespace taskengine {

ComputeTask::ComputeTask(std::uint64_t iterations, std::uint64_t seed) noexcept
    : iterations_(iterations), seed_(seed) {}

void ComputeTask::execute() {
    // splitmix64. Unsigned arithmetic throughout, so the wrapping is defined
    // rather than undefined, which matters under UBSan and matters more for the
    // determinism this task promises.
    std::uint64_t state = seed_;
    std::uint64_t checksum = checksum_;

    for (std::uint64_t i = 0; i < iterations_; ++i) {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        checksum ^= z ^ (z >> 31);
    }

    // Written back to a member, so the result outlives this call and the loop
    // is not dead code the compiler may delete.
    checksum_ = checksum;
}

}  // namespace taskengine
