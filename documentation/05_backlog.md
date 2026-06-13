## 1. How to read and drive this backlog

This is a **single-file, dual-audience** backlog (human-readable rendered, machine-parseable raw).
Each work package (WP) is one fenced `wp` block of `key: value` lines, including a `review_rework_ledger: []` list, so an agent can update one WP's `status` or append one review/rework object without disturbing any other WP.

### 1.1 Identifier scheme (C2)

```
CPP_MEMSAFE-<NNNN>-<SUFFIX>
            │      └─ FUNC = functional library code (include/memsafe/**)
            │         TEST = testing/validation code (testing/**, examples/**, infra_lanes.json)
            └─ 4-digit zero-padded global increasing integer, assigned in steps of 10.
               Per-slice gap blocks: Slice 0 → 0010-0090, Slice 1 → 0100-0190, …, Slice 9 → 0900-0990.
```

- IDs are **never reused or renumbered**. To insert between `0100` and `0110`, use `0105`; between `0220` and `0230`, use `0225`.
- A retired WP keeps its ID with `status: superseded` and a `superseded_by:` pointer; it is never deleted.
- A WP produces **exactly one** work-product class (two-work-product-classes constraint). A `FUNC`
  WP and the `TEST` WP(s) validating it are always separate IDs.

### 1.2 Status lifecycle (C3)

```
not-started → in-progress → awaiting-review → finalized
                                  ↑      │
                                  └ in-rework ┘   (loop any number of times)
blocked = a single current status; record the blocker in notes or a review row.
```

Exactly six tokens: `not-started`, `in-progress`, `awaiting-review`, `in-rework`, `blocked`, `finalized`. `blocked` is a normal single current-status token, not simultaneous metadata; the blocking dependency or condition is recorded in the WP notes or the latest review/rework row. A WP may be `finalized` **only** when its acceptance criteria are met **and** a review object records `approved`.

### 1.3 Work → review → rework loop & rework-reason taxonomy (C4)

Each WP carries a `review_rework_ledger` list. Every review iteration appends one object with these fields:

| field | meaning |
|-------|---------|
| `iter` | 1-based loop iteration |
| `actor` | reviewer id (AI agent or human) |
| `timestamp` | ISO-8601 |
| `verdict` | `approved` or `rework` |
| `reason` | one of `discovered-bug`, `spec-gap`, `failed-validation`, `review-finding` (empty if `approved`) |
| `artifact_ptr` | path to the offending file/symbol (empty if `approved`) |
| `clause_ptr` | the F1/F2/F3 clause or F4 lane/test ID violated (empty if `approved`) |

`reason` tokens: **discovered-bug** (defect in produced code), **spec-gap** (implementation gap vs an
F1/F2/F3 clause), **failed-validation** (a paired F4 lane/test failed), **review-finding** (reviewer
objection not covered by the other three). This row travels with the WP so the next implementer pass
knows exactly what to fix.

### 1.4 Traceability fields

Every WP declares `traces_f1` (slice/section), `traces_f2` (test category/case), `traces_f3`
(component/`Slice_Id`). Coverage of the spec is auditable from these fields alone (see §4 matrix).

### 1.5 Dependency & sequencing rule

`depends_on` lists prerequisite WP IDs. The graph is acyclic and topologically executable.
**Slice 0 gate:** all of `CPP_MEMSAFE-0010-FUNC`, `CPP_MEMSAFE-0020-FUNC`, `CPP_MEMSAFE-0030-FUNC`,
`CPP_MEMSAFE-0035-TEST`, `CPP_MEMSAFE-0040-TEST`, `CPP_MEMSAFE-0050-TEST`, and
`CPP_MEMSAFE-0060-TEST` must be `finalized` before any Slice 1/2/4/6/7 WP starts (F1 Milestones;
F0 R3.RESOLUTION.Q8). The downstream Slice 1/2/4/6/7 functional WPs list those gate IDs in
`depends_on`, so all tests in those slices inherit the gate transitively through their functional prerequisites.
**Acceptance-vs-graph rule (Q12/Q13/Q15):** when a WP's acceptance criterion asserts a property over a
set of other WPs' artifacts (for example "full type roster compiles" or "passes every test"), that WP must
list every WP whose artifact the property ranges over in `depends_on`, so dependency-order execution alone
cannot finalize the assertion before the artifacts it ranges over exist. This is why the C++17 compat WP
`CPP_MEMSAFE-0800-FUNC` depends on every type-producing FUNC WP (`0100`, `0205`, `0300`, `0400`, `0500`)
and the terminal integration WP depends on every leaf TEST WP.
**Terminal integration gate (Q13):** `CPP_MEMSAFE-0920-TEST` is the project-completion WP whose
acceptance is "`run_tests.py` discovers and passes *every* test/example/compile_fail". That acceptance
is only verifiable once every leaf TEST WP is `finalized`, so `0920` lists every leaf TEST WP in
`depends_on` and sits at the end of the DAG; no other WP depends on it. This makes dependency-order
execution itself enforce that the full-suite check runs last, matching the Slice 0 gate precedent of
encoding sequencing in the raw graph rather than prose alone.

### 1.6 F4 drop-in contract (acceptance is expressed as F4 checks — C7)

- `include/memsafe/**` → include path. `testing/tests/*.cpp` & `examples/*.cpp` → built+run, **exit 0 = pass**.
- `testing/tests/compile_fail/*.cpp` → **must fail to compile** (`WILL_FAIL`).
- `testing/infra_lanes.json` lanes: `debug`, `release` (`MEMSAFE_RELEASE_CHECKS=0`), `asan-ubsan`
  (`-fsanitize=address,undefined`), `cxx17` (`MEMSAFE_CXX17_COMPAT=1`). Each test `#define`s its own
  `MEMSAFE_ON_VIOLATION`. The generic CMake exposes only `INFRA_STD`/`INFRA_SANITIZE`/`INFRA_DEFINES`
  and **does not link external libraries**. It also accepts lane-scoped source globs, excludes, and `test_launcher` from `testing/infra_lanes.json`; the shipped optional lanes already route property tests to `testing/tests/property/*.cpp` (under `MEMSAFE_ENABLE_PROPERTY_TESTS=1`), fuzz replay tests to `testing/tests/fuzz/*.cpp` (`MEMSAFE_ENABLE_FUZZ_TESTS=1`), concurrency tests to `testing/tests/concurrency/*.cpp` (`MEMSAFE_ENABLE_CONCURRENCY_TESTS=1`), and performance tests to `testing/tests/perf/*.cpp` (`MEMSAFE_ENABLE_PERF_TESTS=1`); optional `tsan`, `msan`, `hwasan`, and `valgrind-cxx17` lanes re-run the default test set under those sanitizers/memcheck. Normal runtime tests build with `-Wall -Wextra` (no `-Werror`); only `compile_fail/*.cpp` are compiled with `-Werror -Wunused-result`.

### 1.7 Round decisions and residual final-audit points

- **Q1 resolved:** ABORT-policy and sanitizer-negative checks use a parent/child death-test helper. The child triggers abort or sanitizer failure; the parent verifies abnormal termination and exits 0, satisfying the F4 runtime-test contract without a tool edit. (Confirmed independently in Round 6: CMake `WILL_FAIL` warns that signal/abort/heap failures may still fail a test even when `WILL_FAIL` is true, so a runtime `WILL_FAIL` mode cannot replace the death-test wrapper.)
- **Q4 resolved:** the six feature headers are `violation.hpp`, `backend.hpp`, `owner.hpp`, `handle.hpp`, `scope.hpp`, and `sync.hpp`; `config.hpp` is support plumbing included by the umbrella but is not counted as a feature header.
- **Q5 and Q8 resolved for F4 drop-in:** required WPs use self-contained exit-0 tests and shared helpers from `CPP_MEMSAFE-0035-TEST`. Link-dependent frameworks named in F2, such as GoogleTest, RapidCheck, libFuzzer, and google/benchmark, are not required artifacts under the no-tool-edit constraint. Their behavior is represented through F4-compatible adapters with the same category thresholds and oracles; true linked-framework integration remains an optional future infrastructure extension, not an implementation WP.
- **Q6 resolved:** Slice 2 functional work is split into `CPP_MEMSAFE-0200-FUNC` for `Slot<T>` / `Handle<T>` generation primitives and `CPP_MEMSAFE-0205-FUNC` for the bounded `SlotMap<T>` allocator and Treiber free-list.
- **Q9 resolved:** every WP block includes `review_rework_ledger: []`, the empty structured ledger that later reviewers append to before any WP can move to `finalized`.
- **Q10 resolved (Round 4):** F2 sets a per-type property-test mandate -- at least 10 000 sequences/run for each non-stateful type (`Owner`, `Ref`, `MutRef`, `Handle`) and at least 1 000 sequences/run for each stateful type (`SlotMap`, `Mutex`, `Arc`, `Scope`) -- and the Slice 2 and Slice 5 Definitions of Done inherit it. v03 covered `Owner`/`Ref`/`MutRef` (`0140`), `SlotMap` (`0220`), and `Scope` (`0330`) but left `Handle` (non-stateful), `Arc`, and `Mutex` (stateful) without a property-test WP. v04 adds `CPP_MEMSAFE-0225-TEST` (`Handle` non-stateful property test) and `CPP_MEMSAFE-0535-TEST` (`Arc`/`Mutex` stateful property tests), so every F2-enumerated property type now has an owning WP with the correct threshold.
- **Q11 resolved:** the F2 sanitizer matrix (TSan/MSan/HWASan/Valgrind) is covered by F4 lane re-runs. The optional lanes already route the default runtime tests under each sanitizer or launcher, and the multithreaded unit/concurrency WPs provide the targets; no dedicated per-sanitizer test-file WP is required.
- **Q12 resolved (Round 5):** v04 stated the full Slice 0 gate in prose, but some downstream `depends_on` lists named only `0030`. v05 expands the raw `depends_on` graph for Slice 1/2/4/6/7 functional WPs to include every Slice 0 gate WP (`0010`, `0020`, `0030`, `0035`, `0040`, `0050`, `0060`), so dependency-order execution cannot start those slices before the gate is finalized.
- **Q13 resolved (Round 6):** v05 placed the final integration WP `CPP_MEMSAFE-0920-TEST` (whose acceptance is "passes every test/example/compile_fail") with `depends_on: [0040, 0810, 0910]`, so a topological executor could finalize it while ~24 leaf TEST WPs (for example `0610`, `0710`, `0900`) were unfinished, making its acceptance unverifiable in dependency order. v06 expands `0920`'s `depends_on` to every leaf TEST WP, making it a terminal integration gate whose dependency closure equals the whole runnable test set. No other WP changed.
- **Q14 resolved (Round 7):** v06 satisfied graph, status, artifact, and acceptance checks, but 25 downstream TEST WPs lacked an explicit `inputs:` field. v07 adds those missing input declarations, pointing each TEST WP at the relevant F1/F2 clause, prerequisite WP artifacts, and any F4 or shared-support helper it consumes. No IDs, dependencies, artifacts, or acceptance criteria changed.
- **Q15 resolved (Round 8):** v07 satisfied the inputs, status, ledger, artifact, and acceptance checks for all 44 WPs, the Slice 0 gate, and the terminal integration gate, but the C++17 compat WP `CPP_MEMSAFE-0800-FUNC` had `depends_on: [0030, 0100, 0205, 0300]` while its acceptance asserts "full type roster compiles in the cxx17 lane". The full type roster includes `sync.hpp`'s `SyncOwner/SyncRef/SyncMutRef` (`0400`) and `Arc/Mutex` (`0500`), so a topological executor could finalize the compat layer and certify "full type roster compiles under C++17" before `sync.hpp` existed. v08 adds `0400` and `0500` to `0800`'s `depends_on`. No cycle is introduced (nothing in the ancestry of `0400`/`0500` depends on `0800`), and the leaf TEST set and `0920` are unaffected because `0400`/`0500` are FUNC WPs. Scoping note: `0600` (Slice 6 behavioral extension of the already-covered `violation.hpp`) and `0700` (Slice 7 optional/environment-gated backend macros) add no new data type to the roster, so they are deliberately not added to `0800`.
- **Round 13 — Stage 2 validation (terminal):** an independent cross-model validator (cowork-opus-4.8/claude-opus-4-8, whose id differs from the Round 9 finalizer codex) re-derived convergence: re-audited all 44 WP blocks, the Slice 0 gate, the `0800` acceptance-vs-graph closure over all five type-producing FUNC WPs, the `0920` terminal gate over exactly the 27 leaf TEST WPs, strict-numeric-order acyclicity, full F1/F2/F3 coverage, and re-verified the CMake `WILL_FAIL` fact against live official docs. No substantive defect was found; the candidate was confirmed and the planning debate terminated. This file is the canonical, validated F5 deliverable.

---

## 2. Backlog summary (index)

| ID | Class | Title | Status | Depends on | Slice |
|----|-------|-------|--------|-----------|-------|
| CPP_MEMSAFE-0010-FUNC | FUNC | Violation policy core (`violation.hpp`) | not-started | — | 0 |
| CPP_MEMSAFE-0020-FUNC | FUNC | Backend detect & annotation macros (`backend.hpp`) | not-started | — | 0 |
| CPP_MEMSAFE-0030-FUNC | FUNC | Umbrella + config wiring (`memsafe.hpp`, `config.hpp`) | not-started | 0010, 0020 | 0 |
| CPP_MEMSAFE-0035-TEST | TEST | Shared F4-compatible test support | not-started | 0030 | cross |
| CPP_MEMSAFE-0040-TEST | TEST | Violation policy lanes (THROW + HANDLER) | not-started | 0010, 0035 | 0 |
| CPP_MEMSAFE-0050-TEST | TEST | ABORT-policy death test | not-started | 0010, 0035 | 0 |
| CPP_MEMSAFE-0060-TEST | TEST | Backend-macro & annotation compile-time checks | not-started | 0020, 0035 | 0 |
| CPP_MEMSAFE-0100-FUNC | FUNC | Owner/Ref/MutRef (`owner.hpp`) | not-started | 0010, 0020, 0030, 0035, 0040, 0050, 0060 (gate) | 1 |
| CPP_MEMSAFE-0110-TEST | TEST | Ownership unit tests | not-started | 0100 | 1 |
| CPP_MEMSAFE-0120-TEST | TEST | Owner static_assert (noexcept move, non-copy, nodiscard) | not-started | 0100 | 1 |
| CPP_MEMSAFE-0130-TEST | TEST | Owner/borrow compile-fail tests | not-started | 0100 | 1 |
| CPP_MEMSAFE-0140-TEST | TEST | Owner/Ref/MutRef property tests | not-started | 0100, 0035 | 1 |
| CPP_MEMSAFE-0150-TEST | TEST | Owner `std::vector` reallocation test | not-started | 0100 | 1 |
| CPP_MEMSAFE-0200-FUNC | FUNC | Slot/Handle generation core (`handle.hpp`) | not-started | 0010, 0020, 0030, 0035, 0040, 0050, 0060 (gate) | 2 |
| CPP_MEMSAFE-0205-FUNC | FUNC | SlotMap bounded Treiber allocator (`handle.hpp`) | not-started | 0200 | 2 |
| CPP_MEMSAFE-0210-TEST | TEST | SlotMap unit tests (alloc/UAF/bounds) | not-started | 0205 | 2 |
| CPP_MEMSAFE-0220-TEST | TEST | SlotMap property tests | not-started | 0205, 0035 | 2 |
| CPP_MEMSAFE-0225-TEST | TEST | Handle property tests (non-stateful) | not-started | 0200, 0035 | 2 |
| CPP_MEMSAFE-0230-TEST | TEST | SlotMap fuzz replay harness | not-started | 0205, 0035 | 2 |
| CPP_MEMSAFE-0240-TEST | TEST | SlotMap concurrency (Relacy) | not-started | 0205, 0035 | 2 |
| CPP_MEMSAFE-0300-FUNC | FUNC | Scope RAII arena (`scope.hpp`) | not-started | 0030 | 3 |
| CPP_MEMSAFE-0310-TEST | TEST | Scope lifetime unit tests | not-started | 0300 | 3 |
| CPP_MEMSAFE-0320-TEST | TEST | Scope use-after-scope sanitizer test | not-started | 0300, 0035 | 3 |
| CPP_MEMSAFE-0330-TEST | TEST | Scope property tests | not-started | 0300, 0035 | 3 |
| CPP_MEMSAFE-0400-FUNC | FUNC | SyncOwner/SyncRef/SyncMutRef (`sync.hpp`) | not-started | 0010, 0020, 0030, 0035, 0040, 0050, 0060 (gate), 0100 | 4 |
| CPP_MEMSAFE-0410-TEST | TEST | Sync ownership unit tests | not-started | 0400 | 4 |
| CPP_MEMSAFE-0420-TEST | TEST | Sync static_assert (noexcept move, nodiscard) | not-started | 0400 | 4 |
| CPP_MEMSAFE-0430-TEST | TEST | Sync concurrency (Relacy) | not-started | 0400, 0035 | 4 |
| CPP_MEMSAFE-0500-FUNC | FUNC | Arc/Mutex (`sync.hpp` additions) | not-started | 0400 | 5 |
| CPP_MEMSAFE-0510-TEST | TEST | Arc unit tests | not-started | 0500 | 5 |
| CPP_MEMSAFE-0520-TEST | TEST | Mutex unit tests | not-started | 0500 | 5 |
| CPP_MEMSAFE-0530-TEST | TEST | Arc/Mutex concurrency (Relacy) | not-started | 0500, 0035 | 5 |
| CPP_MEMSAFE-0535-TEST | TEST | Arc/Mutex property tests | not-started | 0500, 0035 | 5 |
| CPP_MEMSAFE-0540-TEST | TEST | Arc/Mutex fuzz replay harness | not-started | 0500, 0035 | 5 |
| CPP_MEMSAFE-0600-FUNC | FUNC | Advanced diagnostics (extend `violation.hpp`) | not-started | 0010, 0020, 0030, 0035, 0040, 0050, 0060 (gate) | 6 |
| CPP_MEMSAFE-0610-TEST | TEST | Advanced-diagnostics unit tests | not-started | 0600 | 6 |
| CPP_MEMSAFE-0700-FUNC | FUNC | Vendor backends + extra clang attrs (`backend.hpp`) | not-started | 0010, 0020, 0030, 0035, 0040, 0050, 0060 (gate) | 7 |
| CPP_MEMSAFE-0710-TEST | TEST | Clang co_await compile-fail (backend lane) | not-started | 0700, 0100 | 7 |
| CPP_MEMSAFE-0720-TEST | TEST | CHERI/Rusty-C++ backend lane (optional) | not-started | 0700 | 7 |
| CPP_MEMSAFE-0800-FUNC | FUNC | C++17 SFINAE compat layer (`config.hpp`) | not-started | 0030, 0100, 0205, 0300, 0400, 0500 | 8 |
| CPP_MEMSAFE-0810-TEST | TEST | C++17 (cxx17 lane) + Valgrind memcheck | not-started | 0800 | 8 |
| CPP_MEMSAFE-0900-TEST | TEST | Examples (smoke) under `examples/` | not-started | 0100, 0205, 0300 | 9 |
| CPP_MEMSAFE-0910-TEST | TEST | Performance benchmarks (<=5%) | not-started | 0100, 0205, 0400, 0500, 0035 | 9 |
| CPP_MEMSAFE-0920-TEST | TEST | Final `infra_lanes.json` lane wiring (terminal integration gate) | not-started | all leaf TEST WPs (0110…0910) | 9 |

---

## 3. Work packages

### Slice 0 — Foundational gate (violation policy, diagnostics, backend backbone)

```wp
id: CPP_MEMSAFE-0010-FUNC
class: FUNC
title: Violation policy core
status: not-started
review_rework_ledger: []
depends_on: []
traces_f1: Slice 0 (i) MEMSAFE_ON_VIOLATION; (ii) memsafe::violation type
traces_f2: Violation Policy & Diagnostics (cases 1-4)
traces_f3: subprogram On_Violation (Slice_Id 0); Violation_Policy property
inputs: F1 §Configuration Macros; F1 §Milestones Slice 0; smoke memsafe.hpp (reference impl)
artifacts:
  - include/memsafe/violation.hpp
  - symbols: MEMSAFE_VIOLATION_ABORT/_THROW/_HANDLER (constants); MEMSAFE_ON_VIOLATION (default ABORT);
    MEMSAFE_RELEASE_CHECKS (default 1); memsafe::violation_kind (enum); memsafe::violation_info (struct
    carrying kind, message, file, line, function); memsafe::violation (std::exception subclass);
    memsafe::set_violation_handler / get_violation_handler; memsafe::violation_handler typedef;
    detail::report_violation; MEMSAFE_DETAIL_VIOLATE(KIND,MSG) macro
acceptance:
  - Compiles clean under all four lanes (debug, release, asan-ubsan, cxx17) with -Wall -Wextra.
  - THROW lane: report_violation throws memsafe::violation carrying message + source location (file/line/func).
  - HANDLER lane with no handler installed falls back to abort (fail-safe).
  - release lane (MEMSAFE_RELEASE_CHECKS=0): violation machinery present but check call-sites compile away.
  - Validated by CPP_MEMSAFE-0040-TEST and CPP_MEMSAFE-0050-TEST.
notes: Mirror the smoke header's violation block; v1 may use __FILE__/__LINE__/__func__ where
  std::source_location is unavailable (cxx17 lane). source_location use is gated by MEMSAFE_CXX17_COMPAT.
```

```wp
id: CPP_MEMSAFE-0020-FUNC
class: FUNC
title: Backend detection & annotation macros
status: not-started
review_rework_ledger: []
depends_on: []
traces_f1: Slice 0 (iii) MEMSAFE_BACKEND_* autodetect; (iv) MEMSAFE_BORROWS / MEMSAFE_NODISCARD
traces_f2: Optional Back-End tests (macro expansion); Discardability
traces_f3: subprogram Backend_Detect (Slice_Id 0)
inputs: F1 §Configuration Macros and Back-Ends; smoke memsafe.hpp backend block
artifacts:
  - include/memsafe/backend.hpp
  - symbols: MEMSAFE_BACKEND_CLANG/_GCC/_MSVC (autodetect from __clang__/__GNUC__/_MSC_VER, manual
    override honored); MEMSAFE_NODISCARD ([[nodiscard]] always); MEMSAFE_BORROWS(x)
    ([[clang::lifetimebound]] + [[clang::coro_lifetimebound]] under CLANG, no-op otherwise);
    MEMSAFE_LIFETIMEBOUND
acceptance:
  - Exactly one MEMSAFE_BACKEND_* is active per compiler; manual pre-define suppresses autodetect.
  - MEMSAFE_NODISCARD expands to [[nodiscard]] on all compilers (no -Wattributes warning).
  - MEMSAFE_BORROWS(x) is a no-op (no diagnostic) when no backend / non-clang.
  - Validated by CPP_MEMSAFE-0060-TEST.
```

```wp
id: CPP_MEMSAFE-0030-FUNC
class: FUNC
title: Umbrella header & config wiring
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC]
traces_f1: §Non-Functional Requirements (Header-only; single umbrella includes six feature headers); Slice 8 compat switch plumbing
traces_f2: Compile-time static tests (umbrella includes cleanly under all lanes)
traces_f3: system Memory_Safe_Library (Header_Only, Cxx_Standard, modes)
inputs: F1 §NFR Header-only; C8 six-header roster
artifacts:
  - include/memsafe/memsafe.hpp  (umbrella; includes the six feature headers + config.hpp)
  - include/memsafe/config.hpp   (MEMSAFE_CXX17_COMPAT switch macro: concepts vs SFINAE selection;
    feature-test plumbing). Slice 8 (CPP_MEMSAFE-0800-FUNC) fills the SFINAE bodies.
acceptance:
  - #include <memsafe/memsafe.hpp> alone compiles in all four lanes.
  - Umbrella includes exactly the six feature headers (violation, backend, owner, handle, scope, sync) + config.
  - No .cpp files anywhere under include/ (header-only NFR).
notes: At Slice 0 the feature headers owner/handle/scope/sync may be empty stubs; later FUNC WPs fill them.
  Round 2 resolved Q4: this roster is the accepted six-feature-header split.
```

```wp
id: CPP_MEMSAFE-0035-TEST
class: TEST
title: Shared F4-compatible test support
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0030-FUNC]
traces_f1: N/A (testing support)
traces_f2: Shared support for violation death tests, sanitizer-negative tests, property thresholds (10 000 non-stateful sequences, 1 000 stateful sequences, 10x nightly), fuzz replay, Relacy concurrency, and chrono performance checks
traces_f3: N/A (testing support)
inputs: F2 §Test Categories; F4 testing/CMakeLists.txt and testing/infra_lanes.json
artifacts:
  - testing/tests/support/death_test.hpp
  - testing/tests/support/property_driver.hpp
  - testing/tests/support/fuzz_replay.hpp
  - testing/tests/support/chrono_bench.hpp
  - testing/tests/concurrency/vendor/relacy/README.md and vendored Relacy headers if license review accepts vendoring
  - testing/tests/test_support_smoke.cpp
acceptance:
  - test_support_smoke.cpp builds and exits 0 in the debug lane using only test_harness.hpp and the support headers.
  - death_test.hpp can re-spawn the current test executable as a child on POSIX and Windows, detect abnormal child termination, and return success from the parent process.
  - property_driver.hpp provides deterministic seeds, sequence-count thresholds (>=10 000 non-stateful, >=1 000 stateful, 10x under the nightly multiplier define), and minimal shrinking metadata without external linking.
  - fuzz_replay.hpp invokes byte-sequence fuzz entry points from a normal main() so fuzz WPs are exit-0 replay tests under F4; true libFuzzer remains optional future infra.
  - chrono_bench.hpp computes per-operation ratios and geometric means using std::chrono with enough warm-up and repetition controls for reviewable <=5% checks.
```

```wp
id: CPP_MEMSAFE-0040-TEST
class: TEST
title: Violation policy lanes — THROW + HANDLER
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 0 (i)(ii)
traces_f2: §Violation Policy & Diagnostics cases 1 (canonical names), 3 (throw), 4 (handler); DoD handler lane
traces_f3: On_Violation Violation_Policy => Throw/Handler
inputs: CPP_MEMSAFE-0010-FUNC artifacts
artifacts:
  - testing/tests/test_violation_throw.cpp   (#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW)
  - testing/tests/test_violation_handler.cpp (#define ... _HANDLER; installs deterministic test handler)
acceptance:
  - THROW: triggering a borrow-exclusivity and a generation-mismatch violation throws memsafe::violation;
    test asserts non-empty what() and populated source-location fields → exit 0.
  - HANDLER: installed handler receives violation_kind + message + source location for each representative
    violation class; test asserts captured values → exit 0.
  - Uses only testing/test_harness.hpp (CHECK / CHECK_THROWS); no external framework.
  - Only canonical policy names appear (no assert/terminate names).
```

```wp
id: CPP_MEMSAFE-0050-TEST
class: TEST
title: ABORT-policy death test
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 0 (i) default ABORT
traces_f2: §Violation Policy & Diagnostics case 2 (abort)
traces_f3: On_Violation Violation_Policy => Abort
inputs: CPP_MEMSAFE-0010-FUNC artifacts; death_test.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/test_violation_abort.cpp (#define ... _ABORT)
acceptance:
  - The parent test process re-spawns itself with a child-mode argv flag; the child triggers an invalid
    mutable borrow under ABORT and terminates abnormally.
  - POSIX builds use fork/exec or equivalent spawn plus waitpid status inspection; Windows builds use
    CreateProcess plus process wait and exit-code inspection.
  - Parent asserts the abnormal child result, emits any captured diagnostic context needed by the test, and
    exits 0. No F4 tool edit or WILL_FAIL runtime target is required.
```

```wp
id: CPP_MEMSAFE-0060-TEST
class: TEST
title: Backend-macro & annotation compile-time checks
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 0 (iii)(iv)
traces_f2: Compile-time static tests; Optional Back-End (macro expansion)
traces_f3: Backend_Detect
inputs: CPP_MEMSAFE-0020-FUNC artifacts
artifacts:
  - testing/tests/test_backend_macros.cpp (static_assert exactly one backend active; MEMSAFE_NODISCARD usable)
  - testing/tests/compile_fail/cf_nodiscard_type.cpp (a [[nodiscard]] return discarded → -Wunused-result -Werror → WILL_FAIL)
acceptance:
  - test_backend_macros.cpp exits 0; asserts macro expansions per active compiler.
  - cf_nodiscard_type.cpp fails to compile under -Werror -Wunused-result (validates MEMSAFE_NODISCARD by-value discard, per E6).
```

### Slice 1 — Owner / Ref / MutRef

```wp
id: CPP_MEMSAFE-0100-FUNC
class: FUNC
title: Owner / Ref / MutRef
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0035-TEST, CPP_MEMSAFE-0040-TEST, CPP_MEMSAFE-0050-TEST, CPP_MEMSAFE-0060-TEST]   # Slice 0 gate
traces_f1: Slice 1; §Ownership Types (Owner<T>, Ref<T>, MutRef<T>)
traces_f2: §Ownership (construction, borrow rules, move, discardability)
traces_f3: data Owner / Ref / MutRef (Slice_Id 1); Move_Noexcept, Nodiscard, Borrow_Counted, Co_Await_Prohibited
inputs: F1 §Ownership Types; smoke memsafe.hpp Slice 1 block; CPP_MEMSAFE-0010/0020 macros
artifacts:
  - include/memsafe/owner.hpp
  - symbols: memsafe::Owner<T> (move-only; unconditionally noexcept move ctor/assign; borrow(), borrow_mut(),
    has_value(), operator*/->); memsafe::Ref<T> (MEMSAFE_NODISCARD, copyable, borrow-counted, const deref);
    memsafe::MutRef<T> (MEMSAFE_NODISCARD, non-copyable, exclusive deref); detail::borrow_ctrl
acceptance:
  - borrow_mut() while any borrow exists → borrow_exclusivity violation (Debug_Checks); checks compile away in release.
  - Move ctor/assign are noexcept(true); Owner non-copyable; MutRef non-copyable.
  - Ref/MutRef returned BY VALUE so type-level [[nodiscard]] fires on discard (E6, Q3).
  - MEMSAFE_BORROWS annotation applied to Ref/MutRef for the Slice 7 co_await lane.
  - Validated by 0110/0120/0130/0140/0150.
```

```wp
id: CPP_MEMSAFE-0110-TEST
class: TEST
title: Ownership unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC]
traces_f1: Slice 1
traces_f2: §Ownership cases 1 (construct/destruct), 2 (borrow rules)
traces_f3: Owner/Ref/MutRef
inputs: F1 Slice 1 ownership rules; F2 ownership cases 1-2; CPP_MEMSAFE-0100-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_ownership.cpp (THROW policy in-process oracle for the negative borrow case)
acceptance:
  - Construct Owner<int>, has_value() true; destructor runs once.
  - Two Ref reads coexist; borrow_mut() with a Ref live → violation observed; after Refs drop, borrow_mut() mutates.
  - exit 0; uses test_harness.hpp.
```

```wp
id: CPP_MEMSAFE-0120-TEST
class: TEST
title: Owner static_assert checks
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC]
traces_f1: §Acceptance Criteria (static_assert noexcept move, nodiscard types)
traces_f2: §Compile-time static tests; DoD static_asserts
traces_f3: Move_Noexcept, Nodiscard, Copyable=false
inputs: F1 static-assert acceptance criteria; F2 compile-time static tests; CPP_MEMSAFE-0100-FUNC artifacts
artifacts:
  - testing/tests/test_owner_static_asserts.cpp
acceptance:
  - static_assert(std::is_nothrow_move_constructible_v<Owner<int>>) and ..._move_assignable.
  - static_assert(!std::is_copy_constructible_v<Owner<int>>), !copy_constructible<MutRef<int>>.
  - static_assert nodiscard intent documented (type-level [[nodiscard]] verified behaviorally by 0130).
  - Translation unit compiles (exit 0).
```

```wp
id: CPP_MEMSAFE-0130-TEST
class: TEST
title: Owner / borrow compile-fail tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC]
traces_f1: §Discardability; move-only
traces_f2: §Ownership case 4 (discardability compile-fail)
traces_f3: Nodiscard, Copyable=false
inputs: F1 discardability and move-only requirements; F2 ownership case 4; CPP_MEMSAFE-0100-FUNC artifacts; F4 compile-fail driver
artifacts:
  - testing/tests/compile_fail/cf_copy_owner.cpp     (copy a move-only Owner → WILL_FAIL)
  - testing/tests/compile_fail/cf_discard_borrow.cpp (discard borrow() result under -Wunused-result → WILL_FAIL)
  - testing/tests/compile_fail/cf_copy_mutref.cpp    (copy a MutRef → WILL_FAIL)
acceptance:
  - All three fail to compile under -Werror (-Wunused-result for the discard case). Each is a WILL_FAIL CTest.
```

```wp
id: CPP_MEMSAFE-0140-TEST
class: TEST
title: Owner/Ref/MutRef property tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 1
traces_f2: §Property-based tests (>=10 000 sequences/CI run per non-stateful type; 10x nightly)
traces_f3: Owner/Ref/MutRef
inputs: F2 property-based threshold for Owner/Ref/MutRef; CPP_MEMSAFE-0100-FUNC artifacts; property_driver.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/property/test_owner_property.cpp
acceptance:
  - Generates random construct/borrow/move/drop sequences with property_driver.hpp; asserts the borrow invariant after each step; >=10 000 sequences/run in the property lane and 10x when the nightly multiplier define is set; records the seed and minimal shrunk prefix on failure. exit 0 = pass.
```

```wp
id: CPP_MEMSAFE-0150-TEST
class: TEST
title: Owner std::vector reallocation test
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC]
traces_f1: §Move semantics ("moves rather than copies" under reallocation)
traces_f2: §Ownership case 3 (vector reallocation)
traces_f3: Move_Noexcept
inputs: F1 move-semantics acceptance; F2 ownership vector-reallocation case; CPP_MEMSAFE-0100-FUNC artifacts; F4 runtime test lane
artifacts:
  - testing/tests/test_owner_vector_realloc.cpp
acceptance:
  - Insert Owner<T> into std::vector, force reallocation (reserve growth); program compiles & runs (a copy
    would fail to compile for the move-only type) → exit 0.
  - Asserts is_nothrow_move_constructible_v<Owner<T>>; per Q2/E5 the oracle is "compiles + nothrow-move",
    not "ill-formed if not noexcept".
```

### Slice 2 — Slot / Handle / SlotMap

```wp
id: CPP_MEMSAFE-0200-FUNC
class: FUNC
title: Slot / Handle generation core
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0035-TEST, CPP_MEMSAFE-0040-TEST, CPP_MEMSAFE-0050-TEST, CPP_MEMSAFE-0060-TEST]   # Slice 0 gate
traces_f1: Slice 2; §Handle and Slot Types (`Slot<T>`, `Handle<T>`)
traces_f2: §Generational Handles cases 2 (UAF) and 3 (validity primitives)
traces_f3: data Slot/Handle (Slice_Id 2); Atomic_Generation; Generation_Checked
inputs: F1 §Handle and Slot Types; F3 Slot and Handle components; CPP_MEMSAFE-0010 violation policy
artifacts:
  - include/memsafe/handle.hpp
  - symbols: memsafe::Slot<T> (payload storage plus per-instance std::atomic<uint32_t> generation,
    generation bump primitive used by SlotMap); memsafe::Handle<T> (MEMSAFE_NODISCARD; index plus remembered
    generation; default-invalid sentinel; index() and generation() observers; bool is_valid() only when paired
    with the owning SlotMap context)
acceptance:
  - Slot generation starts at a nonzero value and increments atomically on the exposed bump primitive.
  - Default Handle is never valid and cannot accidentally compare equal to a real allocated slot.
  - Handle is [[nodiscard]], trivially copyable or cheap-copy by design, and carries only index plus remembered generation.
  - No allocator, free-list, capacity management, or Treiber-stack logic is implemented in this WP; those are isolated in 0205.
  - Validated by 0225 (Handle non-stateful properties) and indirectly by 0210/0220/0230/0240 after 0205 wires the owning SlotMap context.
notes: This split resolves Q6 by making generation primitives reviewable without also reviewing lock-free allocation.
```

```wp
id: CPP_MEMSAFE-0205-FUNC
class: FUNC
title: SlotMap bounded Treiber allocator
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0200-FUNC]
traces_f1: Slice 2; §Handle and Slot Types (`SlotMap<T>` bounded allocator)
traces_f2: §Generational Handles cases 1 (alloc/dealloc/reuse/gen bump), 2 (UAF), 3 (bounded capacity), 4 (lock-free contention)
traces_f3: data SlotMap (Slice_Id 2); Lock_Free, Bounded_Capacity, Atomic_Generation, Treiber_Stack, Slot_Capacity
inputs: F1 §Handle and Slot Types; F1[5]/E7 Treiber stack; F3 SlotMap component; CPP_MEMSAFE-0200-FUNC artifacts
artifacts:
  - include/memsafe/handle.hpp (additions)
  - symbols: memsafe::SlotMap<T> (fixed capacity; allocate(args...) [MEMSAFE_NODISCARD] via CAS-pop from a
    packed head-index plus ABA-counter Treiber-stack free list; deallocate(Handle<T>) increments slot generation
    and pushes the index; get(Handle<T>); capacity(); size())
acceptance:
  - Per-slot atomic generation is used; no global mutex and no global generation counter are introduced.
  - allocate() beyond capacity triggers a capacity_exhausted violation; deref/get after deallocate triggers a use_after_free violation through the configured policy.
  - Deallocate then allocate reuses the freed slot, increments generation, and leaves stale handles rejected.
  - Free-list pop/push uses one packed-head compare-and-swap path with ABA counter, matching F1 and F3.
  - Validated by 0210/0220/0230/0240.
```

```wp
id: CPP_MEMSAFE-0210-TEST
class: TEST
title: SlotMap unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0205-FUNC]
traces_f1: Slice 2
traces_f2: §Generational Handles cases 1 (alloc/dealloc/reuse/gen bump), 2 (UAF), 3 (bounded capacity)
traces_f3: SlotMap/Handle
inputs: F1 Slice 2 generational handle requirements; F2 generational-handle cases 1-3; CPP_MEMSAFE-0205-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_slotmap.cpp (THROW policy oracle for negative cases)
acceptance:
  - Allocate N, deallocate one, reallocate → slot reused, generation incremented; stale handle deref → violation;
    over-capacity allocate → capacity_exhausted. exit 0.
```

```wp
id: CPP_MEMSAFE-0220-TEST
class: TEST
title: SlotMap property tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 2
traces_f2: §Property-based tests (>=1 000 sequences/CI run per stateful type; 10x nightly)
traces_f3: SlotMap
inputs: F2 stateful SlotMap property threshold; CPP_MEMSAFE-0205-FUNC artifacts; property_driver.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/property/test_slotmap_property.cpp
acceptance:
  - >=1 000 random alloc/dealloc/deref sequences/run via property_driver.hpp; invariant: live handles deref OK, stale handles always detected; no leaked or double-freed slots; seed and shrunk prefix reported on failure. exit 0.
```

```wp
id: CPP_MEMSAFE-0225-TEST
class: TEST
title: Handle property tests (non-stateful)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0200-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 2; §Handle and Slot Types (`Handle<T>`)
traces_f2: §Property-based tests (>=10 000 sequences/CI run per non-stateful type, Handle<T>; 10x nightly)
traces_f3: data Handle (Slice_Id 2); Nodiscard, Generation_Checked
inputs: CPP_MEMSAFE-0200-FUNC artifacts; property_driver.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/property/test_handle_property.cpp
acceptance:
  - Treats Handle<T> as the F2 non-stateful type: generates >=10 000 random (index, generation) handle values/run via property_driver.hpp (10x under the nightly multiplier define) and asserts non-stateful invariants without a live SlotMap: the default-constructed Handle is never valid; index()/generation() observers round-trip the constructed components; copies compare equal and are independently usable; two handles with distinct (index, generation) never compare equal; a default Handle never equals an allocated-style handle.
  - Records the seed and minimal shrunk counterexample on failure. exit 0 = pass.
notes: Added in Round 4 to resolve Q10. F2 lists Handle<T> among the non-stateful property types (10 000 seq/run); v03 only exercised Handle inside the stateful SlotMap property test (0220, 1 000 seq/run), which did not meet the non-stateful threshold. This WP depends on 0200 only (generation primitives), not on 0205, so it can run as soon as the Slot/Handle core is finalized.
```

```wp
id: CPP_MEMSAFE-0230-TEST
class: TEST
title: SlotMap fuzz replay harness
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 2
traces_f2: §Fuzz tests (>=5 min/change; 24 CPU-h nightly)
traces_f3: SlotMap
inputs: F2 fuzz-test budget and replay expectations; CPP_MEMSAFE-0205-FUNC artifacts; fuzz_replay.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/fuzz/fuzz_slotmap_replay.cpp
acceptance:
  - Provides the same byte-driven state machine shape as a libFuzzer target but wraps it in a deterministic replay main() using fuzz_replay.hpp; runs built-in seeds plus any checked-in corpus bytes in the fuzz lane; under sanitizer lanes it must not produce unexpected ASan or UBSan reports. True timed libFuzzer execution is documented as optional future infrastructure, not a required F4 drop-in artifact.
```

```wp
id: CPP_MEMSAFE-0240-TEST
class: TEST
title: SlotMap concurrency (Relacy)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 2
traces_f2: §Concurrency tests; §Generational Handles case 4 (lock-free under contention)
traces_f3: SlotMap Treiber_Stack; Lock_Free
inputs: F2 concurrency and generational handle contention cases; CPP_MEMSAFE-0205-FUNC artifacts; vendored Relacy/support headers from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/concurrency/test_slotmap_concurrency.cpp
acceptance:
  - Relacy exhausts <=2-thread interleavings of the Treiber CAS pop/push pair; no data race, generations monotonic per slot, no ABA. Relacy is consumed as vendored test code under testing/tests/concurrency/vendor/ and the test exits 0 on success.
```

### Slice 3 — Scope

```wp
id: CPP_MEMSAFE-0300-FUNC
class: FUNC
title: Scope RAII arena
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0030-FUNC]
traces_f1: Slice 3; §Scope and Region (Scope)
traces_f2: §Scope and Concurrency case 1
traces_f3: data Scope (Slice_Id 3); RAII_Scoped; Create_In_Scope returns raw T*
inputs: F1 §Scope; smoke Slice 3 block
artifacts:
  - include/memsafe/scope.hpp
  - symbols: memsafe::Scope (non-copyable; create<T>(args...) → raw T* owned by the arena; destroys all
    created objects in reverse order on scope exit; size())
acceptance:
  - All created objects destroyed exactly once on scope exit (reverse order).
  - v1 create() returns a raw T* (unchecked) — post-scope access is a sanitizer-only oracle (F2), NOT a
    MEMSAFE_ON_VIOLATION path. No violation dependency (F3: Scope has no needs_violation).
  - Validated by 0310/0320/0330.
```

```wp
id: CPP_MEMSAFE-0310-TEST
class: TEST
title: Scope lifetime unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0300-FUNC]
traces_f1: Slice 3
traces_f2: §Scope and Concurrency case 1 (lifetime, destructor-once)
traces_f3: Scope
inputs: F1 Slice 3 Scope lifetime requirements; F2 scope lifetime case 1; CPP_MEMSAFE-0300-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_scope.cpp
acceptance:
  - Create several objects, all accessible within scope; on destruction each destructor runs exactly once
    (instrumented counter). exit 0.
```

```wp
id: CPP_MEMSAFE-0320-TEST
class: TEST
title: Scope use-after-scope sanitizer test
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0300-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 3
traces_f2: §Scope case 1 (sanitizer/death-test only; ASan/HWASan/Valgrind oracle; UBSan not sufficient)
traces_f3: Scope (raw T* UAF)
inputs: F2 scope sanitizer/death-test oracle; CPP_MEMSAFE-0300-FUNC artifacts; death_test.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/test_scope_uaf_sanitizer.cpp
acceptance:
  - Parent process uses death_test.hpp to re-spawn a child that preserves a raw pointer from create(), destroys the scope, then accesses the pointer under the asan-ubsan or valgrind-cxx17 lane. The parent expects the child to terminate with the sanitizer or memcheck error code and exits 0 when the expected diagnostic path occurs. Non-sanitized lanes must not assert MEMSAFE_ON_VIOLATION for this raw-pointer access.
```

```wp
id: CPP_MEMSAFE-0330-TEST
class: TEST
title: Scope property tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0300-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 3
traces_f2: §Property-based tests (>=1 000 sequences/run, stateful Scope)
traces_f3: Scope
inputs: F2 stateful Scope property threshold; CPP_MEMSAFE-0300-FUNC artifacts; property_driver.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/property/test_scope_property.cpp
acceptance:
  - >=1 000 random create/destroy-scope sequences via property_driver.hpp; invariant: destructor count equals constructor count and reverse destruction order is preserved; seed and shrunk prefix reported on failure. exit 0.
```

### Slice 4 — SyncOwner / SyncRef / SyncMutRef

```wp
id: CPP_MEMSAFE-0400-FUNC
class: FUNC
title: SyncOwner / SyncRef / SyncMutRef
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0035-TEST, CPP_MEMSAFE-0040-TEST, CPP_MEMSAFE-0050-TEST, CPP_MEMSAFE-0060-TEST, CPP_MEMSAFE-0100-FUNC]   # Slice 0 gate + reuses Owner patterns
traces_f1: Slice 4; §Ownership Types (Sync* variants)
traces_f2: §Scope and Concurrency case 2 (SyncOwner across threads)
traces_f3: data SyncOwner/SyncRef/SyncMutRef (Slice_Id 4); Thread_Safe; Atomic_Refcount
inputs: F1 §Ownership Types Sync* rows; CPP_MEMSAFE-0100 patterns
artifacts:
  - include/memsafe/sync.hpp
  - symbols: memsafe::SyncOwner<T> (atomic borrow counters; noexcept move; borrow()/borrow_mut());
    memsafe::SyncRef<T> (MEMSAFE_NODISCARD, copyable, atomic shared count); memsafe::SyncMutRef<T>
    (MEMSAFE_NODISCARD, non-copyable, atomic exclusive token)
acceptance:
  - Atomic counters allow cross-thread borrow accounting; SyncOwner noexcept-move; SyncRef/SyncMutRef nodiscard.
  - Validated by 0410/0420/0430.
```

```wp
id: CPP_MEMSAFE-0410-TEST
class: TEST
title: Sync ownership unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0400-FUNC]
traces_f1: Slice 4
traces_f2: §Scope and Concurrency case 2
traces_f3: SyncOwner/SyncRef/SyncMutRef
inputs: F1 Slice 4 sync-owner requirements; F2 scope/concurrency case 2; CPP_MEMSAFE-0400-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_sync_ownership.cpp
acceptance:
  - Single-thread functional parity with Owner; two-thread shared-borrow / exclusive-borrow accounting correct
    when used properly. exit 0.
```

```wp
id: CPP_MEMSAFE-0420-TEST
class: TEST
title: Sync static_assert checks
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0400-FUNC]
traces_f1: §Acceptance Criteria (noexcept move on SyncOwner; nodiscard)
traces_f2: §Compile-time static tests
traces_f3: Move_Noexcept; Nodiscard
inputs: F1 SyncOwner noexcept/nodiscard acceptance criteria; F2 compile-time static tests; CPP_MEMSAFE-0400-FUNC artifacts
artifacts:
  - testing/tests/test_sync_static_asserts.cpp
acceptance:
  - static_assert(is_nothrow_move_constructible_v<SyncOwner<int>>); SyncRef/SyncMutRef nodiscard; SyncMutRef
    non-copyable. Compiles → exit 0.
```

```wp
id: CPP_MEMSAFE-0430-TEST
class: TEST
title: Sync concurrency (Relacy)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0400-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 4
traces_f2: §Concurrency tests (Relacy, <=2-thread interleavings, checks enabled)
traces_f3: SyncOwner Atomic_Refcount
inputs: F2 Relacy concurrency tests for sync ownership; CPP_MEMSAFE-0400-FUNC artifacts; vendored Relacy/support headers from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/concurrency/test_sync_concurrency.cpp
acceptance:
  - Relacy exhausts 2-thread borrow/borrow_mut interleavings; atomic counters race-free. Uses vendored Relacy test headers from CPP_MEMSAFE-0035-TEST and exits 0.
```

### Slice 5 — Arc / Mutex

```wp
id: CPP_MEMSAFE-0500-FUNC
class: FUNC
title: Arc / Mutex
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0400-FUNC]
traces_f1: Slice 5; §Concurrency Types (Arc<T>, Mutex<T>)
traces_f2: §Scope and Concurrency case 3 (Mutex)
traces_f3: data Arc/Mutex (Slice_Id 5); Atomic_Refcount / Mutex_Guarded
inputs: F1 §Concurrency Types
artifacts:
  - include/memsafe/sync.hpp (additions): memsafe::Arc<T> (atomic strong refcount, shared deref);
    memsafe::Mutex<T> (wraps T; lock() [MEMSAFE_NODISCARD] → MutRef<T>; unlock decrements borrow counter)
acceptance:
  - Arc copies/destroys adjust the atomic count; payload destroyed at count 0.
  - Mutex::lock() yields an exclusive MutRef; concurrent increments serialize correctly.
  - Validated by 0510/0520/0530/0535/0540.
notes: Q4 resolved to keep Arc/Mutex additions in sync.hpp; do not introduce concurrency.hpp in v1.
```

```wp
id: CPP_MEMSAFE-0510-TEST
class: TEST
title: Arc unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0500-FUNC]
traces_f1: Slice 5
traces_f2: §Unit tests (Arc)
traces_f3: Arc
inputs: F1 Slice 5 Arc requirements; F2 Arc unit tests; CPP_MEMSAFE-0500-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_arc.cpp
acceptance:
  - Shared ownership across copies; refcount correct; payload destroyed once at last release. exit 0.
```

```wp
id: CPP_MEMSAFE-0520-TEST
class: TEST
title: Mutex unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0500-FUNC]
traces_f1: Slice 5
traces_f2: §Scope and Concurrency case 3 (Mutex)
traces_f3: Mutex
inputs: F1 Slice 5 Mutex requirements; F2 scope/concurrency case 3; CPP_MEMSAFE-0500-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_mutex.cpp
acceptance:
  - Two threads increment a Mutex<int> through lock(); final value correct; no lost updates. exit 0.
```

```wp
id: CPP_MEMSAFE-0530-TEST
class: TEST
title: Arc/Mutex concurrency (Relacy)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0500-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 5
traces_f2: §Concurrency tests (Relacy)
traces_f3: Arc Atomic_Refcount; Mutex Mutex_Guarded
inputs: F2 Relacy concurrency tests for Arc/Mutex; CPP_MEMSAFE-0500-FUNC artifacts; vendored Relacy/support headers from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/concurrency/test_arc_mutex_concurrency.cpp
acceptance:
  - Relacy exhausts 2-thread Arc clone/drop and Mutex lock/unlock interleavings; refcount race-free, mutual exclusion holds. Uses vendored Relacy test headers from CPP_MEMSAFE-0035-TEST and exits 0.
```

```wp
id: CPP_MEMSAFE-0535-TEST
class: TEST
title: Arc/Mutex property tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0500-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 5; §Concurrency Types (Arc<T>, Mutex<T>)
traces_f2: §Property-based tests (>=1 000 sequences/CI run per stateful type, Arc<T> and Mutex<T>; 10x nightly)
traces_f3: data Arc/Mutex (Slice_Id 5); Atomic_Refcount / Mutex_Guarded
inputs: CPP_MEMSAFE-0500-FUNC artifacts; property_driver.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/property/test_arc_property.cpp
  - testing/tests/property/test_mutex_property.cpp
acceptance:
  - Arc: >=1 000 random clone/drop/deref sequences/run (single-threaded sequential model) via property_driver.hpp; invariant: the strong count equals the number of live Arc copies at every step and the payload is destroyed exactly once when the count reaches zero. 10x under the nightly multiplier define.
  - Mutex: >=1 000 random lock/mutate/unlock sequences/run via property_driver.hpp; invariant: a MutRef is obtainable only when the Mutex is unlocked, the guarded value reflects every applied mutation, and the borrow counter returns to zero after each guard is dropped. 10x under the nightly multiplier define.
  - Both files record the seed and minimal shrunk prefix on failure. exit 0 = pass.
notes: Added in Round 4 to resolve Q10. F2 lists Arc<T> and Mutex<T> among the stateful property types (1 000 seq/run each), and the Slice 5 Definition of Done inherits that threshold; v03 covered Arc/Mutex only through unit (0510/0520), concurrency (0530), and fuzz (0540) WPs, none of which is the F2 property-based category. These are sequential property tests; cross-thread interleavings remain the responsibility of the Relacy concurrency WP 0530.
```

```wp
id: CPP_MEMSAFE-0540-TEST
class: TEST
title: Arc/Mutex fuzz replay harness
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0500-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: Slice 5
traces_f2: §Fuzz tests (stateful Arc, Mutex)
traces_f3: Arc/Mutex
inputs: F2 fuzz tests for stateful Arc/Mutex operations; CPP_MEMSAFE-0500-FUNC artifacts; fuzz_replay.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/fuzz/fuzz_arc_mutex_replay.cpp
acceptance:
  - State-machine fuzz replay of clone/drop/lock/unlock using fuzz_replay.hpp under the fuzz and sanitizer lanes; same optional true-libFuzzer note as 0230.
```

### Slice 6 — Advanced diagnostics

```wp
id: CPP_MEMSAFE-0600-FUNC
class: FUNC
title: Advanced diagnostics
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0035-TEST, CPP_MEMSAFE-0040-TEST, CPP_MEMSAFE-0050-TEST, CPP_MEMSAFE-0060-TEST]   # Slice 0 gate
traces_f1: Slice 6 (richer handler payloads, structured borrow-chain diagnostics, logging-framework hooks)
traces_f2: §Violation Policy & Diagnostics (handler payload richness)
traces_f3: Slice_Advanced_Diag (Slice_Id 6) requires On_Violation
inputs: F1 §Milestones Slice 6; CPP_MEMSAFE-0010 violation backbone
artifacts:
  - include/memsafe/violation.hpp (extensions): richer violation_info payload for MEMSAFE_VIOLATION_HANDLER;
    structured borrow-check call-chain diagnostic; optional logging-framework integration hook
acceptance:
  - Handler receives the extended payload; existing Slice 0 ABI/behavior unchanged (no regression in 0040).
  - Compiles all lanes. Validated by 0610.
```

```wp
id: CPP_MEMSAFE-0610-TEST
class: TEST
title: Advanced-diagnostics unit tests
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0600-FUNC]
traces_f1: Slice 6
traces_f2: §Violation Policy & Diagnostics (handler case, extended)
traces_f3: Slice_Advanced_Diag
inputs: F1 Slice 6 advanced diagnostics requirements; F2 extended violation diagnostics cases; CPP_MEMSAFE-0600-FUNC artifacts; F4 testing/test_harness.hpp
artifacts:
  - testing/tests/test_advanced_diag.cpp
acceptance:
  - HANDLER lane: extended payload fields populated and asserted; borrow-chain diagnostic present. exit 0.
```

### Slice 7 — Optional vendor backends

```wp
id: CPP_MEMSAFE-0700-FUNC
class: FUNC
title: Vendor backends + extra Clang attributes
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0010-FUNC, CPP_MEMSAFE-0020-FUNC, CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0035-TEST, CPP_MEMSAFE-0040-TEST, CPP_MEMSAFE-0050-TEST, CPP_MEMSAFE-0060-TEST]   # Slice 0 gate
traces_f1: Slice 7 (MEMSAFE_BACKEND_RUSTY_CPP, _CHERI; clang [[clang::owner]]/[[clang::pointer]] beyond lifetimebound)
traces_f2: §Optional Back-End tests
traces_f3: Slice_Vendor_Backends (Slice_Id 7) requires Backend_Detect; Backend enum (Rusty_Cpp, CHERI)
inputs: F1 §Milestones Slice 7; CPP_MEMSAFE-0020 backend macros
artifacts:
  - include/memsafe/backend.hpp (extensions): MEMSAFE_BACKEND_RUSTY_CPP, MEMSAFE_BACKEND_CHERI; MEMSAFE_OWNED,
    MEMSAFE_POINTER, MEMSAFE_LOCK_HELD(m); extra clang annotations
acceptance:
  - Each new backend macro expands to vendor attributes when enabled, no-op otherwise; default build unaffected.
  - Validated by 0710/0720.
```

```wp
id: CPP_MEMSAFE-0710-TEST
class: TEST
title: Clang co_await compile-fail (backend lane)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0700-FUNC, CPP_MEMSAFE-0100-FUNC]
traces_f1: Slice 7; §Ownership Types (no Ref/MutRef across co_await; enforced via MEMSAFE_BORROWS under CLANG)
traces_f2: §Ownership case 5 (co_await prohibition, backend lane); §Optional Back-End
traces_f3: Co_Await_Prohibited
inputs: F1 Slice 7 backend borrow annotations and co_await prohibition; F2 optional back-end ownership case 5; CPP_MEMSAFE-0700-FUNC and CPP_MEMSAFE-0100-FUNC artifacts; F4 compile-fail driver
artifacts:
  - testing/tests/compile_fail/cf_ref_across_coawait.cpp (MEMSAFE_BACKEND_CLANG; holds Ref across co_await → WILL_FAIL)
acceptance:
  - Under clang with MEMSAFE_BACKEND_CLANG, holding a Ref/MutRef across a co_await is rejected by
    lifetimebound/coro_lifetimebound analysis → compile fails (WILL_FAIL).
  - Note: requires a clang lane with the backend define; gated to clang (skipped where the active compiler is not clang).
```

```wp
id: CPP_MEMSAFE-0720-TEST
class: TEST
title: CHERI / Rusty-C++ backend lane (optional)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0700-FUNC]
traces_f1: Slice 7
traces_f2: §Optional Back-End and Sanitizer Tests (CHERI capability violations = failure)
traces_f3: Backend CHERI / Rusty_Cpp
inputs: F1 Slice 7 CHERI/Rusty-C++ backend requirements; F2 optional back-end sanitizer tests; CPP_MEMSAFE-0700-FUNC artifacts; F4 optional backend lane
artifacts:
  - testing/tests/test_backend_cheri.cpp (compiled/run only on a CHERI-capable system; otherwise skipped)
acceptance:
  - With MEMSAFE_BACKEND_CHERI on a capable system, the suite runs with no new false positives; any capability
    violation fails. Marked optional/environment-gated in infra_lanes.json.
```

### Slice 8 — C++17 compatibility

```wp
id: CPP_MEMSAFE-0800-FUNC
class: FUNC
title: C++17 SFINAE compatibility layer
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0030-FUNC, CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0300-FUNC, CPP_MEMSAFE-0400-FUNC, CPP_MEMSAFE-0500-FUNC]
traces_f1: Slice 8 (replace concepts with SFINAE; drop std::source_location under MEMSAFE_CXX17_COMPAT)
traces_f2: §Test Environment (C++17 mode tested separately; Valgrind for the C++17 subset)
traces_f3: Slice_Cxx17_Compat (Cxx_Standard => Cxx17)
inputs: F1 §Configuration Macros (MEMSAFE_CXX17_COMPAT); config.hpp from 0030; the full feature-header type roster from 0100 (owner.hpp), 0205 (handle.hpp), 0300 (scope.hpp), 0400 + 0500 (sync.hpp)
artifacts:
  - include/memsafe/config.hpp (SFINAE bodies): concept-vs-SFINAE selection so all feature headers compile
    under -std=c++17 with MEMSAFE_CXX17_COMPAT=1; source_location replaced by __FILE__/__LINE__/__func__
acceptance:
  - Full type roster compiles and the unit tests pass in the cxx17 lane (std=17, MEMSAFE_CXX17_COMPAT=1).
  - No concepts / std::source_location reachable when the compat switch is on. Validated by 0810.
notes: Round 8 (Q15) added 0400 and 0500 to depends_on so the "full type roster compiles under C++17"
  acceptance cannot be reached before sync.hpp (SyncOwner/SyncRef/SyncMutRef, Arc/Mutex) exists, matching the
  acceptance-vs-graph rule (§1.5). 0600 (behavioral extension of the already-covered violation.hpp) and 0700
  (optional/environment-gated backend macros) add no new data type to the roster and are intentionally excluded.
```

```wp
id: CPP_MEMSAFE-0810-TEST
class: TEST
title: C++17 lane + Valgrind memcheck
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0800-FUNC]
traces_f1: Slice 8
traces_f2: §Sanitizer matrix (Valgrind memcheck for the C++17 subset, Linux only); cxx17 lane
traces_f3: Cxx17_Compat
inputs: F1 Slice 8 C++17 compatibility requirements; F2 C++17 and Valgrind memcheck lanes; CPP_MEMSAFE-0800-FUNC artifacts; F4 cxx17 and valgrind-cxx17 lanes
artifacts:
  - testing/tests/test_cxx17_smoke.cpp
acceptance:
  - Builds & passes in the cxx17 lane; Valgrind memcheck (Linux) reports no errors on the C++17 subset.
```

### Slice 9 — Tooling, examples, performance, lane wiring

```wp
id: CPP_MEMSAFE-0900-TEST
class: TEST
title: Examples (smoke)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0300-FUNC]
traces_f1: Slice 9 (examples)
traces_f2: examples/*.cpp smoke (exit 0)
traces_f3: Owner/SlotMap/Scope
inputs: F1 Slice 9 examples requirement; CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0205-FUNC, and CPP_MEMSAFE-0300-FUNC artifacts; F4 examples discovery
artifacts:
  - examples/ex_owner_borrow.cpp
  - examples/ex_slotmap_handle.cpp
  - examples/ex_scope.cpp
acceptance:
  - Each example is a self-contained main() demonstrating the API; built & run as a smoke test; exit 0.
```

```wp
id: CPP_MEMSAFE-0910-TEST
class: TEST
title: Performance benchmarks (<=5%)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0400-FUNC, CPP_MEMSAFE-0500-FUNC, CPP_MEMSAFE-0035-TEST]
traces_f1: §NFR Performance; §Acceptance Criteria (<=5% overhead)
traces_f2: §Performance benchmarks (google/benchmark; <=5% geomean slowdown in release/no-checks)
traces_f3: (NFR; release mode)
inputs: F1 performance NFR; F2 performance benchmark threshold; CPP_MEMSAFE-0100-FUNC, CPP_MEMSAFE-0205-FUNC, CPP_MEMSAFE-0400-FUNC, CPP_MEMSAFE-0500-FUNC artifacts; chrono_bench.hpp from CPP_MEMSAFE-0035-TEST
artifacts:
  - testing/tests/perf/bench_primitives.cpp (Owner vs unique_ptr; Handle/SlotMap vs raw; Arc vs shared_ptr; Mutex vs std::mutex)
acceptance:
  - In the release lane (MEMSAFE_RELEASE_CHECKS=0), geometric-mean slowdown <=5% vs raw/unique_ptr/shared_ptr.
  - Because F4 has no external-library link step, the required drop-in form uses chrono_bench.hpp and std::chrono rather than google/benchmark. It exits non-zero if any required ratio exceeds 1.05 or if the geometric mean exceeds 1.05; reserve true google/benchmark for optional future infrastructure.
```

```wp
id: CPP_MEMSAFE-0920-TEST
class: TEST
title: Final infra_lanes.json lane wiring (terminal integration gate)
status: not-started
review_rework_ledger: []
depends_on: [CPP_MEMSAFE-0110-TEST, CPP_MEMSAFE-0120-TEST, CPP_MEMSAFE-0130-TEST, CPP_MEMSAFE-0140-TEST, CPP_MEMSAFE-0150-TEST, CPP_MEMSAFE-0210-TEST, CPP_MEMSAFE-0220-TEST, CPP_MEMSAFE-0225-TEST, CPP_MEMSAFE-0230-TEST, CPP_MEMSAFE-0240-TEST, CPP_MEMSAFE-0310-TEST, CPP_MEMSAFE-0320-TEST, CPP_MEMSAFE-0330-TEST, CPP_MEMSAFE-0410-TEST, CPP_MEMSAFE-0420-TEST, CPP_MEMSAFE-0430-TEST, CPP_MEMSAFE-0510-TEST, CPP_MEMSAFE-0520-TEST, CPP_MEMSAFE-0530-TEST, CPP_MEMSAFE-0535-TEST, CPP_MEMSAFE-0540-TEST, CPP_MEMSAFE-0610-TEST, CPP_MEMSAFE-0710-TEST, CPP_MEMSAFE-0720-TEST, CPP_MEMSAFE-0810-TEST, CPP_MEMSAFE-0900-TEST, CPP_MEMSAFE-0910-TEST]   # every leaf TEST WP — terminal integration gate (Q13)
traces_f1: Slice 9 (tooling integration)
traces_f2: §Test Environment (build configurations; each MEMSAFE_ON_VIOLATION variant; sanitizer matrix)
traces_f3: system modes Release_NoChecks / Debug_Checks
inputs: existing testing/infra_lanes.json (F4); every leaf TEST WP's produced artifacts
artifacts:
  - testing/infra_lanes.json (code-side, extends the existing debug/release/asan-ubsan/cxx17 set):
    confirm the existing optional lane globs for property, fuzz, concurrency, perf, valgrind-cxx17, tsan, msan, and hwasan; add only code-side lane entries if a WP introduces a new subdirectory or launcher. Policy variants remain selected per-test via #define, consistent with the F4 infra_lanes.json comment.
acceptance:
  - `python tools/run_tests.py --backend auto` discovers and passes every test/example/compile_fail with no
    tool edits; SUMMARY line reports result=OK across the declared lanes.
  - This WP edits only the code-side infra_lanes.json (allowed by the drop-in contract), never tools/.
  - Terminal gate (Q13): this WP depends on every leaf TEST WP, so its full-suite acceptance check is only reached
    once the entire runnable test set is finalized; dependency-order execution therefore runs it last.
notes: Round 6 (Q13) expanded depends_on from [0040, 0810, 0910] to the full leaf-TEST set so the raw graph,
  not prose alone, enforces that the "passes every test" check runs after every test exists. 0040 is covered
  transitively (0110→0100→0040), so it is no longer listed explicitly.
```

---

## 4. Spec-coverage matrix (auditable from IDs alone)

### 4.1 F1 Slices 0-9
| Slice | FUNC WP(s) | TEST WP(s) |
|-------|-----------|-----------|
| 0 | 0010, 0020, 0030 | 0035, 0040, 0050, 0060 |
| 1 | 0100 | 0110, 0120, 0130, 0140, 0150 |
| 2 | 0200, 0205 | 0210, 0220, 0225, 0230, 0240 |
| 3 | 0300 | 0310, 0320, 0330 |
| 4 | 0400 | 0410, 0420, 0430 |
| 5 | 0500 | 0510, 0520, 0530, 0535, 0540 |
| 6 | 0600 | 0610 |
| 7 | 0700 | 0710, 0720 |
| 8 | 0800 | 0810 |
| 9 | — | 0900, 0910, 0920 |

### 4.2 F3 types/macros → FUNC WP
Owner/Ref/MutRef → 0100 · Slot/Handle → 0200 · SlotMap → 0205 · Scope → 0300 · SyncOwner/SyncRef/SyncMutRef → 0400 ·
Arc/Mutex → 0500 · MEMSAFE_ON_VIOLATION / memsafe::violation → 0010 · MEMSAFE_BACKEND_* / MEMSAFE_BORROWS /
MEMSAFE_NODISCARD → 0020 · advanced diagnostics → 0600 · Rusty-C++/CHERI + extra clang attrs → 0700 ·
MEMSAFE_CXX17_COMPAT → 0800 · MEMSAFE_RELEASE_CHECKS modes → exercised by release lane (0920).

### 4.3 F2 categories → TEST WP
Unit → 0110/0210/0310/0410/0510/0520 · Sanitizer matrix → asan-ubsan lane (all) + 0320/0810 + optional tsan/msan/hwasan lanes ·
Shared test support → 0035 · Property-based → non-stateful Owner/Ref/MutRef 0140, Handle 0225; stateful SlotMap 0220, Scope 0330, Arc/Mutex 0535 · Fuzz replay → 0230/0540 · Concurrency → 0240/0430/0530 ·
Performance → 0910 · Compile-time static_assert → 0060/0120/0420 · Compile-fail → 0130/0710 ·
Optional back-end → 0710/0720 · Violation policy lanes → 0040/0050 · Full-suite integration gate → 0920.

Per-type property coverage (F2 thresholds): non-stateful (>=10 000 seq/run) — Owner, Ref, MutRef (0140), Handle (0225); stateful (>=1 000 seq/run) — SlotMap (0220), Scope (0330), Arc and Mutex (0535). Every F2-enumerated property type has exactly one owning WP.

---

## 5. Change log

| F5 version | Round | Change |
|------------|-------|--------|
|            |       |        |
|------------|-------|--------|
