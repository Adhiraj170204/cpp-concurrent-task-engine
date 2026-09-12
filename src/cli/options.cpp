#include "taskengine/cli/options.hpp"

#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <thread>

namespace taskengine {
namespace {

constexpr std::size_t kDefaultTasks = 1000;
constexpr std::uint64_t kDefaultWork = 1000;
constexpr std::size_t kDefaultQueueCapacity = 1024;

constexpr std::string_view kUsage =
    "task-engine - concurrent task processing engine\n"
    "\n"
    "Usage:\n"
    "  task-engine [options]\n"
    "\n"
    "Options:\n"
    "  --workers N          worker threads (default: hardware concurrency)\n"
    "  --tasks N            tasks to submit (default: 1000)\n"
    "  --work N             compute iterations per task (default: 1000)\n"
    "                       0 submits empty tasks, measuring engine overhead\n"
    "  --queue-capacity N   bounded queue capacity (default: 1024)\n"
    "  --fail-every N       make every Nth task throw (default: 0, never)\n"
    "                       fault injection, for exercising the failure path\n"
    "  -h, --help           print this help and exit\n"
    "      --version        print the version and exit\n"
    "\n"
    "Exit codes:\n"
    "  0  every task succeeded\n"
    "  1  the run completed, but at least one task failed or was rejected\n"
    "  2  invalid usage or configuration\n"
    "\n"
    "Examples:\n"
    "  task-engine --workers 8 --tasks 10000 --work 500\n"
    "  task-engine --workers 1 --tasks 100 --work 0\n"
    "  task-engine --tasks 100 --fail-every 10\n";

// Parses a whole non-negative integer.
//
// from_chars rather than stoull or a stream: it does not throw, is not affected
// by the locale, reports overflow as a distinct condition, and tells us where it
// stopped, which is how trailing rubbish like "12abc" gets rejected instead of
// silently becoming 12. A leading minus sign is not part of the unsigned grammar
// it accepts, so negative input is rejected without a special case.
bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

bool parse_size(std::string_view text, std::size_t& out) {
    std::uint64_t value = 0;
    if (!parse_u64(text, value)) {
        return false;
    }
    if (value > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

ParseResult fail(std::string message) {
    ParseResult result;
    result.action = ParseResult::Action::Fail;
    result.error = std::move(message);
    return result;
}

std::string quoted(std::string_view text) { return "'" + std::string{text} + "'"; }

}  // namespace

std::size_t default_worker_count() noexcept {
    const unsigned detected = std::thread::hardware_concurrency();
    // hardware_concurrency returns 0 when it cannot tell. One worker is the
    // only safe answer then, and it is stated here rather than left to a
    // zero reaching the engine and throwing.
    return detected == 0 ? 1 : static_cast<std::size_t>(detected);
}

std::string_view usage() noexcept { return kUsage; }

int exit_code_for(std::size_t failed, std::size_t rejected) noexcept {
    // A run that lost work is not a success, however many tasks did complete.
    return (failed == 0 && rejected == 0) ? kExitSuccess : kExitTaskFailure;
}

ParseResult parse_args(const std::vector<std::string_view>& args) {
    Options options;
    options.workers = default_worker_count();
    options.tasks = kDefaultTasks;
    options.work = kDefaultWork;
    options.queue_capacity = kDefaultQueueCapacity;
    options.fail_every = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        if (arg == "-h" || arg == "--help") {
            ParseResult result;
            result.action = ParseResult::Action::ShowHelp;
            return result;
        }
        if (arg == "--version") {
            ParseResult result;
            result.action = ParseResult::Action::ShowVersion;
            return result;
        }

        // Every remaining option takes exactly one value.
        if (arg.size() < 3 || arg.substr(0, 2) != "--") {
            return fail("unrecognised argument " + quoted(arg));
        }
        if (i + 1 >= args.size()) {
            return fail("option " + quoted(arg) + " requires a value");
        }
        const std::string_view value = args[++i];

        if (arg == "--workers") {
            if (!parse_size(value, options.workers)) {
                return fail("--workers expects a non-negative integer, got " + quoted(value));
            }
        } else if (arg == "--tasks") {
            if (!parse_size(value, options.tasks)) {
                return fail("--tasks expects a non-negative integer, got " + quoted(value));
            }
        } else if (arg == "--work") {
            if (!parse_u64(value, options.work)) {
                return fail("--work expects a non-negative integer, got " + quoted(value));
            }
        } else if (arg == "--queue-capacity") {
            if (!parse_size(value, options.queue_capacity)) {
                return fail("--queue-capacity expects a non-negative integer, got " +
                            quoted(value));
            }
        } else if (arg == "--fail-every") {
            if (!parse_u64(value, options.fail_every)) {
                return fail("--fail-every expects a non-negative integer, got " + quoted(value));
            }
        } else {
            return fail("unrecognised option " + quoted(arg));
        }
    }

    // Validated here rather than left to the engine constructor, so the message
    // names the flag the user typed instead of an internal precondition.
    if (options.workers == 0) {
        return fail("--workers must be at least 1");
    }
    if (options.queue_capacity == 0) {
        return fail("--queue-capacity must be at least 1");
    }

    ParseResult result;
    result.action = ParseResult::Action::Run;
    result.options = options;
    return result;
}

}  // namespace taskengine
