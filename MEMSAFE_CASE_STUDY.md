# memsafe - Case Study

## Overview

**`memsafe` is a header-only C++ library that brings practical memory-safety
patterns into existing C++ code without requiring a new compiler, a new runtime,
or a rewrite into another language.** It does not claim to be a full Rust borrow
checker. Instead, it targets a useful middle ground: move-only ownership,
runtime borrow counters, generational handles, scope-bound allocation, explicit
violation policies, and optional backend annotations that can cooperate with
compiler or hardware safety tooling when available.

The project is shaped for systems C++: it is source-distributed through headers,
keeps configuration in preprocessor switches, supports a C++20 baseline with a
C++17 compatibility lane, and validates the public API through ordinary
executable tests, compile-fail checks, sanitizer lanes, property/fuzz replay
lanes, concurrency tests, and performance gates.

| Area | Snapshot |
|---|---|
| **Project type** | Header-only C++ memory-safety primitives |
| **Core language** | C++20 baseline, C++17 compatibility mode |
| **Integration model** | Add `include/` to the include path and include `<memsafe/memsafe.hpp>` |
| **Main safety model** | Unique ownership, counted borrows, generational handles, scoped lifetimes |
| **Public API families** | `Owner`, `Ref`, `MutRef`, `SlotMap`, `Handle`, `Scope`, `SyncOwner`, `Arc`, `Mutex` |
| **Configuration** | `MEMSAFE_ON_VIOLATION`, `MEMSAFE_RELEASE_CHECKS`, `MEMSAFE_CXX17_COMPAT`, backend macros |
| **Validation** | CMake/CTest lanes driven by `tools/run_tests.py`, negative compile tests, JSON summaries |
| **Optional lanes** | ASan/UBSan, TSan, MSan, HWASan, Valgrind, property, fuzz, concurrency, performance, Clang/CHERI/Rusty-C++ backends |
| **Development model** | Human raw-spec seed, AI-agent debate for specs, AI-generated implementation and tests, AI reviewer loops |

---

## AI-Native Development Model

This project is also a case study in AI-native software development. The human
input was intentionally front-loaded: a raw initial specification and high-level
direction. From that seed, AI agents generated the detailed project
specification set in `documentation/`:

| Artifact | AI-generated role |
|---|---|
| `documentation/01_prd.md` | Product requirements, non-goals, API roster, safety constraints, roadmap |
| `documentation/02_test_plan.md` | Test categories, sanitizer strategy, property/fuzz/concurrency/performance expectations |
| `documentation/03_architecture.aadl` | Architecture model for components, data, flows, and validation-facing structure |
| `documentation/04_infra.md` | Build/test infrastructure plan and runner requirements |
| `documentation/05_backlog.md` | Work-package breakdown used by implementer agents |
| `documentation/06_spec_decisions.md` | Debate/review decision log for contested specification points |
| `documentation/BUILD_AND_VALIDATE.md` | Operational documentation for the validation runner |

The key mechanism was not a single prompt-to-code pass. The specs were produced
through an AI-agent debating and review loop: agents proposed requirements,
challenged ambiguities, converged on decisions, and recorded the accepted
outcomes. Human supervision after the initial raw spec was minimal and focused
on steering or accepting the overall direction.

The source code and test code were then generated from those AI-produced specs.
AI agents acted as implementers, reviewers, and validation operators. They wrote
the public headers, examples, unit tests, compile-fail tests, property/fuzz
replay tests, concurrency tests, lane definitions, and validation tooling based
on the specification artifacts. Human input did not manually author the source
or testing code; after the generated specs existed, implementation and testing
proceeded from those specs and automated build/test feedback rather than from
manual human code edits.

That makes the repository useful in two ways:

- As a C++ memory-safety library with concrete primitives and executable tests.
- As evidence for a spec-first AI workflow where AI agents can move from
  contested requirements to implementation and review without continuous human
  intervention.

---

## The Problem Space

C++ remains a default language for systems software, but temporal memory bugs
are still expensive: use-after-free, double-free, stale handles, invalid
borrows, accidental sharing, and lifetime mistakes often survive ordinary code
review. Compilers, sanitizers, and static analyzers help, but many codebases
cannot switch wholesale to a memory-safe language and cannot require every user
to adopt a specialized compiler.

That leaves a practical question:

- Can a library make common ownership and borrowing mistakes harder?
- Can invalid states be detected close to the API boundary?
- Can a project add safety affordances without changing its build model?
- Can checked debug behavior become zero-overhead release behavior when needed?
- Can the validation story be strong enough for AI-assisted and human review?
- Can AI-generated specifications be strong enough to drive AI-generated source
  and test code without hand-written implementation work?

`memsafe` answers those questions with a deliberately bounded design. It moves
some important safety invariants into types and runtime checks, while clearly
documenting what remains outside the v1 model.

---

## The Solution: Runtime-Checked Safety Primitives

The core idea is to make ownership and access explicit through small wrapper
types:

```text
unique owner
  |
  +--> immutable counted borrow: Ref<T>
  |
  +--> exclusive counted borrow: MutRef<T>

bounded slot map
  |
  +--> scalar Handle<T> = index + remembered generation
  |
  +--> lookup checks current per-slot generation

scope arena
  |
  +--> raw T* valid only while Scope is alive
  |
  +--> sanitizer/memcheck lanes own stale-pointer detection in v1
```

When runtime checks are enabled, invalid borrow or handle states route through a
single violation-policy mechanism. When `MEMSAFE_RELEASE_CHECKS=0`, check fields
and violation call sites in the dependent primitives compile away for the
release lane.

This gives users a conscious trade-off: fail-fast diagnostics in debug and
validation builds, and minimal overhead in performance-sensitive builds.

---

## Public API Shape

The public surface is intentionally organized by safety concern.

| API | Role |
|---|---|
| `memsafe::Owner<T>` | Move-only RAII owner for one heap-allocated object. |
| `memsafe::Ref<T>` | Copyable immutable counted borrow. |
| `memsafe::MutRef<T>` | Non-copyable exclusive mutable counted borrow. |
| `memsafe::Slot<T>` | Inline storage plus a per-slot atomic generation counter. |
| `memsafe::Handle<T>` | Nodiscard scalar token containing a slot index and remembered generation. |
| `memsafe::SlotMap<T, Capacity>` | Fixed-capacity allocator using a Treiber-stack free list and per-slot generations. |
| `memsafe::Scope` | RAII arena that destroys created objects in reverse creation order. |
| `memsafe::SyncOwner<T>` | Cross-thread owner with atomic borrow accounting. |
| `memsafe::SyncRef<T>` / `SyncMutRef<T>` | Thread-safe counted borrow handles. |
| `memsafe::Arc<T>` | Atomic reference-counted shared owner. |
| `memsafe::Mutex<T>` | Mutex wrapper whose `lock()` returns a `MutRef<T>`. |

The umbrella header keeps integration simple:

```cpp
#include <memsafe/memsafe.hpp>
```

and the feature headers stay separated:

```text
config.hpp
violation.hpp
backend.hpp
owner.hpp
handle.hpp
scope.hpp
sync.hpp
```

---

## Violation Policies

Detected library-managed violations go through `MEMSAFE_ON_VIOLATION`.

| Policy | Behavior |
|---|---|
| `MEMSAFE_VIOLATION_ABORT` | Default. Emits a diagnostic and aborts. |
| `MEMSAFE_VIOLATION_THROW` | Throws `memsafe::violation` with diagnostic/source metadata. |
| `MEMSAFE_VIOLATION_HANDLER` | Calls an installed handler, falling back to abort if no handler exists. |

The policy names matter because tests compile specific lanes and source files
against the accepted policy vocabulary. The project avoids ambiguous old names
such as "assert" or "terminate" and keeps the runtime behavior auditable.

---

## Design Decisions

### 1. Header-Only By Construction

The library distribution has no `.cpp` build step. Users add `include/` to the
compiler search path and include the relevant headers. That keeps adoption
simple for native projects with different build systems and toolchains.

The cost is that the library cannot enforce whole-program lifetime rules. The
case study is explicit about that boundary: `memsafe` is a practical safety
layer, not a replacement C++ language.

### 2. Runtime Borrow Checks Instead Of A Fake Borrow Checker

`Owner<T>` owns the payload and issues `Ref<T>` or `MutRef<T>` values. In checked
lanes, immutable borrows increment a shared count and mutable borrows acquire an
exclusive token. Requesting mutable access while any borrow is live reports a
`borrow_exclusivity` violation.

That does not prove arbitrary lifetime correctness, but it catches a high-value
class of API-level misuse close to the object that knows the borrow state.

### 3. Nodiscard As A Compile-Time Guardrail

Borrow and handle types are marked `[[nodiscard]]`, and borrow-returning methods
also return nodiscard values. This supports compile-fail tests that reject
discarded borrow results under warnings-as-errors builds.

The design uses the compiler for what it can reliably see: an ignored borrow or
handle value is suspicious enough to make visible.

### 4. Generational Handles Instead Of Raw Pointer Identity

`Handle<T>` stores no pointer. It is an index plus a remembered generation.
`SlotMap<T, Capacity>` owns the slots and compares the handle generation against
the selected slot's current atomic generation on lookup.

When a slot is freed, its generation is bumped. A stale handle can therefore be
recognized even if the slot index is reused for a later object.

### 5. Per-Slot Atomic Generations And A Lock-Free Free List

The slot map avoids a global generation counter and a global mutex. Each slot
has its own atomic generation, while allocation/deallocation publish free slot
indices through a Treiber-stack head containing an index and ABA counter.

This keeps the stale-handle check local to the slot and makes the allocation
path a specific, testable concurrency claim rather than a vague "thread-safe"
label.

### 6. Scope Is Honest About Raw Pointers

`Scope::create<T>(args...)` returns a raw `T*` owned by the scope. The scope
destroys all created objects in reverse creation order, but v1 does not wrap the
returned pointer in a checked scope handle.

That means stale access after the `Scope` dies is outside the library violation
policy and belongs to sanitizer or memcheck lanes. This is a good example of the
project's discipline: the docs state the boundary instead of pretending the
library can detect what it cannot represent.

### 7. Optional Backends Are Hooks, Not Dependencies

Macros such as `MEMSAFE_BORROWS(x)`, `MEMSAFE_LIFETIMEBOUND`,
`MEMSAFE_BACKEND_CLANG`, `MEMSAFE_BACKEND_CHERI`, and
`MEMSAFE_BACKEND_RUSTY_CPP` let the library expose additional annotations where
the environment can use them.

By default, these hooks compile away. That avoids locking the project to one
vendor while still giving advanced toolchains a place to attach stronger
diagnostics.

### 8. Release-No-Checks Is A Tested Mode

`MEMSAFE_RELEASE_CHECKS=0` is not just an optimization idea in prose. It is a
lane in `testing/infra_lanes.json`. The release lane verifies that the library
still compiles and runs when runtime check state is removed from the dependent
types.

That matters because performance-sensitive systems code often needs an explicit
debug-vs-release contract.

### 9. The Runner Is Part Of The Product

The repository includes a generic `tools/run_tests.py` runner that discovers
tests by folder convention and runs them through local, WSL, Docker, or Podman
backends. It writes a machine-readable `testing/build/summary.json`, plus JUnit
XML when the selected CTest supports it.

This makes the validation surface usable by humans, CI, and AI agents without
requiring any of them to scrape compiler logs.

### 10. Specs Are Executable Work Orders

The AI-generated PRD, test plan, architecture model, backlog, and decision log
are not decorative documents. They form the work-order boundary for implementer
agents. The source and test files cite work-package IDs, encode accepted
specification decisions, and route contested behavior through explicit tests or
decision records.

That structure is what allowed implementation agents and reviewer agents to
work with minimal human input after the raw spec: the detailed project context
was carried by the generated specification set, not by ad-hoc conversation.

---

## Worked Example

The ownership API makes a valid access pattern explicit:

```cpp
#include <memsafe/memsafe.hpp>

#include <string>
#include <utility>

int main() {
    memsafe::Owner<std::string> title(std::string("daily"));

    {
        auto first = title.borrow();
        auto second = first;
        (void)first->size();
        (void)second->size();
    }

    {
        auto write = title.borrow_mut();
        write->append(" grind");
    }

    memsafe::Owner<int> source(42);
    memsafe::Owner<int> destination(std::move(source));

    return destination.has_value() && *destination == 42 ? 0 : 1;
}
```

The important shape is lexical: immutable borrows end before the mutable borrow
is requested. If a caller asks for a mutable borrow while immutable borrows are
still live in a checked lane, the configured violation policy runs.

Generational handles address a different class of bug:

```cpp
memsafe::SlotMap<int, 4> values;

auto first = values.allocate(10);
const int* first_value = values.get(first);

values.deallocate(first);

auto second = values.allocate(20);
const int* second_value = values.get(second);

(void)first_value;
(void)second_value;
```

The old `first` handle is not a pointer into freed storage. It is a scalar token
whose remembered generation no longer matches the slot after deallocation and
reuse. Checked lookups can therefore reject stale access through the policy
path.

---

## Validation And Tooling

The validation setup is deliberately executable and dependency-conscious.

The default path is:

```text
python tools/run_tests.py --backend auto
```

The runner chooses an available backend, configures CMake, builds discovered
tests/examples, runs CTest, and writes a stable summary:

```text
SUMMARY backend=local passed=... failed=... result=OK lanes_ok=[debug,release,asan-ubsan,cxx17] lanes_failed=[]
```

The default lane set is:

| Lane | Purpose |
|---|---|
| `debug` | C++20 checks-on baseline. |
| `release` | C++20 with `MEMSAFE_RELEASE_CHECKS=0`. |
| `asan-ubsan` | AddressSanitizer plus UndefinedBehaviorSanitizer where supported. |
| `cxx17` | Compatibility lane with `MEMSAFE_CXX17_COMPAT=1`. |

Preset-only lanes wire in deeper categories without making them required for
every local build:

| Preset | Purpose |
|---|---|
| `property` | Deterministic property-style tests under `testing/tests/property/`. |
| `fuzz` | Fuzz replay tests under `testing/tests/fuzz/`. |
| `concurrency` | Interleaving/concurrency tests, including vendored Relacy support. |
| `perf` | Release/no-checks primitive performance gate. |
| `tsan`, `msan`, `hwasan`, `valgrind-cxx17` | Environment-gated dynamic analysis lanes. |
| `clang-backend`, `cheri`, `rusty-cpp` | Optional backend annotation lanes. |

Negative compile tests live under `testing/tests/compile_fail/` and carry an
`INFRA_EXPECT_FAIL` directive so a must-not-compile test passes only when the
compiler fails for the expected reason. That makes compile-time API contracts
part of the normal validation flow.

GitHub Actions runs the local backend on Ubuntu and uploads `summary.json` plus
per-lane JUnit XML artifacts.

---

## What It Demonstrates

- **Practical C++ memory-safety design** without a custom compiler or runtime.
- **Spec-first AI development** where agents generated the PRD, test plan,
  AADL architecture, infrastructure notes, backlog, and decision log from a
  human raw-spec seed.
- **AI-generated implementation and validation** where agents wrote source,
  examples, tests, lane wiring, and tooling from the generated specs.
- **AI reviewer loops** that challenged contested behavior and recorded
  accepted decisions in `documentation/06_spec_decisions.md`.
- **Honest scope control**: the project states which invariants are enforced by
  the library and which require sanitizers or future API changes.
- **Move-only ownership and counted borrowing** with explicit violation paths.
- **Generational handle design** for stale-reference detection without storing
  raw pointers in handles.
- **Concurrency-aware primitives** through atomic borrow state, `Arc`, `Mutex`,
  and dedicated concurrency lanes.
- **Backend-ready annotations** that preserve portability when unavailable.
- **Executable specifications** through compile-fail tests, sanitizer lanes,
  property/fuzz replay, performance gates, and JSON summaries.
- **AI-friendly validation**: a tool or agent can run one command and parse a
  stable result instead of interpreting ad-hoc build output.

---

## First-Pass Review Notes

A first reviewer should focus on the safety contract and the evidence behind it:

- Are the AI-generated specs detailed enough to justify the implementation
  choices without relying on hidden human context?
- Do source and test artifacts trace back to the generated PRD, test plan,
  backlog, and decision log?
- Are implementer-agent outputs reviewed by independent reviewer-agent checks
  rather than accepted as prompt output?
- Are users likely to understand that this is partial memory safety, not a full
  Rust-style borrow checker?
- Are `Owner`, `Ref`, and `MutRef` lifetimes documented strongly enough to avoid
  non-owning handles outliving their owners?
- Does `MEMSAFE_RELEASE_CHECKS=0` remove only checks that users are willing to
  trade away in production?
- Are stale `Scope::create()` raw pointers clearly assigned to sanitizer or
  memcheck lanes rather than library-managed violations?
- Does `SlotMap`'s fixed capacity match intended workloads, or should a future
  `GrowableSlotMap` be prioritized?
- Are optional backend lanes realistic for the environments users actually run?
- Does the performance gate isolate primitive overhead rather than hiding it
  behind payload work?
- Should the README be updated to describe the full implemented library rather
  than primarily the build-and-validate infrastructure?

The current project is most compelling when treated as a layered safety toolkit:
use types to make ownership visible, use runtime checks to catch invalid access
patterns near the API boundary, use compile-fail tests for static contracts, and
use the lane runner to keep those claims executable. It is also a concrete
example of an AI-agent workflow where the human supplies the starting intent and
the agents carry the specification, implementation, review, and validation loop.
