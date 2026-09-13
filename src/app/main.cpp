// Composition root. The one place that constructs concrete objects and wires
// them together, so everything below it stays injectable and testable.

#include "taskengine/cli/options.hpp"
#include "taskengine/core/task.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/execution/task_engine.hpp"
#include "taskengine/metrics/run_summary.hpp"
#include "taskengine/tasks/compute_task.hpp"
#include "taskengine/version.hpp"

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using taskengine::ComputeTask;
using taskengine::Options;
using taskengine::ParseResult;
using taskengine::RunSummary;
using taskengine::Task;
using taskengine::TaskEngine;
using taskengine::TaskTimings;

// Set from a signal handler and read by the submitting thread.
//
// std::atomic<bool> rather than volatile sig_atomic_t. The latter only has
// defined behaviour between a handler and the thread it interrupted, but in a
// multithreaded process the kernel may deliver SIGINT to any thread that does
// not block it -- including a worker. The handler write and the main-thread read
// would then be a data race. C++17 [support.signal] permits plain lock-free
// atomic operations inside a signal handler, and an atomic is also safe across
// threads, so this one type satisfies both requirements. The static_assert
// makes the lock-free precondition a build failure rather than an assumption.
std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "a signal handler may only use lock-free atomics");

extern "C" void handle_interrupt(int /*signal*/) { g_interrupted.store(true); }

// sigaction rather than std::signal: the handler is guaranteed to stay
// installed, and the mask and flags are explicit rather than
// implementation-defined.
void install_interrupt_handlers() {
    struct sigaction action {};
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

// Fault injection for --fail-every.
//
// Deliberately local to the application rather than a library workload: its only
// purpose is to make the documented failure exit code reachable from a shell.
// Promoting it to tasks/ would put a type in the library whose whole job is to
// throw.
class FailingTask final : public Task {
public:
    void execute() override { throw std::runtime_error{"injected failure"}; }
};

// Adaptive units, so a microsecond and a second are both readable without the
// reader counting zeroes.
std::string format_duration(TaskTimings::Duration duration) {
    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());

    const char* unit = "ns";
    double scaled = nanoseconds;
    if (nanoseconds >= 1e9) {
        unit = "s";
        scaled = nanoseconds / 1e9;
    } else if (nanoseconds >= 1e6) {
        unit = "ms";
        scaled = nanoseconds / 1e6;
    } else if (nanoseconds >= 1e3) {
        unit = "us";
        scaled = nanoseconds / 1e3;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << scaled << unit;
    return out.str();
}

void print_stats_row(std::ostream& out, std::string_view label,
                     const taskengine::DurationStats& stats) {
    out << "  " << std::left << std::setw(14) << label << std::right << std::setw(12)
        << format_duration(stats.min) << std::setw(12) << format_duration(stats.p50)
        << std::setw(12) << format_duration(stats.p95) << std::setw(12)
        << format_duration(stats.max) << std::setw(12) << format_duration(stats.mean()) << "\n";
}

// Printed before the run starts, so the configuration is visible immediately
// rather than only in retrospect if the run is long or is interrupted.
void print_configuration(std::ostream& out, const Options& options) {
    out << "task-engine " << taskengine::version() << "\n\n";
    out << "configuration\n";
    out << "  workers          " << options.workers << "\n";
    out << "  tasks            " << options.tasks << "\n";
    out << "  work             " << options.work << "\n";
    out << "  queue capacity   " << options.queue_capacity << "\n";
    out << "  fail every       " << options.fail_every << "\n\n";
    // Flushed on purpose: this is the point after which the process is busy,
    // and anything watching the output needs to have seen it by now.
    out << "running..." << std::endl;
}

void print_outcome(std::ostream& out, const RunSummary& summary, TaskTimings::Duration wall_time,
                   std::size_t requested, bool interrupted) {
    out << "\noutcome\n";
    if (interrupted) {
        out << "  interrupted      yes, after " << summary.total << " of " << requested
            << " submissions\n";
    }
    out << "  submitted        " << summary.total << "\n";
    out << "  succeeded        " << summary.succeeded << "\n";
    out << "  failed           " << summary.failed << "\n";
    out << "  rejected         " << summary.rejected << "\n\n";

    if (summary.total_latency.count > 0) {
        // Header widths mirror print_stats_row exactly: a two-space indent, a
        // 14-wide label column, then five 12-wide value columns.
        out << std::left << std::setw(16) << "  durations" << std::right << std::setw(12) << "min"
            << std::setw(12) << "p50" << std::setw(12) << "p95" << std::setw(12) << "max"
            << std::setw(12) << "mean" << "\n";
        print_stats_row(out, "queue wait", summary.queue_wait);
        print_stats_row(out, "execution", summary.execution_time);
        print_stats_row(out, "latency", summary.total_latency);
        out << "\n";
    }

    out << "wall time          " << format_duration(wall_time) << "\n";

    const double seconds = std::chrono::duration<double>(wall_time).count();
    out << "throughput         ";
    if (seconds > 0.0 && summary.total > 0) {
        out << std::fixed << std::setprecision(0)
            << static_cast<double>(summary.total) / seconds << " tasks/s\n";
    } else {
        // Not a number worth inventing. A run too short to time says so.
        out << "n/a (run too short to measure)\n";
    }

    out << "\nSingle run, no warm-up: these are what this invocation did, not a\n"
           "benchmark. Use bench-task-engine for repeatable measurement.\n";
}

std::unique_ptr<Task> make_task(const Options& options, std::size_t index) {
    const bool inject_failure = options.fail_every != 0 && ((index + 1) % options.fail_every) == 0;
    if (inject_failure) {
        return std::make_unique<FailingTask>();
    }
    // Index as the seed, so tasks differ from one another while the run as a
    // whole stays reproducible.
    return std::make_unique<ComputeTask>(options.work, static_cast<std::uint64_t>(index));
}

int run(const Options& options) {
    install_interrupt_handlers();

    print_configuration(std::cout, options);

    TaskEngine engine{options.workers, options.queue_capacity};

    const TaskTimings::TimePoint start = TaskTimings::Clock::now();

    std::size_t submitted = 0;
    for (; submitted < options.tasks; ++submitted) {
        if (g_interrupted.load()) {
            break;
        }
        // The future is discarded on purpose. Counts come from the run summary,
        // and a future backed by a promise -- unlike one from std::async -- does
        // not block when it is destroyed, so nothing is serialised by dropping
        // it. Dropping it also avoids holding a promise shared state per task.
        // Memory still grows with --tasks, by one 40-byte Sample per completed
        // task, because exact percentiles need every sample (D10, D33).
        (void)engine.submit(make_task(options, submitted));
    }

    const bool interrupted = g_interrupted.load();
    if (interrupted) {
        // Abandon what is queued rather than finishing a batch the operator has
        // asked to stop. Work already running still completes and still reports
        // its real outcome: there is no safe way to interrupt a running task,
        // and every abandoned task is reported as rejected rather than dropped.
        engine.shutdown_now();
    } else {
        engine.shutdown();
    }

    const TaskTimings::Duration wall_time = TaskTimings::Clock::now() - start;
    const RunSummary summary = engine.summary();

    print_outcome(std::cout, summary, wall_time, options.tasks, interrupted);

    if (interrupted) {
        // The run did not do what was asked, so it is not a success even if
        // every task that did run succeeded.
        std::cerr << "task-engine: interrupted before submitting all tasks\n";
        return taskengine::kExitTaskFailure;
    }
    return taskengine::exit_code_for(summary.failed, summary.rejected);
}

}  // namespace

int main(int argc, char** argv) {
    // argv[0] is the program name; guard against an argc of zero, which is
    // permitted by the standard even though it is rare.
    const int first = argc > 0 ? 1 : 0;
    const std::vector<std::string_view> args(argv + first, argv + argc);

    const ParseResult parsed = taskengine::parse_args(args);

    switch (parsed.action) {
        case ParseResult::Action::ShowHelp:
            std::cout << taskengine::usage();
            return taskengine::kExitSuccess;

        case ParseResult::Action::ShowVersion:
            std::cout << "task-engine " << taskengine::version() << "\n";
            return taskengine::kExitSuccess;

        case ParseResult::Action::Fail:
            // Diagnostics to stderr, so a caller redirecting stdout still sees
            // why nothing came out of it.
            std::cerr << "task-engine: " << parsed.error << "\n\n" << taskengine::usage();
            return taskengine::kExitUsage;

        case ParseResult::Action::Run:
            break;
    }

    try {
        return run(parsed.options);
    } catch (const std::invalid_argument& e) {
        // A configuration the engine itself rejects. Same class of mistake as a
        // bad flag, so the same exit code.
        std::cerr << "task-engine: invalid configuration: " << e.what() << "\n";
        return taskengine::kExitUsage;
    } catch (const std::exception& e) {
        // The run started and could not finish. Reported as a failure rather
        // than dressed up as a partial success.
        std::cerr << "task-engine: run failed: " << e.what() << "\n";
        return taskengine::kExitTaskFailure;
    }
}
