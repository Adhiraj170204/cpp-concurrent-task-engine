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

---

## D19 — Core result model makes invalid states unrepresentable

**Status:** Accepted · **Milestone:** M2

**Decision.** `TaskResult` has no public constructor and no default
constructor. It is built only through three named factories — `succeeded`,
`failed` and `rejected` — and `rejected` takes two time points rather than a
whole `TaskTimings`.

**Reason.** The engine can only ever produce three shapes of result, and a
plain aggregate would also permit combinations it never produces: a `Succeeded`
result carrying an error message, or a default-constructed result describing a
task that never existed. Pushing that into the type means the invariant is
enforced once, at construction, instead of being re-checked by every consumer.
The asymmetric `rejected` signature exists for the same reason: a rejected task
never reached a worker, so it has no dequeue instant, and the type should not
let a caller describe an execution window that never happened. `rejected` sets
`dequeued` equal to `finished`, so `execution_time()` is zero rather than a
meaningless value measured from a default-constructed time point.

**Alternative considered.** A plain aggregate `struct TaskResult` — shorter, and
brace-initialisable at the call site, but it admits states the engine cannot
produce and gives every reader of a result a reason to wonder whether the error
field is meaningful. A `std::variant` of three result types — precise, but it
forces `std::visit` on every consumer for no gain, since all three share the
same fields.

**Trade-off.** Three factory names to learn, and one more line per construction
site than aggregate initialisation. `TaskResult` is not default-constructible,
which would matter if it ever had to live in a container that default-fills;
`std::promise<TaskResult>` does not require it.

**Related.** `TaskId` is a plain `std::uint64_t` alias, not a strong type. It is
an opaque counter used only for identity and diagnostics, and no other integer
appears in these interfaces that could be confused with it; a strong type would
have to carry its own comparison, hashing and streaming support to pay for
itself. Revisit if an interface ever takes both an id and a count.

---

## D20 — `Task` suppresses public copy and move, and anchors its own vtable

**Status:** Accepted · **Milestone:** M2

**Decision.** `Task` declares all five special members. The destructor is
virtual, declared in the header and **defined out of line** in
`src/core/task.cpp`. The copy and move constructors and assignment operators are
`protected` and defaulted.

**Reason.** Two separate concerns, both standard practice for a polymorphic
base.

*Protected copy and move.* Being abstract already prevents constructing a `Task`
by value, so the usual slicing-on-copy story does not apply. The real exposure
is **assignment through a reference**: with a public `operator=`, code holding
two `Task&` could assign one over the other and partially overwrite the derived
object. Making the operators protected makes that a compile error while leaving
derived types free to copy and move themselves, which is exactly the desired
split. This is asserted in the tests rather than left as a comment:
`!std::is_copy_assignable_v<Task>` and `!std::is_move_assignable_v<Task>`.

*Out-of-line destructor.* A vtable and `type_info` are emitted alongside a
class's first non-inline, non-pure virtual member — the "key function". With
every virtual member inline, the compiler emits a copy of the vtable in every
translation unit that includes the header and relies on the linker to merge
them. Defining `~Task()` in one source file pins them to one object file.

**Alternative considered.** `= default` in the header for the destructor —
simpler to read, but emits the vtable everywhere. Deleting copy and move
outright — also prevents derived types from copying themselves, which is a
bigger restriction than the problem warrants. Saying nothing and relying on the
implicit declarations — leaves a public copy assignment on a polymorphic base,
which is the defect being avoided.

**Trade-off.** Five extra lines in the header and one source file that contains
a single defaulted function. In exchange the class states its copy semantics
explicitly instead of inheriting them by accident, which is the point.

---

## D21 — `Sample` deferred from M2 to M5

**Status:** Accepted · **Milestone:** M2 (deferring to M5)

**Decision.** `Sample` is not implemented at M2, despite ARCHITECTURE.md §1.2
listing it among the types `core/` owns at that milestone. It arrives with the
metrics collection that consumes it.

**Reason.** A genuine tension between two parts of the approved architecture.
§1.2 assigns `Sample` to `core/` at M2; §0.1 rule 2 forbids creating a type
"merely because it appears in this document" and requires it to arrive in the
milestone that needs it. §0.1 is a *standing constraint* that explicitly governs
every task in every milestone, so it wins. `Sample` has no consumer until the
thread pool records timings at M5: written now, it would be an untested
structure with no caller — exactly the unexplainable code §0.1 exists to
prevent.

The M2 deliverable in PLAN.md is "task abstraction, result/error model,
lifecycle tests", and `Sample` is a metrics concern rather than part of the
result model. `TaskTimings` — the part the result model genuinely needs — **is**
implemented at M2 and tested.

**Alternative considered.** Implement `Sample` now to match the §1.2 milestone
column literally. Rejected: it would satisfy a table at the cost of the
principle the table sits beneath.

**Trade-off.** §1.2's milestone column is now slightly ahead of reality for one
type. The column is guidance about where a type belongs, not a schedule with
force of its own; no code depends on it.

**Action at M5.** `Sample` lands with the metrics buffers, as the compact
trivially-copyable twin of `TaskResult` described in §4.3.

---

## D22 — Sanitizer wiring lands at M3, and sanitizer flags are global where warning flags are not

**Status:** Accepted · **Milestone:** M3 (ARCHITECTURE.md §9.2 nominally M7)

**Decision.** The `TASKENGINE_SANITIZER` cache option (`off` |
`address+undefined` | `thread`) is added at M3 rather than M7. Its flags are
applied with `add_compile_options` / `add_link_options` at directory scope, so
they reach GoogleTest as well. The warning policy remains target-scoped and
`PRIVATE`.

**Reason — two separate points.**

*Why at M3.* ARCHITECTURE.md §9.4 states that TSan is "the real verification
mechanism for Milestones 3 and 4". M3 cannot honestly be called complete without
it, and a concurrency test suite that has never been under TSan proves very
little. The alternative — passing sanitizer flags ad hoc on the command line —
works but is not reproducible: a reviewer would have to already know the flag
set, which defeats the purpose of writing it down.

*Why global.* §9.2 says sanitizer flags should be applied "on our own targets
only — never through global `CMAKE_CXX_FLAGS`". Taken literally that is wrong
for sanitizers, and it contradicts §9.3 in the same document, which builds
GoogleTest from source *precisely so that it can be instrumented*. A sanitizer
only reasons about instrumented code: mixing instrumented and uninstrumented
objects causes TSan both to miss real races behind uninstrumented frames and to
report false ones, because it cannot see the happens-before edges established
inside the uninstrumented code. §9.2's rule is really about `-Werror`, where the
concern is an upstream change breaking our build. That concern does not transfer
to `-fsanitize`.

Verified rather than assumed: `-fsanitize=thread` is present in both our own
targets' flags and GoogleTest's.

**Alternative considered.** Target-scoped sanitizer flags matching §9.2
literally — leaves GoogleTest uninstrumented and makes TSan results
untrustworthy. Deferring all sanitizer work to M7 — leaves M3 and M4, the two
riskiest milestones, validated only by tests that prove one interleaving worked.

**Trade-off.** §9.2 is now more precise than it was: warning policy
target-scoped, sanitizer policy directory-scoped. Sanitizer builds need their own
build directory, which §9.1 already required since ASan and TSan cannot share a
binary. M7 is correspondingly smaller: it becomes stress coverage and documented
routine runs rather than build plumbing.

**Environment note.** On this kernel (WSL2, 6.6) TSan aborts at startup with
`FATAL: ThreadSanitizer: unexpected memory mapping`. This is the well-known
conflict between TSan's fixed shadow-memory layout and the ASLR entropy modern
kernels use (`vm.mmap_rnd_bits = 32`). The usual fix lowers that sysctl and
needs root. `setarch "$(uname -m)" -R` disables randomisation for one process
tree instead, needs no privileges, and is what the README documents. It is
required for the *build* as well as the test run, because
`gtest_discover_tests` executes the test binary at build time to enumerate
cases.

---

## D23 — BlockingQueue interface: rvalue push, optional pop, draining close

**Status:** Accepted · **Milestone:** M3

**Decision.** Three interface choices, all of which encode a guarantee in the
signature rather than in documentation.

- `bool push(T&& value)` — an rvalue reference, not a by-value sink parameter.
- `std::optional<T> pop()` — no separate "is the queue finished" query.
- `close()` stops intake but delivers everything already queued.

**Reason.**

*`T&&`.* A by-value parameter would silently accept an lvalue and copy it. An
rvalue reference makes the caller write `std::move`, so a copy cannot happen by
accident and the no-copy property is enforced by the compiler rather than by a
comment. The queue will carry move-only envelopes, where this matters.

*`std::optional<T>`.* The alternative shapes all have a race built in. A
`bool pop(T& out)` needs `T` to be default-constructible, which the envelope is
not. A separate `is_closed()` check before `pop()` is a check-then-act: the
queue can close between the two calls. Returning `optional` makes "took an item"
and "the queue is finished" a single atomic answer. `pop()` emplaces into the
optional rather than assigning, so `T` need not be default-constructible at all.

*Draining close.* If `close()` discarded queued items, a consumer could not
distinguish "finished" from "gave up", and the drain-shutdown mode the thread
pool needs would have to be built somewhere else. Because `pop()` returns
`nullopt` only when the queue is both closed **and** empty, a consumer that
loops until `nullopt` has provably seen every item whose `push` returned true.
The abort-shutdown mode that *does* discard queued work is a thread-pool
concern and arrives with it at M4.

**Alternative considered.** `bool try_push` / `bool try_pop` non-blocking
variants — nothing needs them yet, and adding them now would be speculative.
An `emplace`-style variadic push — saves one move, at the cost of a forwarding
interface that is harder to read; the move is not on any measured hot path.

**Trade-off.** Callers must write `std::move` at every push site, which is
slightly noisier and is the point. `size()` is exposed but is a momentary
snapshot, useless for control flow and documented as such; it exists for
invariant checks such as "never above capacity" and for queue-depth reporting
during benchmarking.

---

## D24 — `TaskEnvelope` lives in `core/`, not `execution/`

**Status:** Accepted · **Milestone:** M4 (ARCHITECTURE.md §1.2 placed it in `execution/` at M5)

**Decision.** `TaskEnvelope` is declared in `include/taskengine/core/` and is
part of the `core/` module. ARCHITECTURE.md §1.2 is corrected accordingly.

**Reason.** A genuine contradiction inside the approved architecture, not a
preference. §1.1 fixes the dependency direction as `execution/ -> concurrency/
-> core/`, and states that nothing below `app/` may depend upward. §2.1 shows
`ThreadPool`, which lives in `concurrency/`, owning a
`BlockingQueue<TaskEnvelope>`. Those two cannot both hold while `TaskEnvelope`
belongs to `execution/`: the pool would depend on the layer above it and the
graph would cease to be acyclic.

`core/` is the correct home on the merits, not just to break the tie.
`TaskEnvelope` is pure data and ownership — an id, a submission instant, a
`unique_ptr<Task>` and a `promise<TaskResult>` — with no concurrency primitive,
no scheduling policy and no knowledge of a queue or a pool. It is exactly the
kind of vocabulary type §1.2 says `core/` exists to hold. `std::promise` is a
standard-library type in the same sense `std::string` is; it is not a policy.

**Alternative considered.** Make `ThreadPool` a template over the job type, so
that `concurrency/` never names `TaskEnvelope` and the envelope can stay in
`execution/`. That preserves §1.2 exactly and is defensible, but it pushes the
worker loop and the exception boundary — the most delicate code in the project —
into a template, and it moves the run-and-fulfil logic into the job type anyway.
Rejected as more machinery than the problem justifies.

Leaving `TaskEnvelope` in `execution/` and having `execution/` own the queue,
with `ThreadPool` reduced to bare threads, was also considered. That contradicts
§2.1 instead of §1.2 and makes the pool too thin to be worth naming.

**Trade-off.** One row of §1.2 changes. `core/` now includes `<future>`, which
is a slightly heavier standard header than the rest of that module pulls in. No
dependency edge is added and the graph stays acyclic.

---

## D25 — The pool rejects what it refuses, rather than dropping it

**Status:** Accepted · **Milestone:** M4

**Decision.** `ThreadPool::submit` takes the envelope **by value**. When the
queue refuses it, `submit` fulfils that envelope as `Rejected` before returning
`false`. `shutdown_now` does the same for every envelope it drains. `run()` and
`reject()` are `noexcept`.

**Reason.** Invariant I3 says every future handed out is fulfilled exactly once.
The tempting shortcut is to let a refused envelope fall out of scope: the
standard then breaks the promise, and the caller gets
`future_error(broken_promise)` from `get()` rather than hanging. That is not
good enough. A broken promise is an exception the caller has to handle
separately, it carries no id and no timings, and it is indistinguishable from an
engine bug. Rejecting explicitly gives the caller the same shaped answer as
every other outcome — a `TaskResult` in a terminal state — and keeps
"was it accepted" and "what happened to it" consistent.

This relies on `BlockingQueue::push` leaving its argument untouched when it
refuses, which is documented on `push` and covered by a test.

`noexcept` on `run()` is a statement about where it executes. It runs directly
on a worker thread, where an escaping exception calls `std::terminate`, so the
task boundary has to absorb everything — including throws that are not derived
from `std::exception`, which become a `Failed` result reading
`"unknown exception"` rather than a lost failure. What `noexcept` deliberately
does **not** absorb is a double fulfilment, which would be an engine bug:
terminating there is the right answer, because it means the pool has lost track
of an envelope and some other caller is about to wait forever.

**Alternative considered.** Returning the refused envelope to the caller
(`std::optional<TaskEnvelope> submit(...)`) so the caller decides. More flexible,
but it makes every call site responsible for the invariant, and a caller that
ignores the returned envelope silently breaks a promise. Keeping the obligation
with the code that owns it is the safer default.

Using `promise::set_exception` for refusal — rejected for the same reasons as
D4: a refusal is a countable outcome, not a control-flow event.

**Trade-off.** `submit` has a side effect on failure, which has to be understood
to be understood at all. It is stated on the declaration and is the subject of a
test that asserts the returned future resolves to `Rejected` rather than
throwing.

---

## D26 — Member declaration order in `ThreadPool` is a correctness requirement

**Status:** Accepted · **Milestone:** M4

**Decision.** `ThreadPool` declares `queue_` first and `threads_` last, and the
header says why. The destructor calls `shutdown()`. `shutdown()` closes the
queue and only then joins; `join_workers()` is guarded by its own mutex and a
`joined_` flag.

**Reason.** Members are destroyed in reverse declaration order, so `threads_`
is destroyed before `queue_`. Workers hold a reference to the queue for their
whole lifetime, so the queue has to outlive them. Swapping the two declarations
would compile, pass a casual reading, and introduce a use-after-free that only
appears during destruction — the hardest kind of bug to find by testing, since
it depends on the allocator reusing freed memory.

Close-before-join is the second half of the same ordering argument. Joining
first would wait on workers that are still blocked in `pop()` for work that is
never coming: a deadlock, not a slow shutdown. The rule is stated on
`shutdown_mutex_` so a future edit has to notice it.

`join_workers()` is guarded because `std::thread::join` on a thread that another
thread is already joining is undefined behaviour, and both shutdown modes are
documented as callable from any thread. `joined_` is set only after every join
has returned, so a second caller cannot observe it as true while a thread is
still being joined.

The constructor also has to clean up after itself: if thread creation throws
part way through, no destructor runs for the half-built pool, so the
already-started workers would outlive it. The catch block closes the queue and
joins them before rethrowing.

**Alternative considered.** A comment saying "do not reorder" without the
reasoning. It survives exactly as long as the person who wrote it.

**Trade-off.** None worth the name; this is ordinary RAII discipline written
down. The cost is a header comment longer than the declarations it guards.

**Not supported.** Calling `submit`, `shutdown` or `shutdown_now` from inside a
task running on the same pool. `submit` can deadlock against a full queue only
that worker could drain, and either shutdown would have the worker join itself.
Documented on the class, in the same family as the recursive-submit hazard in
D5.

---

## D27 — Checkpoint C1 resolved: `TaskEngine` is kept

**Status:** Accepted · **Milestone:** M5 · **Resolves:** checkpoint C1 (ARCHITECTURE.md §0.3)

**The question C1 asked.** If `TaskEngine` turns out to be only pass-through
wiring over `ThreadPool` — id assignment and nothing more — merge it, and do not
keep it for the shape of the diagram.

**The measurement.** 21 non-comment lines of implementation. Of its seven public
members, four (`shutdown`, `shutdown_now`, `worker_count`, `is_shut_down`)
forward straight to the pool and nothing else. That is genuinely thin, and
pretending otherwise would be the exact failure C1 exists to catch.

**Decision.** Keep it, on the strength of two things that are not forwarding and
are not the pool's business.

*Identity.* `next_id_` is state the pool does not have and should not acquire.
The pool moves envelopes; it has no opinion about what a task is called.

*The notion of a run.* `summary()` combines per-worker sample buffers with the
refusal count and aggregates them. The pool records raw data; it has no concept
of a run having happened.

The clinching evidence is in the test files rather than the headers. The pool
suite builds `TaskEnvelope`s by hand and never once mentions an id or a summary;
the engine suite submits `unique_ptr<Task>` and never mentions an envelope.
Those are two different interfaces at two different levels, and each is testable
without the other. `submit(TaskEnvelope) -> bool` and
`submit(unique_ptr<Task>) -> future<TaskResult>` are not the same function with
a wrapper around it.

**Alternative considered.** Merge, saving roughly sixty lines and one
indirection. The cost is that `ThreadPool` becomes simultaneously a reusable
concurrency primitive and the application-facing API, its test suite mixes
envelope-level with task-level concerns, and the HTTP and broker layers at M10
and M11 end up depending on a class called `ThreadPool`. That is a worse
repository for a smaller one.

**Trade-off.** Four forwarding methods, honestly. If M6 and the CLI do not give
`TaskEngine` a third real responsibility, this is worth asking again rather than
treating as settled.

---

## D28 — Checkpoint C2 resolved: the `Task` hierarchy is kept

**Status:** Accepted · **Milestone:** M5 · **Resolves:** checkpoint C2 (ARCHITECTURE.md §0.3)

> **Update (D30).** The caveat below — that the runtime-dispatch justification
> was still owed at M11 — is settled. That milestone no longer exists, so C2 was
> re-decided on the remaining evidence rather than deferred again, and is now
> closed. The verdict is unchanged: the hierarchy is kept. See D30.

**The question C2 asked.** If the concrete task types differ only by a parameter
**and** nothing dispatches on type at runtime, collapse the hierarchy to
`std::function<void()>` and delete it.

**The measurement.** Two conditions, and they do not agree.

*Do they differ only by a parameter?* No. `ComputeTask` burns a core;
`SleepTask` occupies a worker while consuming no core at all. They respond to
worker count in opposite ways, which is the entire reason the benchmark plan
needs both: one shows speedup flattening at the core count, the other shows
throughput scaling far past it. Collapsing them into one type with a flag would
be worse code, not less code.

*Does anything dispatch on type at runtime?* **No.** Verified rather than
assumed: there is no `dynamic_cast` and no `typeid` anywhere in `include/` or
`src/`. Today, `std::function<void()>` would in fact suffice for everything the
engine does.

**Decision.** Keep it, because C2 requires *both* conditions to collapse and
only one holds. But the honest position is that the strongest argument for
polymorphism — a factory at M11 choosing a concrete type from a broker message
payload — has not arrived, and until it does the hierarchy is carrying its
weight on the first condition alone.

**Re-examine at M11.** If the broker layer does not produce a genuine runtime
type selection, this should be revisited properly rather than allowed to lapse
by default. A virtual call per task on a hierarchy nothing dispatches over is an
abstraction waiting to be deleted.

**Trade-off.** One heap allocation and one virtual call per task, which the
overhead-floor measurement at M8 will put a number on instead of leaving it as
an assertion.

---

## D29 — Metrics are a pure function, and refuse to answer early

**Status:** Accepted · **Milestone:** M5

**Decision.** `summarize()` is a free function over a `std::vector<Sample>` and
a refusal count. It touches no clock, no thread and no shared state.
`ThreadPool::collect_samples()` throws `std::logic_error` if the workers have
not been joined, and `TaskEngine::summary()` inherits that. Percentiles use
nearest rank, computed in integer arithmetic, with the index function exposed so
it can be tested directly.

**Reason.** Three separate points.

*Pure function.* It makes percentile arithmetic testable against hand-computed
answers with no threads in sight — the empty sample, one sample, two samples,
and an unsorted input all get direct tests. It also makes the measurement code
structurally incapable of perturbing what it measures.

*Refusing early.* Reading the per-worker buffers while workers are still
appending is a data race. The alternative to throwing is returning whatever
happens to be there, which would look like an answer and would not be one. A
summary of an unfinished run is precisely the kind of "partial success presented
as success" the project forbids.

*Nearest rank in integers.* "p95" means different things in different tools, and
a benchmark number nobody can reproduce is worth nothing. The definition is
written down, the arithmetic is exact, and `nearest_rank_index` is public so the
hand-worked ranks are checked rather than trusted.

**Alternative considered.** Linear interpolation between neighbouring ranks,
which most statistics packages default to. It produces values that never
occurred in the sample, which is misleading for latency: a reported p95 should
be a latency some task actually experienced.

Returning an empty summary instead of throwing before shutdown — rejected under
the same rule as above.

**Trade-off.** `summarize` sorts a copy of the durations, so an N-task run costs
three sorts and about 3N durations of scratch memory at summary time. That is
off the hot path by construction: it runs once, after the workers have stopped.

**Related.** `Sample` lands here rather than at M2, as D21 scheduled, now that
there is a consumer for it. Concrete task types live in a new `tasks/` module:
they are neither vocabulary (`core/`) nor mechanism (`concurrency/`) nor policy
(`execution/`), they are workloads, and the CLI and the benchmark driver reach
for them independently.

---

## D30 — Scope reduced to the C++ engine; service integrations removed from the roadmap

**Status:** Accepted · **Milestone:** M5/M6 boundary · **Supersedes forward references in:** D1, D3, D5, D9, D27, D28

**Decision.** The roadmap ends at Milestone 8. The final scope is: task
execution behaviour (M5, complete), CLI (M6), testing and sanitizer hardening
(M7), benchmarking (M8).

Removed from the roadmap entirely: FastAPI service, RabbitMQ adapter, MongoDB,
AWS, Kubernetes, Terraform, distributed scheduling, per-task cancellation, and
further external integrations. Folded into the remaining milestones rather than
dropped: Linux operational observation and failure-mode hardening into M7,
repository and documentation polish into M8.

**Reason.** Those integrations are already demonstrated elsewhere in the
portfolio, so building them again here would add breadth that is already
covered while diluting the one thing this repository is for: C++17 ownership,
lifetime and concurrency correctness that the author can defend line by line.

The interview-defensibility argument runs the same way. A repository containing
a broker adapter and an HTTP service invites questions about all of them, and
every additional surface is another thing to have to explain under pressure. A
smaller repository where every file has a reason to exist is a stronger artefact
than a larger one where some of it is scaffolding. The project already applies
that test file by file; this applies it to the roadmap.

There is also a truthfulness point. A half-finished RabbitMQ adapter on a
resume-facing project is worse than none: it claims familiarity the code would
not survive being asked about.

**Alternative considered.** Keep the integrations as optional stretch
milestones after M8. Rejected because "optional later" is how scaffolding gets
written and never finished, and because an unfinished integration sitting in the
tree is exactly the unexplainable code the project set out to avoid. Removing
them from the roadmap is a decision; leaving them as maybe is not.

**Trade-off.** The end-to-end flow this project originally described is not
built. The engine was always specified to work standalone, so nothing already
written depends on it, and the boundary that would have carried it is still
clean: nothing below `app/` knows about HTTP, brokers or containers, and the
dependency graph is unchanged.

### Forward references this invalidates

Several earlier decisions justified themselves partly by pointing at milestones
that no longer exist. Those entries are left as written, because a decision log
records what was believed at the time; this section records what is no longer
true.

- **D28 and checkpoint C2 (`Task` polymorphism).** The outstanding
  justification was a factory at M11 selecting a concrete task type from a
  broker payload. That milestone is gone, so **the justification will never
  arrive** and C2 cannot be deferred again. Re-deciding it now on the evidence
  that remains: the two concrete types differ in kind rather than by a
  parameter, which is the condition C2 required, and that condition stands on
  its own. `ComputeTask` saturates a core while `SleepTask` occupies a worker
  without one; they respond to worker count in opposite directions, and the M8
  benchmark needs both. **Verdict: the hierarchy is kept, and C2 is closed
  rather than reopened.** The cost — one allocation and one virtual call per
  task — gets a measured number at M8 instead of an assertion.
- **D1** cited the same M11 factory. Superseded by the above.
- **D3** cited a FastAPI status endpoint as a secondary benefit of returning
  `std::future`. The primary reasons are unaffected: it is standard library, it
  needs no dependency, and it gives one place to enforce the exactly-once
  fulfilment invariant.
- **D5** described backpressure as the honest behaviour for something later fed
  by a broker. The argument does not depend on the broker: an unbounded queue
  still converts a rate mismatch into unbounded memory growth, and bounding it
  is still what makes a queue-wait measurement mean anything.
- **D9** deferred the question of a logging framework to a RabbitMQ consumer
  that will not be built. A minimal stderr diagnostic is now the final answer,
  not a provisional one.
- **D27** cited future HTTP and broker layers as a reason to keep `TaskEngine`
  separate from `ThreadPool`. That reason is gone; the others are not. The two
  suites still exercise the two levels independently, and the CLI at M6 is the
  remaining consumer of the separation. C1 is worth one honest look at the end
  of M6, as D27 already said.
- **JSON dependency**, listed in ARCHITECTURE.md §12 for M11 only, is removed.
  The project ends with exactly one dependency, GoogleTest, in test builds only.

### Not covered by this decision

Container packaging was not named in the scope reduction. It was raised as the
one item worth confirming, and has since been approved as a single multi-stage
Dockerfile at M8 with no orchestration. See D31.

---

## D31 — One multi-stage Dockerfile at M8, and nothing else container-shaped

**Status:** Accepted · **Milestone:** M8

**Decision.** A single multi-stage `Dockerfile` at the repository root, building
and running the `task-engine` CLI. Pinned base image, build tooling confined to
the build stage, non-root runtime user, and a runtime stage containing the
binary and its runtime libraries and nothing else.

Explicitly not included: Docker Compose, an entrypoint script wrapping a
service, a registry or publishing step, and any of the integrations D30 removed.
Compose exists to orchestrate several services; there is exactly one process
here, so it would be ceremony describing a topology that does not exist.

**Reason.** This is the one piece of the deferred deployment work that is about
**build reproducibility** rather than about adding a service surface. It answers
a question the README currently cannot: how does someone who is not on WSL2
Ubuntu 24.04 with GCC 13 build and run this. Right now the answer is a list of
prerequisites and some hope; a pinned image makes it exact.

It also stays inside the project's own rule that every file has to earn its
place. A Dockerfile that builds the thing the repository is about is
explainable layer by layer, which is the standard the earlier plan set for it.
An orchestration file for a single process is not.

**Alternative considered.** No container at all, which was the position after
D30. Rejected on the reproducibility point above: the cost is one file and the
benefit is that the build instructions become verifiable rather than
aspirational.

A full deployment story — compose, health checks, a registry, an orchestrator —
rejected under D30 for the same reasons as the rest of it.

**Trade-off.** The image is a second build path to keep working, so it has to be
built as part of M8 rather than written and left to rot. It also adds a second
place where the toolchain version is pinned, which has to agree with what the
README claims.

**Measurement note.** Benchmark numbers must not be taken from inside the
container. Development already happens in a VM (D14, ARCHITECTURE.md §8.5 item
4), and measuring inside a container on top of that puts two layers between the
number and the hardware. The container is for reproducing the *build* and
running the program; §8 numbers come from a Release build on the host.
