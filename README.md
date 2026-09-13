# task-engine

A concurrent task-processing engine in C++17: a bounded blocking queue, a fixed
pool of worker threads, results delivered through `std::future`, two shutdown
modes, per-task timing, a command-line driver, and a repeatable benchmark.

The project is a study of ownership, lifetime and concurrency correctness rather
than a feature exercise. Every abstraction in it has to justify its own
existence, every non-obvious choice is recorded with the alternative that was
rejected, and every claim about performance comes from a run whose raw output is
committed.

> **Status: complete.** Milestones 0 to 8 are done. The roadmap ended at
> Milestone 8 by design ([D30](docs/DECISIONS.md)); there is no further planned
> work.

## Architecture

```
    task-engine (CLI)                     bench-task-engine
          |                                        |
          v                                        v
    +--------------------------------------------------------------+
    |  TaskEngine      assigns ids, submits, summarises the run    |
    +--------------------------------------------------------------+
          |  submit(unique_ptr<Task>)  ->  future<TaskResult>
          v
    +--------------------------------------------------------------+
    |  ThreadPool                                                  |
    |    BlockingQueue<TaskEnvelope>   bounded; one mutex,         |
    |                                  two condition variables     |
    |    N worker threads              created once, joined once   |
    |    per-worker Sample buffers     no lock on the record path  |
    +--------------------------------------------------------------+
          |  envelope.run() on a worker thread
          v
    +--------------------------------------------------------------+
    |  Task: ComputeTask | SleepTask                               |
    |  promise fulfilled exactly once: Succeeded, Failed, Rejected |
    +--------------------------------------------------------------+
```

The invariants the code is built around, and tested against:

- **Every future is fulfilled exactly once**, on every path — success, a thrown
  exception, refusal after shutdown, work abandoned by an abort, and the pool
  being destroyed underneath it. A caller blocked in `get()` is never left
  waiting and never sees a broken promise.
- **No thread per task.** Workers are created in the pool constructor and joined
  on shutdown; nothing is left running afterwards.
- **No polling in production code.** Every blocking wait is a condition variable
  wait with a predicate.
- **No raw owning pointers**, and no `shared_ptr`: ownership is `unique_ptr` and
  values. The pool's member declaration order is load-bearing — the queue the
  workers use is destroyed after the workers — and the header says so.
- **No silent fallback.** A partially successful run is never reported as a
  success, and every exit code means one thing.

Two shutdown modes: `shutdown()` drains, running everything already queued;
`shutdown_now()` abandons what is queued and reports each abandoned task as
rejected. Neither interrupts a task that is already running.

The full design — module graph, ownership, thread model, lifecycle, error
propagation, benchmarking and testing strategy — is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Thirty-four decisions, each with
its reason, the alternative considered and the trade-off, are in
[`docs/DECISIONS.md`](docs/DECISIONS.md).

## Requirements

Linux. Developed and tested on WSL2 Ubuntu 24.04 (D14). The tests read
`/proc/self/task` and use POSIX signals and process APIs.

| Tool | Minimum | Verified with |
|---|---|---|
| C++ compiler | C++17 | GCC 13.3.0 |
| CMake | 3.22 | 3.28.3 |
| Make | — | GNU Make 4.3 |
| Git | — | 2.43.0 |

```bash
sudo apt install build-essential cmake git
```

CMake 3.22 is the floor for a specific reason: below it, policy CMP0128 is OLD
and `CXX_EXTENSIONS OFF` is silently ignored whenever the compiler's default
standard already satisfies the request, so the build would compile as `gnu++17`
while claiming strict C++17 (D17).

## Build

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j"$(nproc)"

cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j"$(nproc)"
```

Build trees live under `build/` and are gitignored. Release is pinned to
`-O2 -DNDEBUG` (D16) and is the only configuration the benchmark will measure.
All of our own code compiles with
`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror`; third-party code
does not inherit that policy.

The build produces `task-engine`, `benchmarks/bench-task-engine`, and one test
binary per suite.

## Test

```bash
ctest --test-dir build/debug --output-on-failure
```

Each `TEST()` is its own CTest entry, so a failure names the case. Suites carry
labels:

```bash
ctest --test-dir build/debug -L unit          # fast, no threads
ctest --test-dir build/debug -L concurrency  # threaded behaviour
ctest --test-dir build/debug -L stress       # high volume, shutdown under load
ctest --test-dir build/debug -L integration  # the real binaries: exit codes, signals
```

GoogleTest is fetched at configure time, so the first configure needs network
access. `-DTASKENGINE_BUILD_TESTS=OFF` builds without it.

No test coordinates threads with a sleep. Two bounded waits on a predicate
remain, each because no correct alternative exists, and both are explained in
the code and in D33.

## Sanitizers

`TASKENGINE_SANITIZER` selects `off` (default), `address+undefined`, or
`thread`. AddressSanitizer and ThreadSanitizer cannot share a binary, so each
gets its own build tree.

```bash
# AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_SANITIZER=address+undefined
cmake --build build/asan -j"$(nproc)"
ctest --test-dir build/asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_SANITIZER=thread
setarch "$(uname -m)" -R cmake --build build/tsan -j"$(nproc)"
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R \
    ctest --test-dir build/tsan --output-on-failure

# shake out interleavings rather than sampling one
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R \
    ctest --test-dir build/tsan -L concurrency --repeat until-fail:25
```

`setarch -R` is needed for ThreadSanitizer on current kernels, WSL2 included:
TSan maps shadow memory at fixed addresses and aborts with
`unexpected memory mapping` under the modern ASLR default of
`vm.mmap_rnd_bits = 32`. Lowering that sysctl needs root; `setarch -R` disables
randomisation for one process tree and does not. It wraps the build too, because
`gtest_discover_tests` runs each test binary at build time.

## Usage

```bash
./build/release/task-engine --workers 8 --tasks 10000 --work 500
```

| Option | Default | Meaning |
|---|---|---|
| `--workers N` | hardware concurrency | worker threads |
| `--tasks N` | 1000 | tasks to submit |
| `--work N` | 1000 | compute iterations per task; `0` submits empty tasks |
| `--queue-capacity N` | 1024 | bounded queue capacity |
| `--fail-every N` | 0 (never) | make every Nth task throw — fault injection |
| `-h`, `--help` | | print help and exit |
| `--version` | | print the version and exit |

`--help` carries the same table and the exit codes.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | every task succeeded |
| `1` | the run completed but at least one task failed or was rejected, or the run was interrupted |
| `2` | invalid usage or configuration |

Results go to stdout and diagnostics to stderr, so a caller redirecting stdout
still learns why nothing came out of it.

### Interrupting a run

SIGINT and SIGTERM stop a run gracefully: submission stops, queued work is
abandoned and counted as rejected, tasks already running finish, the report is
printed with the interruption stated, and the exit code is 1. `docker stop`
behaves the same way.

### Examples

```bash
task-engine --workers 8 --tasks 5000 --work 2000        # exit 0
task-engine --tasks 100 --work 100 --fail-every 10       # exit 1, 10 failed
task-engine --workers 1 --tasks 20000 --work 0           # exit 0, engine overhead only
task-engine --workers 0                                  # exit 2
```

Output of the first example:

```
task-engine 0.1.0

configuration
  workers          8
  tasks            5000
  work             2000
  queue capacity   1024
  fail every       0

running...

outcome
  submitted        5000
  succeeded        5000
  failed           0
  rejected         0

  durations              min         p50         p95         max        mean
  queue wait        239.00ns      6.02us    160.18us    217.52us     24.00us
  execution           1.34us      1.45us      2.65us     65.19us      1.65us
  latency             1.65us      7.56us    162.12us    220.12us     25.65us

wall time          53.35ms
throughput         93716 tasks/s

Single run, no warm-up: these are what this invocation did, not a
benchmark. Use bench-task-engine for repeatable measurement.
```

The wall time and throughput are real measurements of that one invocation, and
the output says so. They are not benchmark results; see below.

## Benchmarks

```bash
./build/release/benchmarks/bench-task-engine > results.csv
```

A fixed experiment rather than a configurable tool (D34), so anyone running it
measures the same thing:

- **Profiles.** `overhead-floor`: empty tasks on one worker, the engine's
  per-task cost. `cpu-heavy`: tasks doing real work, where parallelism should
  help. `lightweight`: tasks so small that the engine's own synchronisation
  dominates. `blocking`: tasks that sleep, occupying a worker without a core.
- **Worker counts** 1, 2, 4, 8, 12 and 16.
- **Protocol.** One discarded warm-up, then five measured repetitions per
  configuration; median, minimum and maximum reported.
- **Setup versus steady state.** Task objects are built, and the engine and its
  threads created, before the clock starts. The timed region is from the first
  submit to the return of drain shutdown. Submission time is recorded separately
  inside it, so a run limited by the single submitting thread is visible as such.
- **Percentiles** by nearest rank, the same definition the engine uses.
- **Output** is CSV, with the machine, kernel, compiler, flags and time recorded
  in `#` comment lines.

The harness refuses to measure a Debug or sanitizer build. `--smoke` runs a tiny
version of the matrix in any build to prove the harness works; it is part of the
integration suite and is not a measurement.

### Results

| | |
|---|---|
| CPU | 12th Gen Intel Core i5-12450H, 12 logical CPUs |
| Kernel | 6.6.87.2-microsoft-standard-WSL2 |
| OS | Ubuntu 24.04.4 LTS, under WSL2 |
| Compiler | GCC 13.3.0, Release, `-O2 -DNDEBUG`, no sanitizer |
| Queue capacity | 1024 |
| Submitting threads | 1 |
| Recorded | 2026-09-13T02:02:20Z; load average 0.44 before, 0.90 after; 94 s |

The harness records the CPU model and logical CPU count. That this part is a
hybrid design — four performance cores with Hyper-Threading and four efficiency
cores — comes from the processor's published specification, not from anything
the harness measured, and matters to the interpretation below. WSL2 is a virtual
machine: these figures compare worker counts on one machine and are not
bare-metal numbers.

**Overhead floor** — empty tasks, one worker: **1,321,017 tasks/s** median
(1,296,516 – 1,354,052), about **757 ns per task** end to end through submit,
queue, dequeue, promise fulfilment and sample recording. Execution itself is
20 ns at the median; submission accounts for 151.1 ms of the 151.4 ms wall time.

**cpu-heavy** — 4,000 tasks of 200,000 iterations each:

| Workers | Tasks/s (median) | Min – max | Speedup | Execution p50 | Queue wait p50 |
|---:|---:|---:|---:|---:|---:|
| 1 | 6,549 | 6,508 – 6,621 | 1.00× | 142 µs | 155.7 ms |
| 2 | 12,637 | 12,245 – 12,985 | 1.93× | 145 µs | 79.7 ms |
| 4 | 21,617 | 20,967 – 21,973 | 3.30× | 161 µs | 47.0 ms |
| 8 | 30,475 | 29,933 – 30,701 | 4.65× | 287 µs | 33.4 ms |
| 12 | 37,907 | 37,074 – 38,052 | 5.79× | 301 µs | 26.6 ms |
| 16 | 33,432 | 31,656 – 37,247 | 5.11× | 302 µs | 26.8 ms |

**lightweight** — 200,000 tasks of 200 iterations each:

| Workers | Tasks/s (median) | Min – max | Speedup | Execution p50 | Queue wait p50 |
|---:|---:|---:|---:|---:|---:|
| 1 | 1,042,422 | 1,017,416 – 1,064,793 | 1.00× | 0.17 µs | 957 µs |
| 2 | 165,507 | 151,971 – 175,885 | 0.16× | 0.20 µs | 36 µs |
| 4 | 66,772 | 65,141 – 69,506 | 0.06× | 0.24 µs | 6.4 µs |
| 8 | 77,373 | 74,599 – 82,751 | 0.07× | 0.26 µs | 7.2 µs |
| 12 | 87,650 | 83,312 – 90,434 | 0.08× | 0.26 µs | 7.2 µs |
| 16 | 89,753 | 87,935 – 90,683 | 0.09× | 0.26 µs | 7.5 µs |

**blocking** — 400 tasks that each sleep 2 ms:

| Workers | Tasks/s (median) | Min – max | Speedup | Execution p50 | Queue wait p50 |
|---:|---:|---:|---:|---:|---:|
| 1 | 451 | 448 – 454 | 1.00× | 2.20 ms | 441 ms |
| 2 | 908 | 905 – 940 | 2.01× | 2.17 ms | 218 ms |
| 4 | 1,812 | 1,796 – 1,837 | 4.01× | 2.17 ms | 106 ms |
| 8 | 3,703 | 3,595 – 3,717 | 8.20× | 2.12 ms | 52.3 ms |
| 12 | 5,460 | 5,329 – 5,580 | 12.10× | 2.12 ms | 34.0 ms |
| 16 | 7,358 | 7,293 – 7,446 | 16.30× | 2.13 ms | 25.5 ms |

Every configuration completed with zero failed and zero rejected tasks.

The complete CSV for this run, including every latency column, is
[`benchmarks/results/2026-09-13-wsl2-i5-12450h.csv`](benchmarks/results/2026-09-13-wsl2-i5-12450h.csv).

### What the numbers show

Three different regimes, and one of them is the reason the benchmark exists.

**Blocking tasks scale with workers, not cores.** Throughput rose 16.3× at 16
workers on 12 logical CPUs (16.7× in the second run). A sleeping worker holds no
core, so workers beyond the core count still help — which is why the pool size
is configurable at all. The slight superlinearity is measured rather than
mysterious: each 2 ms sleep overran by less when more threads were busy, with
execution p50 falling from 2.20 ms at one worker to 2.13 ms at sixteen, and
16 × (2.20 / 2.13) ≈ 16.6 accounts for most of it.

**CPU-heavy tasks scale until the cores run out, and sooner than the core count
suggests.** Two workers gave 1.86–1.93×, four gave 3.30×, eight 4.7–4.8×, and
twelve or sixteen between 5.1× and 5.8×. The work in every task is identical and
deterministic, yet its measured execution time roughly doubled — from 142–146 µs
with one or two workers to 283–302 µs with eight or more. The same work taking
twice as long means each thread was getting about half a core's worth of
throughput. That is consistent with this CPU's layout of four performance cores
with Hyper-Threading plus four efficiency cores, where the fifth thread onward
lands on a sibling or a slower core. The harness does not pin threads, so it
cannot separate those two effects from each other or from the WSL2 scheduler.
The queue was not the limit: queue wait fell steadily as workers were added, and
at one worker it was close to what a full 1024-task queue draining at 142 µs a
task implies — about 145 ms, against 156 ms measured.

**Lightweight tasks got about 15× slower when workers were added.** One worker
handled about a million tasks a second. Two handled 162–166 thousand; four about
67 thousand. The tasks themselves ran in 0.17–0.27 µs throughout, and submission
accounted for more than 99.7% of wall time in every configuration, so the cost is
in handing tasks over, not in running them.

Context-switch counts show where it goes. With one worker the process made
**0.08 voluntary context switches per task**: the queue stayed full, the worker
never had to wait, and in glibc a condition-variable notification with no waiter
costs no system call. With four workers it made **1.21 per task** — fifteen times
as many, matching the fifteen-fold slowdown. Several workers drain the queue
faster than one thread can fill it, so they sit blocked on the condition
variable, queue wait drops to 6–8 µs, and nearly every submission has to wake a
worker with a system call and a context switch. Each task then costs about 15 µs
end to end, against under 1 µs with a single worker, for work that takes 0.2 µs.

**What this says about the design.** ARCHITECTURE.md predicted that lightweight
throughput would rise and then fall, with the queue mutex as the bottleneck. It
did not rise at all, and the evidence points at wake-ups rather than at time spent
holding the lock (D34). One mutex and a notification on every push is simple and
correct, costs nothing measurable when tasks are large, and pays for that
simplicity when they are tiny. The two task sizes bracket the crossover rather
than locate it: at 0.2 µs a second worker cost more than 6×, and at 142 µs it
gained 1.86–1.93×. For work that small, fewer workers is faster.

**How far to trust these figures.** A second, independent full run agreed within
4% for most configurations. The exceptions were the overhead floor at 6.9% and
the configurations at 12 or 16 workers, where the largest difference was 12.7%,
on the CPU-heavy profile at 16 workers. The first run put 12 workers ahead of 16
on that profile and the second put 16 ahead of 12, so no ordering between them is
claimed. The second run is in
[`benchmarks/results/2026-09-13-wsl2-i5-12450h-run2.csv`](benchmarks/results/2026-09-13-wsl2-i5-12450h-run2.csv).
Context-switch counts come from GNU `time -v` around the CLI with the same task
sizes, three runs per worker count, and are recorded in D34.

## Docker

A multi-stage build of the CLI (D31, D34):

```bash
docker build -t task-engine .
docker run --rm task-engine --workers 4 --tasks 10000 --work 500
docker run --rm task-engine            # no arguments: prints --help
```

- **Build stage.** Ubuntu 24.04 pinned by digest, the toolchain, a Release build,
  and the unit, concurrency and integration suites run inside the build. An image
  is only produced if those 125 tests pass inside it.
- **Runtime stage.** The same pinned base, the one binary, and an unprivileged
  system user (`uid=999`). No compiler, CMake, make or git; `ldd` shows only
  libstdc++, libgcc, libc and libm. `docker image inspect` reports 29.8 MB, of
  which the layers added on top of the base are the 106 kB binary and a 41 kB
  user entry.
- **Signals.** `docker stop` sends SIGTERM to PID 1; because the CLI installs its
  own handlers it stops gracefully, prints its report and exits 1.
- **Reproducibility.** The base image is pinned by digest; the apt packages on top
  resolve against the Ubuntu 24.04 archive at build time, so the build is
  reproducible in toolchain series rather than bit for bit.
- **Not a measurement environment.** Benchmark figures come from a Release build
  on the host, never from inside the container.

Validated with Docker 29.5.2, building from both a path context and a tar stream.

## Limitations and deliberate non-goals

Known limitations, each measured or stated rather than hidden:

- **Memory grows with run size.** Exact nearest-rank percentiles require keeping
  every sample, at 40 bytes per completed task. Observed directly: resident
  memory rose from 3.6 MB to 19.1 MB while about 460 thousand tasks completed.
  On an 8 GB machine that bounds a single run at roughly 10^8 tasks (D33).
- **One submitting thread** in both the CLI and the benchmark, so throughput can
  be limited by submission before it is limited by the workers. The benchmark
  measures this rather than assuming it away.
- **Running tasks are never interrupted.** Shutdown waits for them; there is no
  portable, safe way to stop a running thread. There is no per-task
  cancellation.
- **Interruption is noticed between submissions.** A submitter blocked on a full
  queue notices a signal only once a worker frees a slot.
- **A task must not submit to, or shut down, the engine running it.** Submission
  can deadlock against a full queue only that worker could drain; shutdown would
  have the worker join itself. Documented as unsupported rather than defended
  against (D5, D26).
- **Benchmark figures are from a laptop under WSL2**: valid for comparison across
  worker counts on that machine, not bare-metal numbers.
- **Linux only.** The tests use `/proc`, POSIX signals and process APIs.
- **The container build is reproducible in toolchain series**, not bit for bit:
  the base image is pinned by digest, the apt packages installed on it are not.
- **The CLI accepts space-separated values only**; `--workers=8` is not
  recognised.

Deliberately out of scope, and removed from the roadmap rather than deferred
([D30](docs/DECISIONS.md)): an HTTP service, a message-broker adapter, a data
store, cloud or cluster deployment, distributed scheduling, and GPU execution.

## Layout

```
CMakeLists.txt                project, C++17, warning and sanitizer policy, targets
Dockerfile, .dockerignore     multi-stage container build (D31, D34)
include/taskengine/
  core/                       Task, TaskResult, TaskEnvelope, Sample, state and ids
  concurrency/                BlockingQueue, ThreadPool
  execution/                  TaskEngine
  metrics/                    run summary and nearest-rank percentiles
  tasks/                      ComputeTask, SleepTask
  cli/                        argument parsing, validation, exit codes
src/                          library sources; src/app/main.cpp is the CLI
tests/unit/                   no threads             (label: unit)
tests/concurrency/            threaded behaviour     (label: concurrency)
tests/stress/                 volume, abort under load (label: stress)
tests/integration/            real binaries in child processes (label: integration)
tests/support/                gates, barrier, thread observation
benchmarks/                   bench-task-engine and committed results
docs/                         ARCHITECTURE.md, DECISIONS.md
PLAN.md, CLAUDE.md            roadmap and development rules
```

## Dependencies

GoogleTest, pinned to a commit and used in test builds only (D15). The library,
the CLI and the benchmark link nothing outside the C++ standard library and
POSIX.
