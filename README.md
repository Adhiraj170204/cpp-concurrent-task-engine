# task-engine

A concurrent task-processing engine in C++17. The project is a study of
ownership, lifetime and concurrency correctness rather than a feature exercise:
every abstraction in it has to justify its own existence.

> **Status: Milestone 3 — blocking queue.**
> The engine is not running tasks yet. What exists today is the CMake/C++17
> foundation, the core task model, and a bounded thread-safe producer/
> consumer queue with its concurrency suite. The thread pool, metrics, CLI
> and benchmarks arrive in later milestones.
> [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> describes the design they will follow.

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

## Run

```bash
./build/debug/task-engine
```

Prints the program name and version. That is all it does at this milestone.

## Layout

```
CMakeLists.txt              project, C++17, warning policy, targets
include/taskengine/         public headers
src/                        library sources and the executable entry point
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
