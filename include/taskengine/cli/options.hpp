#ifndef TASKENGINE_CLI_OPTIONS_HPP
#define TASKENGINE_CLI_OPTIONS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace taskengine {

// Process exit codes, as specified in ARCHITECTURE.md section 6.
//
// Three, and only three. A partially successful run is never reported as
// success, and a usage mistake is distinguishable from a run that executed and
// came back with failures -- which is the distinction a caller in a shell script
// actually needs.
inline constexpr int kExitSuccess = 0;       // every task succeeded
inline constexpr int kExitTaskFailure = 1;   // ran, but something failed or was rejected
inline constexpr int kExitUsage = 2;         // invalid usage or configuration

// A validated run configuration.
//
// Every field is already known to be usable by the time an Options exists:
// parse_args does the validation, so run() does not re-check and main() has no
// half-valid state to reason about.
struct Options {
    std::size_t workers{};
    std::size_t tasks{};
    std::uint64_t work{};
    std::size_t queue_capacity{};

    // Make every Nth task throw, so the failure path and its exit code can be
    // exercised from a shell. Zero means never. This is fault injection and is
    // documented as such; without it the documented exit code 1 would not be
    // reachable from the command line at all.
    std::uint64_t fail_every{};
};

// What the command line asked for.
//
// A plain struct with a tag rather than named factories: this is the outcome of
// parsing text, not a domain invariant that has to be impossible to misuse.
struct ParseResult {
    enum class Action {
        Run,
        ShowHelp,
        ShowVersion,
        Fail,
    };

    Action action{Action::Fail};
    Options options{};

    // Set only when action is Fail. Says what was wrong, not merely that
    // something was.
    std::string error;
};

// Parses arguments, excluding the program name.
//
// Takes already-split views rather than argc/argv so that it can be tested
// directly, without a process. Never throws and never writes to a stream:
// reporting is the callers job, which keeps this function a pure mapping from
// text to a decision.
[[nodiscard]] ParseResult parse_args(const std::vector<std::string_view>& args);

// Help text, including the exit-code contract. Callers print it verbatim.
[[nodiscard]] std::string_view usage() noexcept;

// Exit code for a run that completed.
//
// Separated from run() so the exit-code contract is unit-testable without
// starting an engine or a process.
[[nodiscard]] int exit_code_for(std::size_t failed, std::size_t rejected) noexcept;

// Worker count used when --workers is not given: the hardware concurrency, or
// one if the implementation cannot report it.
[[nodiscard]] std::size_t default_worker_count() noexcept;

}  // namespace taskengine

#endif  // TASKENGINE_CLI_OPTIONS_HPP
