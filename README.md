# task-engine

A concurrent task-processing engine in C++17. The project is a study of
ownership, lifetime and concurrency correctness rather than a feature exercise:
every abstraction in it has to justify its own existence.

> **Status: Milestone 6 — command-line interface.**
> The engine runs from the command line: submit a configurable workload,
> get a report of what happened, and an exit code that distinguishes a clean
> run from one that lost work. Remaining: sanitizer and failure-mode
> hardening (M7), then benchmarking, packaging and polish (M8).
> [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) describes the design.

## Requirements

Linux. Developed and tested on WSL2 Ubuntu 24.04 (D14).

| Tool | Minimum | Verified with |
|---|---|---|
| C++ compiler | C++17 | GCC 13.3.0 |
| CMake | 3.22 | 3.28.3 |
| Make | — | GNU Make 4.3 |
| Git | — | 2.43.0 |

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake git
```

CMake 3.22 is the floor for a specific reason, not an arbitrary one: below it
policy CMP0128 is OLD and `CXX_EXTENSIONS OFF` is silently ignored whenever the
compiler's default standard already satisfies the request, so the build would
compile as `gnu++17` while claiming strict C++17.

## Build

```bash
# Debug
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j"$(nproc)"

# Release
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j"$(nproc)"
```

Build directories live under `build/` and are gitignored; nothing is ever
generated into the source tree. Debug uses `-g` with no optimisation. Release is
pinned to `-O2 -DNDEBUG` (D16) and is the only configuration benchmarks may use.

Both configurations compile our own code with
`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror`, so any warning
fails the build. Third-party code does not inherit this policy.

## Test

```bash
ctest --test-dir build/debug --output-on-failure
```

Each `TEST()` is registered as an individual CTest entry, so a failure names the
case rather than the binary. Suites carry labels, so a subset can be run
directly:

```bash
ctest --test-dir build/debug -L unit          # fast, no threads
ctest --test-dir build/debug -L concurrency  # threaded behaviour
```

GoogleTest is fetched from GitHub at *configure* time, so the first configure
needs network access. To build without it:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_BUILD_TESTS=OFF
```

## Sanitizers

`TASKENGINE_SANITIZER` selects one of `off` (default), `address+undefined`, or
`thread`. AddressSanitizer and ThreadSanitizer are mutually exclusive runtimes
and cannot share a binary, so each gets its own build directory.

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_SANITIZER=address+undefined
cmake --build build/asan -j"$(nproc)"
ctest --test-dir build/asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_SANITIZER=thread
setarch "$(uname -m)" -R cmake --build build/tsan -j"$(nproc)"
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R     ctest --test-dir build/tsan --output-on-failure
```

`setarch -R` is needed for ThreadSanitizer on current kernels, including WSL2.
TSan maps its shadow memory at fixed addresses and aborts with
`FATAL: ThreadSanitizer: unexpected memory mapping` when ASLR entropy is set to
the modern default of `vm.mmap_rnd_bits = 32`. Lowering that sysctl is the usual
fix and needs root; `setarch -R` disables randomisation for one process tree and
does not. It wraps the **build** as well as the test run, because
`gtest_discover_tests` runs the test binary at build time to enumerate cases.

Never benchmark a sanitizer build. ASan costs roughly 2x and TSan 5-15x, so a
sanitized timing is not a timing.

To shake out interleavings rather than sampling one:

```bash
ctest --test-dir build/tsan -L concurrency --repeat until-fail:25
```

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

`--help` carries the same table plus the exit codes, so the interface is
documented at the prompt and not only here.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | every task succeeded |
| `1` | the run completed, but at least one task failed or was rejected |
| `2` | invalid usage or configuration |

A partially successful run is never reported as success. Diagnostics go to
stderr, so a caller redirecting stdout still sees why nothing came out of it.

### Examples

```bash
# a clean run
task-engine --workers 8 --tasks 5000 --work 2000        # exit 0

# exercise the failure path: every 10th task throws
task-engine --tasks 100 --work 100 --fail-every 10       # exit 1, 10 failed

# engine overhead with no work in the tasks
task-engine --workers 1 --tasks 20000 --work 0           # exit 0

# a usage mistake
task-engine --workers 0                                  # exit 2
```

Sample output:

```
outcome
  submitted        5000
  succeeded        5000
  failed           0
  rejected         0

  durations              min         p50         p95         max        mean
  queue wait        275.00ns      6.69us     46.68us    264.79us     10.80us
  execution           1.38us      1.56us      1.61us     14.94us      1.62us
  latency             1.80us      8.27us     48.54us    266.40us     12.42us
```

The wall time and throughput the CLI prints are real measurements of that one
invocation, and the output says so: single run, no warm-up, not a benchmark.
Repeatable measurement — warm-up, repeats, medians, environment capture — is a
separate concern and arrives with its own binary at M8.

## Layout

```
CMakeLists.txt              project, C++17, warning policy, targets
include/taskengine/         public headers
src/                        library sources and the executable entry point
include/taskengine/cli/     argument parsing, validation, exit codes
include/taskengine/tasks/   concrete workloads (ComputeTask, SleepTask)
src/app/main.cpp            composition root
tests/unit/                 unit suite (CTest label: unit)
tests/concurrency/          threaded suite (CTest label: concurrency)
docs/                       architecture and decision records
build/                      build trees (gitignored)
```

Directories appear in the milestone that needs them. There are no placeholder
files for future work.

## Design

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — target architecture: module
  graph, ownership model, thread model, task lifecycle, shutdown semantics,
  error propagation, benchmarking model, sanitizer and testing strategy.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — every non-obvious decision, with
  its reason, the alternative considered, and its trade-off.
- [`PLAN.md`](PLAN.md) — the milestone roadmap.

## Dependencies

GoogleTest, test builds only. The library and executable link nothing outside
the C++ standard library.
