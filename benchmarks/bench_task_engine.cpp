// bench-task-engine: the repeatable worker-scaling benchmark.
//
// A fixed experiment rather than a configurable tool. It always runs the same
// matrix, so two people who run it on the same machine are measuring the same
// thing, and there are no flags whose values a published number silently
// depends on. Methodology, per ARCHITECTURE.md section 8:
//
//   - profiles: an overhead floor, a CPU-heavy workload, a lightweight workload
//     and a blocking workload
//   - worker counts 1, 2, 4, 8, 12 and 16, plus the hardware concurrency if it is
//     not already among them
//   - one warm-up run per configuration, discarded
//   - five measured repetitions per configuration; median, minimum and maximum
//     reported, never a single run
//   - setup excluded from timing: every task object is built, and the engine
//     and its worker threads are constructed, before the clock starts
//   - timed region: from the first submit to the return of drain shutdown
//   - percentiles by nearest rank, the same definition the engine uses
//   - CSV on stdout, with the environment recorded in comment lines
//
// The harness refuses to measure a sanitizer build or a non-Release build: a
// sanitized timing is not a timing, and an unoptimised one is not the program.
// --smoke runs a tiny version of the matrix to prove the harness works end to
// end; it is permitted in any build because it is explicitly not a measurement.

#include "taskengine/core/task.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/execution/task_engine.hpp"
#include "taskengine/metrics/run_summary.hpp"
#include "taskengine/tasks/compute_task.hpp"
#include "taskengine/tasks/sleep_task.hpp"
#include "taskengine/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using taskengine::ComputeTask;
using taskengine::RunSummary;
using taskengine::SleepTask;
using taskengine::Task;
using taskengine::TaskEngine;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kBuildType = TASKENGINE_BUILD_TYPE;
constexpr std::string_view kSanitizerMode = TASKENGINE_SANITIZER_MODE;
constexpr std::string_view kOptimisationFlags = TASKENGINE_OPTIMISATION_FLAGS;

constexpr std::size_t kQueueCapacity = 1024;
constexpr int kExitOk = 0;
constexpr int kExitLostWork = 1;
constexpr int kExitRefused = 2;

struct Profile {
    std::string_view name;
    std::size_t tasks;
    std::uint64_t work;               // ComputeTask iterations, for compute profiles
    std::chrono::microseconds sleep;  // SleepTask duration; zero means a compute profile
    std::vector<std::size_t> workers;
};

struct Plan {
    int warmups;
    int repetitions;
    std::vector<Profile> profiles;
};

struct Measurement {
    double wall_seconds{};
    double submit_seconds{};
    double throughput{};
    RunSummary summary{};
};

std::vector<std::size_t> worker_sweep() {
    std::vector<std::size_t> counts{1, 2, 4, 8, 12, 16};
    const unsigned detected = std::thread::hardware_concurrency();
    if (detected != 0 &&
        std::find(counts.begin(), counts.end(), static_cast<std::size_t>(detected)) == counts.end()) {
        counts.push_back(static_cast<std::size_t>(detected));
        std::sort(counts.begin(), counts.end());
    }
    return counts;
}

Plan measurement_plan() {
    const std::vector<std::size_t> sweep = worker_sweep();
    return Plan{1, 5,
                {
                    // Engine cost with no work in the tasks: the floor every other
                    // number sits on. One worker, so it is not a scaling question.
                    Profile{"overhead-floor", 200000, 0, std::chrono::microseconds{0}, {1}},
                    // Real work dominates; useful parallelism.
                    Profile{"cpu-heavy", 4000, 200000, std::chrono::microseconds{0}, sweep},
                    // Engine overhead dominates; synchronisation cost.
                    Profile{"lightweight", 200000, 200, std::chrono::microseconds{0}, sweep},
                    // Occupies a worker without a core; scales with workers.
                    Profile{"blocking", 400, 0, std::chrono::microseconds{2000}, sweep},
                }};
}

Plan smoke_plan() {
    return Plan{1, 2,
                {
                    Profile{"overhead-floor", 2000, 0, std::chrono::microseconds{0}, {1}},
                    Profile{"cpu-heavy", 50, 2000, std::chrono::microseconds{0}, {1, 2}},
                    Profile{"lightweight", 2000, 10, std::chrono::microseconds{0}, {1, 2}},
                    Profile{"blocking", 20, 0, std::chrono::microseconds{100}, {1, 2}},
                }};
}

double seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

double microseconds(taskengine::TaskTimings::Duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

// Median by nearest rank, the same definition the engine uses for percentiles,
// so every "middle value" in the output means one thing.
double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[taskengine::nearest_rank_index(50, values.size())];
}

Measurement measure_once(const Profile& profile, std::size_t workers) {
    // Setup, excluded from timing. Building a task object is the submitters
    // cost, not the engines, and creating threads is a one-off.
    std::vector<std::unique_ptr<Task>> tasks;
    tasks.reserve(profile.tasks);
    for (std::size_t i = 0; i < profile.tasks; ++i) {
        if (profile.sleep.count() > 0) {
            tasks.push_back(std::make_unique<SleepTask>(profile.sleep));
        } else {
            tasks.push_back(std::make_unique<ComputeTask>(profile.work, static_cast<std::uint64_t>(i)));
        }
    }
    TaskEngine engine{workers, kQueueCapacity};

    // Timed region: submission and complete execution.
    const Clock::time_point start = Clock::now();
    for (std::unique_ptr<Task>& task : tasks) {
        (void)engine.submit(std::move(task));
    }
    const Clock::time_point submitted = Clock::now();
    engine.shutdown();
    const Clock::time_point finished = Clock::now();

    Measurement measurement;
    measurement.wall_seconds = seconds(finished - start);
    // Recorded separately so that a run limited by the single submitting thread
    // is visible as such, rather than being mistaken for a limit in the workers.
    measurement.submit_seconds = seconds(submitted - start);
    measurement.summary = engine.summary();
    measurement.throughput = measurement.wall_seconds > 0.0
                                 ? static_cast<double>(profile.tasks) / measurement.wall_seconds
                                 : 0.0;
    return measurement;
}

std::string first_line_containing(const char* path, std::string_view key) {
    std::ifstream file{path};
    std::string line;
    while (std::getline(file, line)) {
        if (line.find(key) != std::string::npos) {
            return line;
        }
    }
    return {};
}

std::string after_separator(const std::string& line, char separator) {
    const std::size_t position = line.find(separator);
    if (position == std::string::npos) {
        return "unknown";
    }
    std::string value = line.substr(position + 1);
    const std::size_t begin = value.find_first_not_of(" \t\"");
    const std::size_t end = value.find_last_not_of(" \t\"");
    return begin == std::string::npos ? std::string{"unknown"} : value.substr(begin, end - begin + 1);
}

std::string read_first_line(const char* path) {
    std::ifstream file{path};
    std::string line;
    std::getline(file, line);
    return line.empty() ? std::string{"unknown"} : line;
}

std::string compiler_identity() {
#if defined(__clang__)
    return std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    return std::string{"gcc "} + __VERSION__;
#else
    return "unknown";
#endif
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::ostringstream out;
    out << std::put_time(std::gmtime(&now), "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void print_environment(std::ostream& out, const Plan& plan, bool smoke) {
    const std::string kernel = read_first_line("/proc/sys/kernel/osrelease");
    const bool wsl = kernel.find("microsoft") != std::string::npos ||
                     kernel.find("WSL") != std::string::npos;

    out << "# bench-task-engine " << taskengine::version() << "\n";
    if (smoke) {
        out << "# SMOKE RUN: proves the harness works end to end. Not a measurement.\n";
    }
    out << "# methodology: " << plan.warmups << " warm-up run discarded, then " << plan.repetitions
        << " measured repetitions per configuration; median (min, max) reported\n";
    out << "# timed region: first submit to return of drain shutdown; task construction "
           "and engine/thread creation are setup and excluded\n";
    out << "# percentiles: nearest rank; latency columns are the median across repetitions "
           "of each repetition's percentile\n";
    out << "# queue_capacity: " << kQueueCapacity << "\n";
    out << "# submitters: 1\n";
    out << "# cpu: " << after_separator(first_line_containing("/proc/cpuinfo", "model name"), ':')
        << "\n";
    out << "# logical_cpus: " << std::thread::hardware_concurrency() << "\n";
    out << "# kernel: " << kernel << "\n";
    out << "# os: " << after_separator(first_line_containing("/etc/os-release", "PRETTY_NAME"), '=')
        << "\n";
    out << "# virtualisation: " << (wsl ? "WSL2 (numbers are relative, not bare-metal)"
                                        : "no WSL marker detected")
        << "\n";
    out << "# compiler: " << compiler_identity() << "\n";
    out << "# build_type: " << kBuildType << "\n";
    out << "# optimisation_flags: " << kOptimisationFlags << "\n";
    out << "# sanitizer: " << kSanitizerMode << "\n";
    out << "# utc_time: " << utc_now() << "\n";
}

void print_header(std::ostream& out) {
    out << "profile,workers,tasks,work,sleep_us,repetitions,"
           "throughput_median,throughput_min,throughput_max,speedup_vs_1_worker,"
           "wall_ms_median,submit_ms_median,"
           "queue_wait_p50_us,queue_wait_p95_us,exec_p50_us,exec_p95_us,"
           "latency_p50_us,latency_p95_us,failed,rejected\n";
}

// Runs one profile across its worker counts and writes a row per count.
// Returns the number of tasks that failed or were rejected, which a benchmark
// run must never have.
std::size_t run_profile(std::ostream& out, const Plan& plan, const Profile& profile) {
    std::size_t lost = 0;
    double single_worker_throughput = 0.0;

    for (const std::size_t workers : profile.workers) {
        std::cerr << "  " << profile.name << " workers=" << workers << " ..." << std::flush;

        for (int i = 0; i < plan.warmups; ++i) {
            (void)measure_once(profile, workers);  // discarded
        }

        std::vector<double> throughput;
        std::vector<double> wall_ms;
        std::vector<double> submit_ms;
        std::vector<double> wait_p50;
        std::vector<double> wait_p95;
        std::vector<double> exec_p50;
        std::vector<double> exec_p95;
        std::vector<double> latency_p50;
        std::vector<double> latency_p95;
        std::size_t failed = 0;
        std::size_t rejected = 0;

        for (int i = 0; i < plan.repetitions; ++i) {
            const Measurement m = measure_once(profile, workers);
            throughput.push_back(m.throughput);
            wall_ms.push_back(m.wall_seconds * 1e3);
            submit_ms.push_back(m.submit_seconds * 1e3);
            wait_p50.push_back(microseconds(m.summary.queue_wait.p50));
            wait_p95.push_back(microseconds(m.summary.queue_wait.p95));
            exec_p50.push_back(microseconds(m.summary.execution_time.p50));
            exec_p95.push_back(microseconds(m.summary.execution_time.p95));
            latency_p50.push_back(microseconds(m.summary.total_latency.p50));
            latency_p95.push_back(microseconds(m.summary.total_latency.p95));
            failed += m.summary.failed;
            rejected += m.summary.rejected;
        }

        const double median_throughput = median(throughput);
        if (workers == 1) {
            single_worker_throughput = median_throughput;
        }
        const double speedup =
            single_worker_throughput > 0.0 ? median_throughput / single_worker_throughput : 0.0;

        out << profile.name << ',' << workers << ',' << profile.tasks << ',' << profile.work << ','
            << profile.sleep.count() << ',' << plan.repetitions << ',' << std::fixed
            << std::setprecision(0) << median_throughput << ','
            << *std::min_element(throughput.begin(), throughput.end()) << ','
            << *std::max_element(throughput.begin(), throughput.end()) << ','
            << std::setprecision(2) << speedup << ',' << std::setprecision(3) << median(wall_ms)
            << ',' << median(submit_ms) << ',' << median(wait_p50) << ',' << median(wait_p95)
            << ',' << median(exec_p50) << ',' << median(exec_p95) << ',' << median(latency_p50)
            << ',' << median(latency_p95) << ',' << failed << ',' << rejected << '\n';
        out.flush();

        std::cerr << " " << std::fixed << std::setprecision(0) << median_throughput
                  << " tasks/s\n";
        lost += failed + rejected;
    }
    return lost;
}

void print_usage(std::ostream& out) {
    out << "bench-task-engine - repeatable worker-scaling benchmark for task-engine\n"
           "\n"
           "Usage:\n"
           "  bench-task-engine            run the full measurement matrix (Release only)\n"
           "  bench-task-engine --smoke    tiny matrix proving the harness runs; not a measurement\n"
           "  bench-task-engine --help     print this help\n"
           "\n"
           "Writes CSV to stdout, with the environment in '#' comment lines, and progress\n"
           "to stderr. The matrix is fixed on purpose: see ARCHITECTURE.md section 8.\n"
           "\n"
           "Exit codes: 0 completed, 1 a task failed or was rejected, 2 refused or misused.\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return kExitOk;
        }
        if (arg == "--smoke") {
            smoke = true;
            continue;
        }
        std::cerr << "bench-task-engine: unrecognised argument '" << arg << "'\n\n";
        print_usage(std::cerr);
        return kExitRefused;
    }

    if (!smoke && kSanitizerMode != "off") {
        std::cerr << "bench-task-engine: refusing to measure a sanitizer build (" << kSanitizerMode
                  << "). A sanitized timing is not a timing. Build with "
                     "-DCMAKE_BUILD_TYPE=Release and no TASKENGINE_SANITIZER.\n";
        return kExitRefused;
    }
    if (!smoke && kBuildType != "Release") {
        std::cerr << "bench-task-engine: refusing to measure a " << kBuildType
                  << " build. Benchmarks are taken from Release builds only.\n";
        return kExitRefused;
    }

    const Plan plan = smoke ? smoke_plan() : measurement_plan();

    print_environment(std::cout, plan, smoke);
    print_header(std::cout);

    std::size_t lost = 0;
    for (const Profile& profile : plan.profiles) {
        lost += run_profile(std::cout, plan, profile);
    }

    if (lost != 0) {
        std::cerr << "bench-task-engine: " << lost
                  << " tasks failed or were rejected; these results are not valid\n";
        return kExitLostWork;
    }
    return kExitOk;
}
