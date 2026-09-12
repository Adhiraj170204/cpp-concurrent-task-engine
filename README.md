# task-engine

A concurrent task-processing engine in C++17. The project is a study of
ownership, lifetime and concurrency correctness rather than a feature exercise:
every abstraction in it has to justify its own existence.

> **Status: Milestone 2 — task model.**
> The engine is not running tasks yet. What exists today is the CMake/C++17
> foundation plus the core task model: the `Task` interface and the result,
> error and timing types, with their ownership and lifetime behaviour under
> test. The blocking queue, thread pool, metrics, CLI and benchmarks arrive
> in later milestones. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
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
ctest --test-dir build/debug -L unit
```

GoogleTest is fetched from GitHub at *configure* time, so the first configure
needs network access. To build without it:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DTASKENGINE_BUILD_TESTS=OFF
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
