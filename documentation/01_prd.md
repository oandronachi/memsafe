# Memory‑Safe C++ Library v1 – Product Requirements Document

## Executive Summary
C++ remains dominant in systems software but suffers from memory‑safety vulnerabilities. Approximately 70 % of CVE‑assigned vulnerabilities in Microsoft products and the Chromium project are caused by memory‑safety issues, a figure that has remained roughly stable for over a decade [1][2][3], yet migrating existing C++ code to memory‑safe languages such as Rust is costly. The goal of this project is to deliver a header‑only library that provides meaningful memory‑safety guarantees without requiring a new compiler or external runtime. The v1 design offers partial Rust‑like safety through move‑only ownership wrappers, runtime borrow counters, generational handles and RAII scopes. It is explicitly not a full borrow checker, but it reduces use‑after‑free and double‑free bugs and provides an upgrade path throu`_HANDLER`gh optional back‑end hooks and C++17 compatibility.

## Problem Statement
Manual memory management in C++ leads to buffer overflows, use‑after‑free errors and dangling references, which collectively account for the majority of security bugs [1][2]. Compilers and static analyzers help but cannot eliminate temporal safety violations in the absence of language support. Rewriting large C++ codebases in memory‑safe languages is often infeasible due to cost, performance requirements or ecosystem dependencies. A portable, header‑only library that enforces ownership and borrowing semantics at runtime can mitigate these risks while preserving existing tooling.

## Goals and Objectives
- **Provide safe ownership primitives** – A move‑only `Owner<T>` type encapsulates unique ownership of a heap‑allocated `T` and ensures destruction on scope exit. Its move constructor is `noexcept(true)` so that `Owner<T>` is usable in `std::vector` and other standard containers without falling back to copy semantics under reallocation [4][F1 SP2].
- **Support borrowing with runtime checks** – Non‑owning `Ref<T>` and `MutRef<T>` types, both marked `[[nodiscard]]`, track the number of immutable and mutable borrows, enforcing the exclusivity rule and triggering a configurable violation policy when it is broken [F1 SP2].
- **Detect use‑after‑free** – `Handle<T>` and `Slot<T>` form generational handles: each slot carries its own atomic generation counter, and a handle stores the index and a remembered generation; dereferencing verifies that the generations match [5][F1 SP2].
- **Provide bounded arenas and scopes** – A `SlotMap<T>` allocator manages a fixed‑capacity array of slots and a lock‑free Treiber‑stack free list with per‑instance atomic generations, not a global mutex [5][F1 SP2]. A `Scope` type allocates objects in a lifetime‑bounded arena; destruction of the scope frees all contained objects.
- **Offer thread‑safe variants** – `SyncOwner<T>`, `SyncRef<T>`, `SyncMutRef<T>`, `Arc<T>` and `Mutex<T>` add atomic borrow counters or mutual exclusion for cross‑thread sharing.
- **Support optional back‑ends** – Attribute macros such as `MEMSAFE_LIFETIMEBOUND` and `MEMSAFE_BORROWS(x)` expand to nothing by default but can emit annotations for Clang's lifetime analysis, Rusty‑C++ checkers or CHERI memory‑tagging back‑ends.
- **Maintain zero‑cost abstractions in release** – All runtime checks are compiled away when `MEMSAFE_RELEASE_CHECKS=0`, ensuring that the library has negligible overhead in performance builds.
- **Remain header‑only and portable** – Provide a single umbrella header `<memsafe/memsafe.hpp>` that includes six feature headers; support C++20 baseline and offer a C++17 compatibility mode using SFINAE rather than concepts.

## Out‑of‑Scope / Non‑Goals
- **Full Rust‑style borrow checking** – A pure header‑only library cannot track lifetimes across arbitrary C++ code [F1 SP1].
- **Data‑race freedom by construction** – The library does not guarantee race‑free code; it offers `Sync*` types to manage concurrency.
- **Unbounded allocators and weak references** – Features such as `GrowableSlotMap`, `Region<Tag>` and weak handles are deferred to version 1.1.

## Stakeholders and Users
- **Systems programmers** who need to harden legacy C++ applications against memory‑safety bugs without rewriting in another language.
- **Library authors** integrating safe ownership patterns into existing APIs.
- **Security teams** seeking to reduce CVEs attributable to memory management errors.

## User Stories
- *As a developer*, I can wrap a pointer in an `Owner<Foo>` to ensure it is destroyed exactly once when it goes out of scope, and I can store `Owner<Foo>` values in `std::vector` without copy‑degradation under reallocation.
- *As a developer*, I can call `borrow()` on an owner to get a read‑only `Ref<T>` and call `borrow_mut()` to get an exclusive `MutRef<T>`; if I request a mutable borrow while immutable borrows exist, the violation policy is triggered. Discarding the result of `borrow()` or `borrow_mut()` produces a compiler diagnostic because both return types are `[[nodiscard]]`.
- *As a developer*, I can store objects in a `SlotMap<T>` and obtain `Handle<T>` values; if I dereference a handle after the slot has been freed, the library detects the generation mismatch and triggers the violation policy.
- *As a developer*, I can enable back‑end macros to emit lifetime annotations consumed by static analyzers and hardware safety features, improving diagnostics without changing my code.

## Functional Requirements

### Ownership Types
| Type | Description |
| --- | --- |
| `Owner<T>` | Move‑only RAII wrapper that owns a single heap‑allocated `T`. Construction allocates `T`; destruction frees it. Cannot be copied; moving transfers ownership. The move constructor and move‑assignment operator are `noexcept(true)` unconditionally so that `Owner<T>` works correctly with `std::vector` reallocation, which uses `std::move_if_noexcept` and would otherwise degrade to copy and fail to compile for a move‑only type [4]. Provides `T &operator*()`, `T *operator->()`, `bool has_value()`, `[[nodiscard]] Ref<T> borrow()`, and `[[nodiscard]] MutRef<T> borrow_mut()`. Violates exclusivity if a mutable borrow is requested while any borrow exists. |
| `Ref<T>` | Immutable borrow. The type itself is marked `[[nodiscard]]` unconditionally so that discarding a borrow result is a diagnostic in any compliant C++20 build. Holds a non‑owning pointer and increments the owner's borrow counter on construction. Provides `const T &operator*()`, `const T *operator->()`. Copyable; each copy increments the counter. Destruction decrements the counter. A `Ref<T>` may not be held across a `co_await` suspension point; in v1, this constraint is enforced through the backend‑hook annotation `MEMSAFE_BORROWS(x)` paired with a Clang lifetimebound expansion when `MEMSAFE_BACKEND_CLANG` is enabled. Runtime detection of the violation is deferred to v1.1 [6]. |
| `MutRef<T>` | Exclusive mutable borrow. The type itself is marked `[[nodiscard]]` unconditionally. Holds a non‑owning pointer and a token representing exclusive access. Not copyable. Provides `T &operator*()`, `T *operator->()`. Same `co_await` prohibition as `Ref<T>`, enforced via the same backend‑hook annotation mechanism in v1 [6]. |
| `SyncOwner<T>` | Same as `Owner<T>` but uses atomic counters so borrows can cross thread boundaries. Pairs with `SyncRef<T>` and `SyncMutRef<T>`. Also `noexcept(true)` move‑constructible. |
| `SyncRef<T>` / `SyncMutRef<T>` | Thread‑safe variants of `Ref<T>`/`MutRef<T>`; increment and decrement atomic borrow counters. Both marked `[[nodiscard]]`. |

### Handle and Slot Types
| Type | Description |
| --- | --- |
| `Slot<T>` | An entry in a slot map. Stores a `T` and a per‑instance atomic generation counter (`std::atomic<uint32_t>` or wider). When the slot is freed, the generation counter is atomically incremented. |
| `Handle<T>` | Stores an index and a remembered generation. Dereferencing performs a single atomic load of the slot's current generation and compares it with the remembered generation; mismatch triggers the configured violation policy. Provides `T &operator*()`, `T *operator->()`, `bool is_valid()`. |
| `SlotMap<T>` | Lock‑free allocator containing a fixed‑capacity array of slots. The slot‑map does not use a single global generation counter or a shared mutex; each slot carries its own atomic generation. Allocation uses a single compare‑and‑swap on a packed (head‑index, ABA‑counter) head of a Treiber‑stack free list of free slot indices; deallocation pushes the slot index back onto the free list and atomically increments the slot's generation [5]. Lookup of a `Handle<T>` is a single atomic load of the slot's generation followed by a comparison with the handle's remembered generation. Methods: `[[nodiscard]] Handle<T> allocate(args…)` returns a handle to a new slot, `void deallocate(Handle<T>)`, `T *get(Handle<T>)`, `size_t capacity()`, `size_t size()`. Bounded capacity is an explicit v1 trade‑off; growth is deferred to `GrowableSlotMap<T>` in v1.1. |

### Scope and Region
| Type | Description |
| --- | --- |
| `Scope` | An untyped arena that allocates objects and destroys all of them when the scope is destroyed. Methods: `T *create(args…)`, returns an owning pointer managed by the scope. |
| `Region<Tag>` | (deferred to v1.1) Typed region allocator with compile‑time tag to prevent cross‑region moves. Not included in v1. |

### Concurrency Types
| Type | Description |
| --- | --- |
| `Arc<T>` | Atomically reference‑counted shared owner, similar to `std::shared_ptr`. Provides shared ownership across threads. |
| `Mutex<T>` | Mutual exclusion primitive that wraps a `T` and provides a `MutRef<T>` through a `lock()` method. Unlocking decrements the borrow counter. |

### Configuration Macros and Back‑Ends
- `MEMSAFE_ON_VIOLATION`: defines the violation policy; canonical values are `MEMSAFE_VIOLATION_ABORT` (default; emits the configured diagnostic and aborts), `MEMSAFE_VIOLATION_THROW` (throws `memsafe::violation`) and `MEMSAFE_VIOLATION_HANDLER` (invokes a user-installed handler). The PRD, test plan and generated Scrum stories must use only these three policy names.
- `MEMSAFE_RELEASE_CHECKS`: when `0`, disables runtime borrow and generation checks in release builds, providing zero‑overhead in production.
- `MEMSAFE_CXX17_COMPAT`: builds a C++17 subset that removes concepts and `std::source_location` diagnostics.
- `MEMSAFE_BACKEND_CLANG`, `MEMSAFE_BACKEND_RUSTY_CPP`, `MEMSAFE_BACKEND_CHERI`: enable optional back‑ends that expand annotation macros such as `MEMSAFE_LIFETIMEBOUND`, `MEMSAFE_BORROWS(x)`, `MEMSAFE_OWNED`, `MEMSAFE_POINTER`, and `MEMSAFE_LOCK_HELD(m)` to vendor‑specific attributes. By default each macro expands to nothing; the borrow‑across‑`co_await` prohibition is enforced via these annotations only when a backend is enabled.

## Non‑Functional Requirements
- **Performance**: In release builds with checks disabled, operations on `Owner`, `Ref`, `Handle` and other primitives should have overhead comparable to or less than `std::unique_ptr` and `std::shared_ptr`. Performance regressions greater than 5 % relative to baseline in microbenchmarks are unacceptable.
- **Move semantics**: `Owner<T>` and `SyncOwner<T>` must be `noexcept` move‑constructible and `noexcept` move‑assignable. This is verifiable with `static_assert(std::is_nothrow_move_constructible_v<Owner<T>>)` and is required for correct interoperation with `std::vector` reallocation [4].
- **Discardability**: `Ref<T>`, `MutRef<T>`, `SyncRef<T>`, `SyncMutRef<T>` and `Handle<T>` must be `[[nodiscard]]` types; the methods `Owner::borrow()`, `Owner::borrow_mut()`, `SyncOwner::borrow()`, `SyncOwner::borrow_mut()`, `Mutex::lock()` and `SlotMap::allocate()` must carry `[[nodiscard]]` on their return values.
- **Portability**: The library must compile on any standards‑conforming C++20 compiler (GCC, Clang, MSVC) and provide a compatibility switch for C++17.
- **Header‑only**: No `.cpp` files, build system integration or link‑time dependencies for the library itself. The umbrella header includes all feature headers. (Test and benchmark dependencies described in the test plan are not part of the library distribution.)
- **Configurability**: Users can select violation policies and enable optional back‑ends at compile time without modifying source.
- **Safety**: Violating ownership or borrow invariants triggers the configured policy; the default policy is `MEMSAFE_VIOLATION_ABORT`, preventing silent undefined behaviour by emitting diagnostics and aborting.

## Milestones and Roadmap

1. **Vertical slice 0 (foundational) – Violation policy, diagnostics & backend‑hook backbone**: a foundational gate that must land before any other slice begins. Delivers (i) the `MEMSAFE_ON_VIOLATION` configuration macro with three documented modes (`MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW`, `MEMSAFE_VIOLATION_HANDLER`); (ii) the `memsafe::violation` exception type carrying a `std::source_location` and a UTF‑8 diagnostic string; (iii) the backend‑detection macros `MEMSAFE_BACKEND_CLANG`, `MEMSAFE_BACKEND_GCC` and `MEMSAFE_BACKEND_MSVC`, autodetected from `__clang__` / `__GNUC__` / `_MSC_VER` with manual override permitted; (iv) the user‑facing macros `MEMSAFE_BORROWS(x)` (expanding to `[[clang::lifetimebound]]` and `[[clang::coro_lifetimebound]]` under `MEMSAFE_BACKEND_CLANG`, no‑op otherwise) and `MEMSAFE_NODISCARD` (always `[[nodiscard]]`). Justification: slices 1, 2 and 4 below each have a hard compile‑time dependency on the violation type and the backend‑hook macros; without an explicit owner, the dependency gets re‑derived as fragments inside each slice. Approximately 400 LOC of headers plus 200 LOC of tests.
2. **Vertical slice 1 – Owner/Ref/MutRef**: implement basic ownership and borrowing with runtime counters, including the `noexcept` move and `[[nodiscard]]` constraints; include unit tests and property‑based tests. Depends on Slice 0.
3. **Vertical slice 2 – Handle/Slot/SlotMap**: implement generational handles and bounded slot map with per‑instance atomic generation counters and a Treiber‑stack free list; include fuzz harness. Depends on Slice 0.
4. **Vertical slice 3 – Scope**: implement RAII arena and integration with Owner.
5. **Vertical slice 4 – SyncOwner/SyncRef/SyncMutRef**: implement thread‑safe borrowing with atomic counters. Depends on Slice 0.
6. **Vertical slice 5 – Arc/Mutex**: implement shared reference counting and mutex wrapper.
7. **Vertical slice 6 – Advanced diagnostics**: extend the Slice 0 backbone with optional features — richer user-handler payloads for `MEMSAFE_VIOLATION_HANDLER`, structured diagnostics for the borrow‑check call chain, and integration helpers for logging frameworks. Depends on Slice 0.
8. **Vertical slice 7 – Optional vendor backends**: implement annotation macros and compile‑time switches for `MEMSAFE_BACKEND_RUSTY_CPP` and `MEMSAFE_BACKEND_CHERI`, plus any Clang annotations beyond the `lifetimebound` / `coro_lifetimebound` pair already in Slice 0 (e.g., `[[clang::owner]]`, `[[clang::pointer]]`). Depends on Slice 0.
9. **Vertical slice 8 – C++17 compatibility**: implement compatibility layer replacing concepts with SFINAE.
10. **Vertical slice 9 – Tooling integration and examples**: provide examples, integration with sanitizers and build scripts.

## Acceptance Criteria
- Each vertical slice passes all test categories defined in the test plan (unit tests, sanitizer matrix, property‑based tests, fuzz tests, concurrency tests, performance benchmarks, compile‑time checks and optional back‑end tests).
- Documentation for public interfaces covers semantics, preconditions and postconditions.
- No memory‑safety violations (use‑after‑free, double‑free, invalid borrows) are detected in test suites.
- Microbenchmark overhead in release builds with checks disabled is within 5 % of baseline implementations.
- `static_assert` checks confirm `noexcept` move semantics on `Owner<T>` and `SyncOwner<T>` and `[[nodiscard]]` on all borrow/handle types.

## Dependencies and Risks
- The design depends on the availability of C++20 features; C++17 compatibility may lack diagnostics.
- Integration with external analyzers (Rusty‑C++ checker, Clang lifetime analysis) requires vendor support and may produce false positives.
- Concurrency safety depends on the correct use of `Sync*` types; misuse can still cause data races.
- Future hardware back‑ends (CHERI, MTE) are non‑portable and optional; adoption is not guaranteed.
- The `co_await`‑prohibition enforcement is annotation‑based in v1; users building without `MEMSAFE_BACKEND_CLANG` rely on the documented rule rather than a compiler diagnostic. Runtime detection is planned for v1.1.

## Future Work
Version 1.1 may introduce unbounded `GrowableSlotMap`, typed `Region<Tag>` arenas, weak handles, random generational handles, runtime detection of borrows held across `co_await`, and additional concurrency primitives. The library could also integrate deeper with upcoming C++ safety proposals and hardware capabilities.

## References

[1] Microsoft Security Response Center, "We need a safer systems programming language," 2019. https://www.microsoft.com/en-us/msrc/blog/2019/07/we-need-a-safer-systems-programming-language

[2] Microsoft Windows Experience Blog, "Advancing security with Windows and Surface — Microsoft SFI Report Nov 2025," November 2025. https://blogs.windows.com/windowsexperience/2025/11/10/advancing-security-with-windows-and-surface-microsoft-sfi-report-nov-2025/

[3] CISA, "The Urgent Need for Memory Safety in Software Products," 2023. https://www.cisa.gov/news-events/news/urgent-need-memory-safety-software-products

[4] cppreference, `std::vector` and `std::move_if_noexcept`. `std::vector` reallocation uses `std::move_if_noexcept`; if `T`'s move constructor is not `noexcept` and `T` is copy‑constructible, vector copies instead of moves to preserve the strong exception guarantee. For move‑only types whose move constructor is not `noexcept`, reallocation is ill‑formed. https://en.cppreference.com/w/cpp/container/vector ; https://en.cppreference.com/w/cpp/utility/move_if_noexcept

[5] R. K. Treiber, "Systems Programming: Coping with Parallelism" (IBM RJ‑5118, 1986). The Treiber stack is a classic lock‑free LIFO using a single CAS on a head pointer with an ABA‑prevention counter packed alongside the index. Production generational slot‑maps (slotmap Rust crate, EnTT) implement allocation as a CAS pop from a free‑list stack of slot indices and use per‑slot atomic generation counters; lookup is a single atomic load plus comparison.

[6] Rust Reference, "Pin and Unpin" and "Send/Sync in async." Holding a non‑`Send` borrow across an `await` point makes the resulting future non‑`Send`, enforced at compile time by Rust's `Send` checker on the generator state machine. The pattern transfers directly to C++ coroutines: holding `Ref<T>`/`MutRef<T>` across `co_await` is the same risk and admits the same compile‑time mitigation via lifetime annotations. https://doc.rust-lang.org/std/pin/index.html ; https://rust-lang.github.io/async-book/07_workarounds/03_send_approximation.html

[F1 SP1, F1 SP2] Parent debate `00-final-cpp-mem-safe-ai-debate.md`, terminated round 12, convergence 0.95. SP1 establishes that full Rust‑style borrow checking cannot be implemented in a pure header‑only C++ library. SP2 enumerates the v1 type roster and the three design constraints restated above (`noexcept` move on `Owner<T>`; `[[nodiscard]]` and no‑borrow‑across‑`co_await` on `Ref<T>`/`MutRef<T>`; per‑instance atomic generations with Treiber‑stack free list in `SlotMap<T>`).

[F0 R3.RESOLUTION.Q8] Validation debate `03-cpp-mem-safe-docs-ai-debate.md`, terminated round 3, convergence 0.92. R3.RESOLUTION.Q8 establishes that violation policy, diagnostics and the backend‑hook backbone must be delivered as a single foundational slice (Slice 0) before any slice that depends on `MEMSAFE_ON_VIOLATION`, the `memsafe::violation` type or the `MEMSAFE_BACKEND_*` macros.
