# Architecture — Concurrent Task Processing Engine

**Status:** approved (Milestone 0, Task 0.1). No implementation exists yet.

**Audience:** the maintainer, and a reviewer reading this repository cold.

This document is the *target* architecture. It describes where the system is
going, so that each milestone is a small and obvious step. It is not a licence
to build any of it ahead of schedule — see [§0](#0-standing-constraints).

Every design decision referenced below as `Dn` is recorded with its reasoning,
the alternative considered, and its trade-off in [DECISIONS.md](DECISIONS.md).

---

## 0. Standing constraints

These govern every task in every milestone.

### 0.1 Incrementalism

1. This document is the target architecture. **Future milestones are not
   implemented prematurely.**
2. **No placeholder abstractions, files, dependencies, or interfaces** are
   created merely because they appear in this document. A module listed here
   comes into existence in the milestone that needs it, and not before. An
   empty directory or a stub header is not "preparation"; it is unexplainable
   code.
3. Every task ends at its own boundary. Scope discovered mid-task is reported,
   not absorbed.

### 0.2 Non-negotiable invariants

These are correctness requirements, not preferences. A change that violates one
is a defect regardless of whether the tests pass.

| # | Invariant | Enforced by |
|---|---|---|
| I1 | **No raw owning pointers.** Raw pointers and references are non-owning parameters only. | review; [§2](#2-ownership-model) |
| I2 | **`TaskEnvelope` is move-only.** | move-only members; rule of zero |
| I3 | **Every future is fulfilled exactly once**, on every path. | [§5.4](#54-the-core-invariant); dedicated test group |
| I4 | **No thread-per-task.** Worker threads are created once, in the pool constructor. | [§3.1](#31-threads-in-the-system) |
| I5 | **No polling in production concurrency code.** Blocking waits use condition variables with predicates. | [§3.3](#33-blocking-and-wakeups) |
| I6 | **No silent fallback.** A partial success is never reported as success. | [§6](#6-error-propagation); exit codes |
| I7 | **No unnecessary dependencies.** Each one is justified in DECISIONS.md before it is added. | [§12](#12-dependencies) |

### 0.3 Planned checkpoints

Points where the design has committed to re-examining itself rather than
defending a shape that stopped earning its place.

- **C1 — `TaskEngine` (end of M5). RESOLVED: kept.** 21 lines of
  implementation, four of seven public members pure forwarding — but id
  assignment and run-level aggregation are genuinely not the pool's business,
  and the two test suites exercise the two levels independently. See D27.
  Worth asking again at M6 if the CLI gives it no third responsibility.
- **C2 — `Task` polymorphism (end of M5). CLOSED: kept.** The two types differ in kind, not by a parameter:
  one burns a core, the other occupies a worker without one. But nothing
  dispatches on type at runtime — verified, no `dynamic_cast` or `typeid`
  anywhere. C2 requires both conditions to collapse and only one holds.
  **Closed by D30**, not deferred: the milestone that would have supplied
  runtime type selection no longer exists, so the checkpoint was re-decided on
  the evidence that remains rather than postponed again. See D28 and D30.

---

## 1. Module dependency graph

```
                              +------------------------------+
                              |           app/               |   composition root
                              |  main(), run(Options)        |   owns TaskEngine
                              +---+----------------------+---+
                                  |                      |
                    +-------------v------+      +--------v---------+
                    |       cli/         |      |   execution/     |
                    |  Options           |      |   TaskEngine     |
                    |  parse_args()      |      |   TaskEnvelope   |
                    +---------+----------+      +--+------------+--+
                              |                    |            |
                              |        +-----------v--+   +-----v--------+
                              |        | concurrency/ |   |   metrics/   |
                              |        | BlockingQueue|   | summarize()  |
                              |        | ThreadPool   |   | RunSummary   |
                              |        +-------+------+   +-----+--------+
                              |                |                |
                          +---v----------------v----------------v---+
                          |                 core/                   |
                          |  Task (abstract)   TaskId   TaskState   |
                          |  TaskResult   TaskError   Sample        |
                          +--------------------+--------------------+
                                               |
                                    +----------v----------+
                                    |  C++17 standard lib |
                                    +---------------------+

   Nothing sits above app/. Service, messaging and deployment layers were
   removed from the roadmap by D30.
```

### 1.1 Invariants of the graph

- The graph is **acyclic**, and arrows point downward only.
- `core/` includes nothing but the standard library.
- `concurrency/` and `metrics/` are siblings that **do not know about each
  other**. The pool *collects* `Sample`s; `metrics/` *aggregates* them; the two
  never meet.
- Nothing below `app/` includes anything from `cli/`. The core remains
  buildable, testable and benchmarkable with no HTTP, AMQP or container
  concepts anywhere in its dependency closure — a property that is now trivially
  satisfied, since D30 removed those layers from the roadmap, but which is still
  what keeps the core testable in isolation.

### 1.2 Module responsibilities

| Module | Reason to exist | Owns | Milestone |
|---|---|---|---|
| `core/` | The vocabulary every other layer speaks. Pure data plus one abstract interface, zero concurrency — so the task model is unit-testable without threads. | `Task`, `TaskId`, `TaskState`, `TaskResult`, `TaskError`, `TaskTimings`, `TaskEnvelope`, `Sample` | M2, M4 |
| `concurrency/` | The two reusable synchronization primitives, testable in isolation. | `BlockingQueue<T>`, `ThreadPool` | M3, M4 |
| `execution/` | Policy: assigns identity, stamps submission, owns pool lifetime, aggregates results. | `TaskEngine` | M5 |
| `metrics/` | Aggregation of timing samples, kept out of the hot path by construction so measurement cannot distort what it measures. | `RunSummary`, `summarize()` | M5, M8 |
| `tasks/` | Concrete workloads. Neither vocabulary, mechanism nor policy: the CLI and the benchmark driver each reach for these independently. | `ComputeTask`, `SleepTask` | M5 |
| `cli/` | Argument parsing, validation, exit codes. Nothing else. | `Options`, `parse_args()` | M6 |
| `app/` | Composition root: the single place that constructs concrete objects and wires them, keeping everything below it injectable and testable. | `main()`, `run()` | M1, M6 |
| `tests/` | Unit, concurrency, and stress coverage. | — | M1 onward |
| `benchmarks/` | Repeatable measurement driver, as a separate binary so benchmark-only code never links into the product path. | — | M8 |

> **Correction (M4, D24).** `TaskEnvelope` was originally listed under
> `execution/`. That contradicts the dependency direction in §1.1: `ThreadPool`
> lives in `concurrency/` and owns a `BlockingQueue<TaskEnvelope>` per §2.1, so
> the envelope cannot belong to the layer above it. It is pure data and
> ownership with no concurrency or policy in it, so `core/` is its correct
> home. See D24.

**Why `metrics/` is aggregation-only.** Making it a pure function over a
`std::vector<Sample>` means percentile logic is testable with hand-written
inputs and zero threads, and it renders the measurement code physically
incapable of perturbing the hot path.

---

## 2. Ownership model

### 2.1 The ownership tree

```
main() stack frame
 `-- TaskEngine                              (by value -- RAII root)
      |-- std::atomic<uint64_t> next_id_
      `-- ThreadPool                         (by value)
           |-- BlockingQueue<TaskEnvelope>   (by value; declared FIRST)
           |-- std::vector<std::vector<Sample>> samples_  (one per worker; SECOND)
           `-- std::vector<std::thread>      (declared LAST)
                `-- each TaskEnvelope, while executing
                     |-- std::unique_ptr<Task>        (sole owner of the work)
                     `-- std::promise<TaskResult>     (sole owner of the result channel)

caller (any thread)
 `-- std::future<TaskResult>                 (the only other end of the promise)
```

### 2.2 Rules

- **Declaration order in `ThreadPool` is load-bearing.** Members are destroyed
  in reverse declaration order, so the thread vector is destroyed first (after
  `shutdown()` has joined every thread), and the queue and sample buffers —
  which those threads touch — are destroyed only once no thread can reach them.
  This is a correctness requirement, not a style choice, and will carry a
  comment in the source saying so.
- **`TaskEnvelope` is move-only (I2).** It holds a `unique_ptr` and a
  `promise`, both move-only; the compiler generates the correct special members
  and suppresses copying. Rule of zero — no hand-written special members.
- **`BlockingQueue<T>` never copies `T`.** `push` takes `T&&`; `pop` moves out.
- **No `shared_ptr` anywhere in the initial design.** There is no object with
  genuinely shared or unclear lifetime. If one appears, it is justified in
  DECISIONS.md first (D10 rationale applies).
- **No raw owning pointers (I1).** Raw pointers and references appear only as
  non-owning parameters whose lifetime is dominated by the caller's frame.
- **Per-worker sample buffers are exclusively owned by index.** Worker *i*
  touches only `samples_[i]`. The outer vector is sized once before any thread
  starts and is never resized, so no worker's write can race another's and no
  reallocation can invalidate anything. Merging happens only after `join()`.

### 2.3 O2: the `TaskEngine` checkpoint

Once `ThreadPool` owns the queue, the worker loop, and the sample buffers,
`TaskEngine` is left with ID assignment, envelope construction, and handing back
futures. That is real work, but thin.

**Checkpoint C1 stands (end of M5):** if `TaskEngine` has not acquired a second
genuine responsibility by then, merge it into `ThreadPool`. A pass-through class
kept for the shape of the diagram is exactly the kind of thing this project
exists to avoid.

---

## 3. Thread model

### 3.1 Threads in the system

| Thread | Count | Created by | Lifetime |
|---|---|---|---|
| Submitter | >= 1 (main thread via the CLI at M6; any caller thread) | caller | outside the engine |
| Worker | exactly `N` | `ThreadPool` constructor | joined in `shutdown()` |

**No thread-per-task, ever (I4).** The pool constructor is the only thread
creation point in the system. `N == 0` is rejected with `std::invalid_argument`
at construction and, earlier, by CLI validation. `N` may legitimately exceed
core count — workload profile C in [§8](#8-benchmarking-model) exists to show
why that is sometimes correct.

### 3.2 Shared mutable state (exhaustive)

| State | Guarded by | Owner |
|---|---|---|
| queue deque + `closed_` flag | `BlockingQueue::mutex_` | `BlockingQueue` |
| `next_id_` | `std::atomic<uint64_t>`, `fetch_add(relaxed)` | `TaskEngine` |
| `join_done_` | `ThreadPool::shutdown_mutex_` | `ThreadPool` |
| `samples_[i]` | not shared — single-writer by index | `ThreadPool` |

There is **one mutex on the only real contention point**, so lock-order
inversion is impossible by construction at this stage. `shutdown_mutex_` is
never held while `mutex_` is held: shutdown closes the queue, *then* takes the
join lock. **Rule: close before join, never the reverse.** This is written down
so it survives future edits.

`relaxed` ordering on the ID counter is justified: IDs require uniqueness, not
ordering relative to any other memory, and the queue mutex already establishes
every happens-before edge the envelope itself needs.

### 3.3 Blocking and wakeups

Two condition variables on the one mutex: `not_empty_` (waited by consumers) and
`not_full_` (waited by producers). **Every wait uses the predicate overload**,
so spurious and lost wakeups are both handled.

**No polling (I5):** no busy-wait, no timed wait, and no sleep on any production
path.

### 3.4 Documented limitation: recursive submission can deadlock

**A `Task::execute()` must not call `submit()` on the engine running it.** If
the queue is full, that worker blocks in `push`, which can only be unblocked by
a worker popping — and if every worker is blocked the same way, the system
deadlocks permanently.

This is **unsupported by design**. We do not defend against it: every defense
(unbounded overflow path, a second pool, an inline-execute fallback) costs more
complexity than the capability is worth here, and an inline-execute fallback
would additionally violate I6. The limitation appears in three places: a comment
on `TaskEngine::submit`, a section in `README.md`, and D5 in DECISIONS.md.

This is the honest cost of choosing backpressure. Stating it plainly is better
than leaving an undocumented landmine.

---

## 4. Task lifecycle

### 4.1 State machine

```
                      submit(unique_ptr<Task>)
                                |
                 +--------------v---------------+
                 |  engine assigns TaskId,      |
                 |  stamps t_submitted,         |
                 |  builds TaskEnvelope         |
                 +--------------+---------------+
                                | queue.push(...)  ---- may BLOCK if full
                                |
          +---------------------+--------------------------+
          | push succeeded                                 | queue closed
          |                                                | (before or *during* the block)
          v                                                v
       Queued ------- shutdown_now() discards -------> Rejected  (terminal)
          |
          | a worker pops it; stamps t_dequeued
          v
       Running
          |
          +-- execute() returns normally -------------> Succeeded (terminal)
          |
          `-- execute() throws -----------------------> Failed    (terminal)
                 (std::exception -> what();  ... -> "unknown exception")

   Every terminal state stamps t_finished and fulfils the promise exactly once.
```

`TaskId` is a monotonically increasing `std::uint64_t`, assigned at submission,
never reused.

### 4.2 Timing

Three `std::chrono::steady_clock::time_point`s per task — `t_submitted`,
`t_dequeued`, `t_finished` — yielding:

- **queue wait** = `t_dequeued - t_submitted`
- **execution time** = `t_finished - t_dequeued`
- **total latency** = `t_finished - t_submitted`

`steady_clock` specifically: it is monotonic and unaffected by wall-clock
adjustments, which `system_clock` is not. A measured duration must never be able
to come out negative because NTP stepped the clock.

### 4.3 What lands where

`Sample` is a trivially-copyable aggregate (`TaskId`, `TaskState`, three time
points) pushed to the worker's own buffer. The full `TaskResult` — which
additionally carries a `TaskError` holding a `std::string` message — is moved
into the promise.

This split means the common success path copies no strings into the metrics
buffer, and the buffer stays compact and cache-friendly.

### 4.4 No cancellation

Per-task cancellation is a **non-goal**. `shutdown_now()` covers the actual
requirement — stop doing work now — without threading a cancellation token
through every user-authored `execute()` and doubling the state machine. See D13.

---

## 5. Shutdown behavior

### 5.1 The two modes

| | `shutdown()` — **drain** | `shutdown_now()` — **abort** |
|---|---|---|
| New submissions | rejected | rejected |
| Already queued | **executed to completion** | **discarded -> `Rejected`** |
| In-flight task | runs to completion | runs to completion |
| Blocked producers | released; their push fails -> `Rejected` | same |
| Then | join all workers | join all workers |

**Neither mode interrupts a running `execute()`.** There is no portable, safe
way to do so, and pretending otherwise would be dishonest.

### 5.2 Mechanism

`close()` sets `closed_` under the mutex and calls `notify_all()` on **both**
condition variables — consumers *and* producers. A consumer's `pop` returns
`std::nullopt` when the queue is closed and empty; a producer's `push` returns
`false` immediately once closed, **including a producer already blocked in
`wait`**. Workers exit their loop on `nullopt`, and `shutdown` joins them.

`shutdown_now()` additionally drains the deque under the lock, fulfilling each
discarded envelope's promise with `Rejected` before releasing it.

### 5.3 RAII and idempotence

`~ThreadPool()` calls `shutdown()` and joins. Both entry points are idempotent
and callable from any thread; `join_done_` under `shutdown_mutex_` ensures each
thread is joined exactly once, since concurrent `join()` on the same thread is
undefined behaviour.

**Postcondition:** after any shutdown returns, no thread is joinable and no
worker thread exists. This is Milestone 4's acceptance criterion expressed as
something a test can assert.

### 5.4 The core invariant

> **I3 — Every `std::future` handed out by `submit()` is fulfilled exactly
> once, on every path:** both shutdown modes, a full queue, a closed queue, a
> throwing task, and destruction.

A caller blocking on `future.get()` can therefore never hang. This is the single
most important correctness property of the design and has a dedicated test group
([§10.3](#103-concurrency-concurrency)).

Note the consequence: `submit()` may block on a full queue and *then* return an
already-`Rejected` future. That is the correct and intended behaviour when
shutdown races a blocked producer.

---

## 6. Error propagation

```
  Task::execute() throws
          |
          v
  worker catch(const std::exception&) / catch(...)
          |                        exceptions NEVER cross the thread boundary
          v                        (an escaping throw would call std::terminate)
  TaskResult{ id, Failed, TaskError{what()}, timings }
          |
          +--> promise.set_value(...)   <-- set_value, NOT set_exception
          `--> Sample{ id, Failed, timings } -> samples_[worker]
                              |
                              v
                    RunSummary.failed_count > 0
                              |
                              v
                    process exit code 1
```

**`set_value`, not `set_exception` (D4):** a failed task is an expected outcome
that must be *counted*, not a control-flow event that forces every caller into a
try/catch. The exception type is lost; the `what()` text is preserved.

| Failure class | Mechanism | Surfaces as |
|---|---|---|
| Task body throws | caught in worker loop | `Failed` + message |
| Submit after shutdown, or push on a closed queue | return value | `Rejected` — never an exception |
| `workers == 0`, invalid capacity | `std::invalid_argument` at construction | caught in `main` -> exit **2** |
| Invalid CLI input | validated before construction | usage on `stderr` -> exit **2** |
| >= 1 task failed or rejected during the run | aggregated count | exit **1**, counts printed |
| `bad_alloc`, thread creation failure | propagates out of `run()`; RAII unwinds and joins | diagnostic -> non-zero exit |

**Exit codes.** `0` every task succeeded · `1` ran to completion but at least one
task failed or was rejected · `2` usage or configuration error.

**A partially-successful run is never reported as success (I6).**

Destructors must not throw. A `Task` implementation that could fail during
cleanup must fail in `execute()` instead; this will be stated on the `Task`
interface.

---

## 7. Task implementations

`Task` is polymorphic, owned by `std::unique_ptr<Task>` (D1). The abstraction
carries an obligation rather than a demonstration.

**Why the hierarchy earns its place.** The original argument was a runtime
factory selecting a concrete type from a broker payload. That milestone is gone
(D30), so the case now rests entirely on the two types differing in *kind*
rather than by a parameter — which they do, and which is what checkpoint C2
actually required. See D28 and D30 for the closed verdict.

**Two real implementations, landing at M5:**

| Type | Behaviour | Why it is genuinely different |
|---|---|---|
| `ComputeTask{iterations}` | deterministic integer mixing; accumulates a checksum that is consumed so the optimizer cannot delete it | CPU-bound — throughput scales with **cores** |
| `SleepTask{duration}` | blocks on `std::this_thread::sleep_for` | occupies a worker without consuming CPU — throughput scales with **workers**, well past core count |

These differ along the one dimension this project is about: how they respond to
worker count. Together they are what makes [§8](#8-benchmarking-model)'s
comparison possible, so the second type is required by the benchmark plan — not
added to justify a `virtual` keyword.

**Checkpoint C2 (end of M5):** if both types turn out to be distinguishable by a
single parameter and nothing dispatches on type at runtime, collapse to
`std::function<void()>` and delete the hierarchy. Deleting a hollow abstraction
is better than defending one.

---

## 8. Benchmarking model

### 8.1 Three workload profiles

A single profile cannot distinguish useful parallelism from synchronization
overhead, so there are three.

| Profile | Task | Per-task cost | Expected shape vs. worker count | What it demonstrates |
|---|---|---|---|---|
| **A — CPU-heavy** | `ComputeTask`, large K | ~100 us – 1 ms | near-linear to core count, then flat | **useful parallelism**: real work dominates, overhead is noise |
| **B — Lightweight** | `ComputeTask`, tiny K | ~100 ns – 1 us | rises then **falls**; may be worse at 8 workers than at 1 | **synchronization overhead**: the single queue mutex is the bottleneck, not the CPU |
| **C — Blocking / I/O-like** | `SleepTask`, 1–10 ms | ~1–10 ms, ~0 CPU | scales far beyond core count | **workers != cores**; why the pool size is configurable |

**Profile B is the point of the exercise.** A benchmark that only ever shows
speedup teaches nothing. Being able to say *"here is where adding threads made
it slower, and here is the mutex that caused it"* is the stronger and more
honest result.

### 8.2 The overhead floor

A dedicated measurement — `work = 0`, `workers = 1` — establishes the engine's
**per-task floor cost**: enqueue + dequeue + promise fulfilment + sample record.
Everything else is interpreted against it. The crossover where profile B becomes
profile A is approximately where per-task work exceeds that floor; we report the
**measured** value rather than assert a rule of thumb.

### 8.3 Protocol

- Parameters: `workers` in {1, 2, 4, 8, ... hardware_concurrency}, `tasks`,
  `work`, `queue-capacity`, `profile`.
- One **warm-up run, discarded**. Then **at least 5 repetitions**.
- Report **median**, plus min and max. Never a single run; never a mean without
  spread.
- Percentiles by **nearest-rank on the sorted sample**: p95 is the element at
  `ceil(0.95 * n)`, 1-indexed. Stated explicitly so the number is reproducible.
- Reported per configuration: wall time, throughput (tasks/s), queue-wait
  p50/p95, execution-time p50/p95, total-latency p50/p95, failed and rejected
  counts.
- **CSV to stdout**, so analysis is offline and raw numbers survive.
- Separate `bench-task-engine` binary; benchmark-only code never links into the
  product path.

### 8.4 Environment capture (automatic)

Every CSV carries a header block: CPU model and logical core count
(`/proc/cpuinfo`), kernel and distribution (`uname`), compiler and version,
`CMAKE_BUILD_TYPE`, effective flags, and the literal marker **`WSL2`**.

### 8.5 Validity threats — stated, not hidden

1. **Benchmarks run in Release only, never under a sanitizer.** ASan is roughly
   2x and TSan 5–15x; a sanitized number is not a performance number. The
   harness refuses to run if built with sanitizers enabled.
2. **The producer can become the bottleneck.** With one submitting thread and a
   bounded queue, if the producer is slower than the workers, the measurement is
   of the producer. Mitigation: report queue starvation alongside throughput,
   and size capacity so the queue stays non-empty. If it cannot, say so rather
   than publish the number.
3. **Queue capacity is a reported parameter**, because it materially changes
   results.
4. **WSL2 is a virtual machine.** Numbers are valid for *relative* comparison
   across worker counts on the same host. They are not bare-metal Linux figures
   and are never labelled as such.
5. CPU frequency scaling, turbo, and background load are uncontrolled on a
   laptop — hence medians over repetitions, and hence no claim smaller than the
   observed run-to-run spread.

**No number is ever written down that was not produced by an actual run.**

---

## 9. Sanitizer strategy

### 9.1 Build matrix

| Build dir | Type | Key flags | Purpose |
|---|---|---|---|
| `build/debug` | Debug | `-O0 -g` + warnings | day-to-day development |
| `build/asan` | Debug | `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -g` | memory errors, leaks, UB |
| `build/tsan` | Debug | `-fsanitize=thread -O1 -g` | **data races** |
| `build/release` | Release | `-O2 -DNDEBUG` | benchmarks only |

**ASan and TSan cannot be combined** — they are mutually exclusive runtimes,
hence two separate build directories rather than one flag. ASan and UBSan *do*
combine, so they share a build.

`-fno-sanitize-recover=all` makes UBSan **abort** rather than print and
continue, so a test or CI run actually fails instead of passing with a warning
buried in the log.

### 9.2 CMake shape

A single `SANITIZER` cache option (`off` | `address+undefined` | `thread`),
applied via `target_compile_options` **and** `target_link_options` on our own
targets only — never through global `CMAKE_CXX_FLAGS`. The same target-scoped
discipline applies to the warning set
(`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror`): third-party
code must not inherit our warning policy, or an upstream tag bump breaks our
build for no good reason.

### 9.3 Why GoogleTest is built from source

TSan only observes races in **instrumented** code. A system-installed, prebuilt
`libgtest` is not instrumented, which hides races inside test infrastructure and
produces confusing partial stacks. Building GoogleTest from a pinned source tag
means the entire program — test framework included — is instrumented under TSan.
This is a concrete justification for `FetchContent` over `find_package` (D15).

### 9.4 Where sanitizers earn their keep

- **TSan** is the real verification mechanism for Milestones 3 and 4. The queue
  and pool suites run under it.
- **ASan/LSan** catch use-after-free on shutdown paths — precisely the class of
  bug that the member-declaration-order rule in [§2.2](#22-rules) exists to
  prevent. If that ordering is wrong, ASan is what will say so.
- **UBSan** covers the integer and lifetime rules on the study checklist.

Suppression files are permitted only with a written justification per entry. A
suppression without a reason is a hidden bug.

---

## 10. Testing strategy

### 10.1 Structure

CTest labels — `unit`, `concurrency`, `stress`, `integration` — one test binary
each, so the narrowest useful suite runs first. `integration` was added at M7
(D33): it runs the real executable in a child process to check exit codes,
stdout versus stderr, and graceful handling of SIGINT and SIGTERM.

### 10.2 Unit (`unit`, no threads)

`core/`: task lifecycle, result and error construction, move-only semantics of
`TaskEnvelope`, state transitions. `concurrency/`: single-threaded FIFO order,
capacity enforcement, close semantics. `metrics/`: percentile arithmetic against
hand-computed inputs, including n=0, n=1, and n=2.

### 10.3 Concurrency (`concurrency`)

- **Conservation:** N producers x M consumers; every pushed item is popped
  exactly **once** — no loss, no duplication — verified by a per-item counter,
  not by a total.
- **Wakeups:** a blocked `pop` wakes on `push`; a blocked `push` wakes on `pop`;
  `close()` wakes **all** blocked threads of both kinds.
- **Shutdown group:** drain executes everything queued; abort rejects everything
  queued; the in-flight task still completes; `~ThreadPool` joins; double
  shutdown is idempotent; shutdown while producers are blocked in `push`.
- **Invariant I3:** across every one of the above, assert that **every future
  was fulfilled exactly once**. This is the highest-value test in the suite.
- Worker counts 1, 2, 4, 8; task counts spanning under- and over-capacity.

### 10.4 Stress (`stress`)

High task volume with tiny tasks — the configuration most likely to expose a
lost wakeup — run under TSan.

### 10.5 Discipline

**No sleep-based synchronization in tests.** Ordering is established with
condition variables or counters. A test that passes because a sleep was long
enough is a test that will fail in CI.

Two bounded waits on a predicate remain, each because no correct alternative
exists (D33). One waits for an abort to drain the queue, a transition that
happens inside the production pool and cannot be observed without a test hook
in library code. The other waits for exited threads to leave `/proc/self/task`,
because `pthread_join` returns before the kernel removes that entry and the
kernel publishes no event for the final step.

**What these tests actually prove.** A concurrency test passing once proves very
little — it proves that one interleaving worked. Repetition raises confidence
slightly. **TSan is the actual evidence**, because it reasons about the
happens-before graph rather than sampling schedules. The suite's job is to
*create* interleavings; TSan's job is to *judge* them. Both are required, and a
green suite alone is never claimed as proof of race-freedom.

A failing test is never weakened or deleted to go green.

---

## 11. Platform

**WSL2 Ubuntu is the first-class development and test target.** Windows is the
host and editor environment only.

The repository must live on the **ext4 filesystem inside WSL**, not under
`/mnt/...`. The 9p bridge makes git and CMake substantially slower and reports
incorrect file modes to Linux tools, which breaks executable bits and
permissions. The operational observation folded into Milestone 7 (`ps`,
`vmstat`, signal handling, graceful termination) has no meaningful Windows
equivalent.

Consequence for measurement: see [§8.5](#85-validity-threats--stated-not-hidden),
item 4. The M8 container (D31) exists to make the build reproducible off this
host; it is not a measurement environment, because a container on top of a VM
puts two layers between a number and the hardware.

---

## 12. Dependencies

Current count: **one** — GoogleTest, and only in the test build. The product
library and the executable link nothing outside the C++ standard library. Each
dependency is justified before it is added (I7).

With the roadmap ending at M8 (D30), this is also the **final** count: no
further dependency is planned. The M8 container (D31) pins a base image, which
is a build input rather than something the library links against.

| Dependency | Milestone | Why the standard library is insufficient | Alternative considered |
|---|---|---|---|
| GoogleTest (FetchContent, pinned commit) | M1 | C++17 has no test framework; CTest alone provides no assertions or fixtures. Source build also gives TSan instrumentation ([§9.3](#93-why-googletest-is-built-from-source)). | Catch2 (viable); hand-rolled asserts — rejected, poor diagnostics |
| *(none)* for benchmarking | M8 | End-to-end throughput and per-task percentiles come directly from `<chrono>` | Google Benchmark — rejected: built for microbenchmarks, does not fit whole-pipeline measurement |

---

## 13. Repository layout

Created in Task 0.2. Directories appear when the milestone that needs them
arrives, not before ([§0.1](#01-incrementalism)).

```
task-engine/
|-- CLAUDE.md   PLAN.md   README.md   .gitignore
|-- Dockerfile       multi-stage build of the CLI (M8, D31)
|-- docs/            ARCHITECTURE.md   DECISIONS.md
|-- include/taskengine/{core,concurrency,execution,metrics}/
|-- src/{core,concurrency,execution,metrics,cli,app}/
|-- tests/           unit/  concurrency/  stress/  integration/  support/
|-- benchmarks/
```

One library target `taskengine` (D7), one CLI binary `task-engine`, one
benchmark binary `bench-task-engine`, one test binary per label group. Build
directories (`build/*`) are gitignored and never live inside source directories.

Namespace is flat `taskengine` (D8); the directory conveys the module.

---

## 14. Open items

None outstanding.

| # | Item | Resolution |
|---|---|---|
| O1 | WSL2 target path, and whether `PLAN.md`/`CLAUDE.md` carry over | Resolved at Task 0.2: `~/projects/task-engine` on ext4; both files carried over in the first commit. |
| O2 | Whether `TaskEngine` is too thin to justify | Became checkpoint C1 in [§0.3](#03-planned-checkpoints); resolved at M5 (D27), with one more honest look due at the end of M6. |
| O3 | Broker message format and the JSON dependency | Removed with the roadmap it belonged to (D30). |

The only open architectural question left is C1, and it has a scheduled date.
