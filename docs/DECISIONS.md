# Design Decisions

Each entry records **decision / reason / alternative considered / trade-off**,
per `CLAUDE.md`. A decision is added here *before* the code that depends on it.

Status values: **Accepted** (approved, may or may not be implemented yet) ·
**Revisit at Cn** (accepted with a scheduled re-examination) · **Superseded**.

Architectural context: [ARCHITECTURE.md](ARCHITECTURE.md).

---

## D1 — Polymorphic `Task` owned by `std::unique_ptr`, not `std::function<void()>`

**Status:** Accepted — **revisit at C2** (end of M5) · **Milestone:** M2

**Decision.** The unit of work is an abstract base class:

```cpp
class Task {
public:
    virtual ~Task() = default;
    virtual void execute() = 0;   // may throw; the engine catches
};
```

owned and transferred as `std::unique_ptr<Task>`.

**Reason.** At M11 a broker message is deserialized and its payload *selects the
concrete task type at runtime*; a factory returning `std::unique_ptr<Task>` is
the natural shape, and the type cannot be known at compile time. That is a
genuinely open set of types, which is what virtual dispatch is for. Ownership
transfer through a move-only `unique_ptr` is also explicit in a way a copyable
`std::function` is not.

**Alternative considered.** `std::function<void()>` — less code, no hierarchy,
no virtual call. Rejected because it erases task identity and type, is copyable
(a weaker ownership story), and cannot support runtime type selection from a
message payload.

**Trade-off.** One heap allocation and one virtual call per task. Expected to be
negligible against the workload, but this will be **measured** via the overhead
floor (ARCHITECTURE.md §8.2), not asserted.

**Checkpoint C2.** If, at the end of M5, the concrete types differ only by a
parameter and nothing dispatches on type at runtime, collapse to
`std::function<void()>` and delete the hierarchy.

---

## D2 — Engine bookkeeping lives in `TaskEnvelope`, not on `Task`

**Status:** Accepted · **Milestone:** M5

**Decision.** `TaskEnvelope` holds `std::unique_ptr<Task>`, the `TaskId`, the
three timestamps, and the `std::promise<TaskResult>`. `Task` itself holds only
user work.

**Reason.** A user-authored task should not carry engine plumbing. Keeping
`Task` minimal makes it trivial to implement and to unit-test with no engine
present.

**Alternative considered.** Put the promise and timings on the base class —
fewer types, but the engine leaks into every user type, and `Task` could no
longer be constructed standalone in a test.

**Trade-off.** One additional struct.

**Consequence.** `TaskEnvelope` holds two move-only members, so it is move-only
by the rule of zero. This is invariant **I2**.

---

## D3 — `submit()` returns `std::future<TaskResult>`

**Status:** Accepted · **Milestone:** M5

**Decision.** `TaskEngine::submit(std::unique_ptr<Task>)` returns
`std::future<TaskResult>`, backed by the `std::promise` in the envelope.

**Reason.** Standard library, no dependency; a natural "task status" hook for
the FastAPI layer at M10; and the promise/future pair gives a single, uniform
place to enforce invariant **I3** (every future fulfilled exactly once).

**Alternative considered.** A callback sink, or polling a result map. A map
requires another mutex, another lifetime question, and an eviction policy — more
shared mutable state for less capability.

**Trade-off.** Callers that do not want a result must discard the future. Worth
noting explicitly: a discarded `std::future` from a `promise` does **not** block
on destruction (unlike one from `std::async`), so the benchmark driver can
discard freely. One promise allocation per task, already counted in D1's
overhead floor.

---

## D4 — Task exceptions become `set_value(Failed)`, never `set_exception`

**Status:** Accepted · **Milestone:** M4

**Decision.** The worker loop wraps `execute()` in
`catch (const std::exception&)` and `catch (...)`, converts the failure to
`TaskState::Failed` with the `what()` text (or `"unknown exception"`), and
fulfils the promise with **`set_value`**.

**Reason.** Two separate things. First, an exception escaping a thread function
calls `std::terminate` — catching is mandatory, not stylistic. Second, a failed
task is a normal, expected, countable outcome; making it a control-flow event
would force every caller into a try/catch and make "how many tasks failed"
awkward to compute.

**Alternative considered.** `promise.set_exception(std::current_exception())` —
preserves the exception type and rethrows on `get()`. Rejected for the counting
and caller-ergonomics reasons above.

**Trade-off.** The exception *type* is lost; only the message survives. If a
caller ever needs to discriminate failure types, that is a change to
`TaskError`, not a reversal of this decision.

---

## D5 — Bounded `BlockingQueue` with blocking push (backpressure)

**Status:** Accepted · **Milestone:** M3

**Decision.** The queue has a fixed capacity set at construction. `push` blocks
on a `not_full_` condition variable when the queue is full.

**Reason.** An unbounded queue converts a producer/consumer rate mismatch into
unbounded memory growth and hides it. Bounded is the honest behaviour for a
system that will later be fed by a message broker, and it is what makes the
queue-wait metric meaningful rather than an artefact of unlimited buffering.

**Alternative considered.** An unbounded queue — simpler, one condition variable
instead of two, no deadlock risk below.

**Trade-off — documented limitation.** A `Task::execute()` that calls `submit()`
on the engine running it can deadlock permanently: the worker blocks in `push`,
which only another worker popping can release, and if all workers block the same
way nothing progresses. **Recursive submission is unsupported by design.** We do
not defend against it — an unbounded overflow path, a second pool, or an
inline-execute fallback each cost more than the capability is worth, and
inline-execute would additionally violate invariant I6 (no silent fallback).

The limitation is stated in three places: a comment on `TaskEngine::submit`,
`README.md`, and here.

---

## D6 — Two explicit shutdown modes, plus RAII shutdown

**Status:** Accepted · **Milestone:** M4

**Decision.**
- `shutdown()` — **drain**: stop accepting; finish everything already queued;
  join.
- `shutdown_now()` — **abort**: stop accepting; discard queued envelopes, each
  fulfilled as `Rejected`; let the in-flight task finish; join.
- `~ThreadPool()` calls `shutdown()` and joins.
- Both are idempotent and callable from any thread.
- Neither interrupts a running `execute()`.

**Reason.** M4 requires deterministic shutdown and no joinable threads
afterwards; M13 requires "shutdown while work is pending" to be a tested
behaviour. Two *named* modes are unambiguous where a single boolean flag is not.
RAII guarantees the join even on an exception path.

**Alternative considered.** A single mode — insufficient to express both "finish
the batch" and "stop now". Interrupting running work — no portable, safe
mechanism exists; claiming it would be dishonest.

**Trade-off.** Slightly more state (`closed_` in the queue, `join_done_` in the
pool) and a lock-ordering rule that must be respected: **close before join,
never the reverse**.

**Invariant this protects.** **I3** — every pending promise is fulfilled on
every path, so a caller blocked in `future.get()` can never hang. Note the
consequence: `submit()` may block on a full queue and then return an
already-`Rejected` future when shutdown races it. That is correct.

---

## D7 — One library target, not one per module

**Status:** Accepted · **Milestone:** M1

**Decision.** A single `taskengine` library target. Module boundaries are
enforced by directory layout, include paths, and the documented dependency rule
in ARCHITECTURE.md §1.1.

**Reason.** Five CMake targets for a codebase of this size is ceremony. PLAN.md
§M1 says "library target(s)", so one target is compliant.

**Alternative considered.** Per-module `OBJECT` or `STATIC` libraries — the
linker would then mechanically enforce layering.

**Trade-off.** Layering violations are caught by review rather than by the
build. Revisit if the core grows past a few thousand lines or if a violation
actually slips through.

---

## D8 — Flat namespace `taskengine`

**Status:** Accepted · **Milestone:** M1

**Decision.** One namespace, `taskengine`. Headers live under
`include/taskengine/<module>/`.

**Reason.** The directory already conveys the module. Nested namespaces would
make every call site noisier without adding safety.

**Alternative considered.** `taskengine::core`, `taskengine::concurrency`, etc.

**Trade-off.** Name collisions across modules are possible in principle; with
this few types, unlikely in practice.

---

## D9 — No logging framework

**Status:** Accepted · **Milestone:** M6

**Decision.** Diagnostics go to `stderr` through a minimal helper. No external
logging library.

**Reason.** At this scale the standard library is adequate, and every dependency
must justify itself (I7).

**Alternative considered.** spdlog — a real dependency, plus a formatting
library, for something we can do in a few lines.

**Trade-off.** No log levels, structured output, sinks, or rotation. If the
RabbitMQ consumer at M11 needs any of those, that is the point to revisit —
with a justification, not by default.

---

## D10 — Per-worker metrics buffers, merged after shutdown

**Status:** Accepted · **Milestone:** M5

**Decision.** `ThreadPool` owns `std::vector<std::vector<Sample>> samples_`,
sized to the worker count before any thread starts and never resized. Worker *i*
appends only to `samples_[i]`. Buffers are merged only after every worker has
been joined.

**Reason.** A shared counter or shared container touched once per task is a
contention point that would distort the very worker-scaling curve M8 exists to
measure. Single-writer-by-index means there is no synchronization on the
recording path at all, and the merge happens when no thread can be running.

**Alternative considered.** Global atomic counters — simpler, but measurably
distorting at 8–16 workers, and incapable of producing per-task percentiles.
A mutex-guarded shared vector — worse on both counts.

**Trade-off.** Memory proportional to task count. `Sample` is a small
trivially-copyable aggregate (id, state, three `steady_clock` time points) and
the benchmark bounds the task count, so this is acceptable. The full
`TaskResult`, which carries a `std::string`, goes to the promise only — the
metrics path copies no strings on the success path.

---

## D11 — Sanitizers as separate build directories, ASan+UBSan together, TSan alone

**Status:** Accepted · **Milestone:** M7

**Decision.** Four build directories: `build/debug`, `build/asan`
(`-fsanitize=address,undefined -fno-sanitize-recover=all
-fno-omit-frame-pointer -O1 -g`), `build/tsan` (`-fsanitize=thread -O1 -g`), and
`build/release`. Selected by a `SANITIZER` cache option applied with
`target_compile_options` and `target_link_options` on our own targets only.

**Reason.** ASan and TSan are mutually exclusive runtimes and cannot share a
binary, so separate build trees are forced, not chosen. ASan and UBSan do
combine. `-fno-sanitize-recover=all` makes UBSan abort rather than print and
continue, so a violation actually fails a test run instead of scrolling past in
a log. Target-scoped flags keep third-party code out of our warning and
sanitizer policy.

**Alternative considered.** Global `CMAKE_CXX_FLAGS` — simpler, but it applies
our `-Werror` to FetchContent'd sources, where an upstream tag bump would break
our build for reasons unrelated to our code.

**Trade-off.** Four build trees to keep configured, and disk. Benchmarks must
run only in `build/release`: ASan costs roughly 2x and TSan 5–15x, so a
sanitized timing is not a timing. The benchmark harness refuses to run when
built with sanitizers enabled.

---

## D12 — Three benchmark workload profiles, not one

**Status:** Accepted · **Milestone:** M8

**Decision.** Benchmark across three profiles: **A** CPU-heavy
(`ComputeTask`, large K), **B** lightweight (`ComputeTask`, tiny K), **C**
blocking/I/O-like (`SleepTask`). Plus a dedicated overhead-floor measurement at
`work = 0, workers = 1`.

**Reason.** A single CPU-heavy profile only ever shows speedup, which proves
nothing interesting and teaches nothing. Profile B is where adding workers makes
throughput *worse*, because the single queue mutex becomes the bottleneck —
being able to show that curve and name its cause is the point of the exercise.
Profile C shows why worker count is decoupled from core count.

**Alternative considered.** One tunable workload with a size parameter — B and A
are indeed the same task at different K, but C is genuinely a different type,
and naming the three regimes is what makes the results interpretable.

**Trade-off.** Three times the runs and a longer results table. Also: profile C
is the main consumer of the second `Task` implementation, which ties D12 to D1's
checkpoint C2 — if the polymorphism is collapsed, profile C must survive in some
other form.

---

## D13 — No per-task cancellation

**Status:** Accepted · **Milestone:** n/a (non-goal)

**Decision.** Individual tasks cannot be cancelled. `shutdown_now()` is the only
mechanism for stopping pending work.

**Reason.** `shutdown_now()` covers the actual requirement — stop doing work
now. Cooperative cancellation requires a token threaded through every
user-authored `execute()` and roughly doubles the state machine.

**Alternative considered.** A cancellation token on `Task::execute()`, or a
`Cancelled` state reachable from `Queued` and `Running`.

**Trade-off.** A long-running task cannot be stopped early. Accepted; this is
recorded as a limitation in `README.md` rather than hidden. PLAN.md §4 should
gain "per-task cancellation" as an explicit non-goal.

---

## D14 — WSL2 Ubuntu is the first-class development and test target

**Status:** Accepted · **Milestone:** all

**Decision.** All building, testing, sanitizing, and benchmarking happens in
WSL2 Ubuntu. Windows is the host and editor environment only. The repository
lives on the **ext4 filesystem inside WSL**, not under `/mnt/...`.

**Reason.** PLAN.md makes Linux compatibility a first-class requirement, and
Milestone 9 (`ps`, `vmstat`, signal handling, graceful termination) has no
meaningful Windows equivalent. The `/mnt` 9p bridge is substantially slower for
git and CMake and reports incorrect file modes to Linux tools, breaking
executable bits.

**Alternative considered.** Dual-platform portability (MSVC + GCC) — real
complexity, particularly around signals and sanitizer availability, for a
benefit PLAN.md never asks for.

**Trade-off.** WSL2 is a virtual machine, so benchmark figures are valid for
*relative* comparison across worker counts on the same host but are not
bare-metal Linux numbers. Every result set is labelled `WSL2` for this reason
(ARCHITECTURE.md §8.5).

---

## D15 — GoogleTest via `FetchContent`, pinned to a commit

**Status:** Accepted · **Milestone:** M1

**Decision.** GoogleTest, fetched at a pinned tag and built from source. The
first and, for the C++ core, only dependency.

**Reason.** C++17 has no test framework, and CTest alone provides no assertions
or fixtures. Building from source matters beyond convenience: **TSan only
observes races in instrumented code**, so a prebuilt system `libgtest` would
hide races in test infrastructure and produce partial stacks.

**Alternative considered.** Catch2 — equally defensible, header-only.
`find_package(GTest)` — loses the TSan instrumentation argument above.
Hand-rolled assertion macros — no dependency, but poor diagnostics and no
fixtures or death tests; a false economy.

**Trade-off.** Configure-time network fetch, and the pin must be updated
deliberately.

**Implementation note (M1, Task 1.2).** Pinned to the immutable commit
`52eb8108c5bdec04579160ae17225d66034bd723` (GoogleTest v1.17.0) rather than to
the tag name, because a tag can be repointed upstream and a commit cannot. This
is stricter than the decision as originally written, not a departure from it.
v1.17.0 was chosen over the newest release (v1.18.0) after reading both
projects' stated C++ requirements: both require C++17, and one release of field
time is worth more here than being current. `BUILD_GMOCK` is OFF — gmock is
unused, and skipping it removes roughly half the dependency's build time. The
dependency is declared in `tests/CMakeLists.txt`, not at the top level, so
nothing outside the test tree can acquire it by accident.


---

## D16 — Release is pinned to `-O2`, not CMake's default `-O3`

**Status:** Accepted · **Milestone:** M1

**Decision.** `set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")`. Release is the
benchmark baseline (ARCHITECTURE.md §8.5) and states its own optimisation level
rather than inheriting whatever CMake's default happens to be.

**Reason.** A benchmark configuration that depends on a toolchain default is not
reproducible: the same source measured under a different CMake could silently
change optimisation level. `-O2` is also the more representative production
setting, and `-O3`'s extra vectorisation and unrolling would be measuring the
compiler more than the engine.

**Alternative considered.** Leave CMake's `-O3` default — one line shorter, but
unstated and therefore not a baseline. A target-scoped
`$<$<CONFIG:Release>:-O2>` generator expression — leaves both `-O3` and `-O2` on
the command line and relies on GCC's last-flag-wins rule; correct, but not
something worth defending.

**Trade-off.** This is a build-type variable, so unlike the warning policy it
does apply to third-party code. That is deliberate and stated in the build file:
an optimisation level is correct to apply to every object in a Release build;
`-Werror` is not. `-DNDEBUG` is retained explicitly, since dropping it would
silently re-enable `assert` in Release.

---

## D17 — CMake minimum is 3.22, for a correctness reason

**Status:** Accepted · **Milestone:** M1

**Decision.** `cmake_minimum_required(VERSION 3.22)`.

**Reason.** Not a guess at a reasonable floor. Below 3.22, policy **CMP0128** is
OLD, and CMake omits the `-std` flag entirely whenever the compiler's default
standard already satisfies the requested one. GCC 13 defaults to `gnu++17`,
which satisfies `cxx_std_17` — so `CMAKE_CXX_EXTENSIONS OFF` was silently
ignored and the build compiled as `gnu++17` while the build file claimed strict
C++17. This was found by checking `__STRICT_ANSI__` in the real compile flags,
not by reading the CMake source. 3.22 is also the CMake in Ubuntu 22.04 LTS, so
the floor still covers the last two LTS releases.

**Alternative considered.** Keep 3.16 and append `-std=c++17` manually to
`target_compile_options` — works, but bypasses CMake's standard mechanism and
would silently diverge from `target_compile_features`. `cmake_policy(SET
CMP0128 NEW)` with the lower minimum — equivalent, but an unexplained policy
line is harder to read than a version floor with a comment.

**Trade-off.** Drops support for CMake older than 3.22. Nothing in the target
environment (D14) is affected.

**Verification.** `-std=c++17` present in both compile databases,
`__STRICT_ANSI__` defined, `__cplusplus == 201703L`.

---

## D18 — `version()` is defined out-of-line, deliberately

**Status:** Accepted · **Milestone:** M1

**Decision.** `taskengine::version()` is declared in
`include/taskengine/version.hpp` and defined in `src/version.cpp`, rather than
being an `inline constexpr` constant in the header.

**Reason.** It is the one symbol that makes "the executable actually links the
library" a provable claim. An inline constant would be resolved entirely in the
header, letting the linker discard `libtaskengine.a` while every test still
passed — the build foundation would prove nothing. The out-of-line definition is
verified negatively: linking `main.cpp.o` without the archive fails with
`undefined reference to taskengine::version()`.

**Alternative considered.** `inline constexpr std::string_view kVersion` in the
header — fewer moving parts, no translation unit, but unverifiable.

**Trade-off.** One function call that cannot be inlined across the archive
boundary. Irrelevant: it is called once, at startup. The version string itself
comes from `project(VERSION ...)` via a compile definition with no fallback, so
a misconfigured build fails to compile rather than reporting a wrong version.
