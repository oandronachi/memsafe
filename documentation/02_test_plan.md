# Memory‑Safe C++ Library v1 – Test Plan

## Introduction
This test plan defines the testing strategy for each vertical slice of the memory‑safe C++ library described in the product requirements document. The objective is to demonstrate that the v1 implementation enforces ownership and borrowing invariants, detects use‑after‑free and invalid borrows, provides thread‑safe variants, integrates with sanitizers and optional back‑ends, and meets performance goals.

## Test Categories
| Category | Purpose |
|---|---|
| **Unit tests** | Exercise every public operation of each type with positive and negative cases. Negative tests for library-managed invariants intentionally violate ownership or borrowing rules and verify that the violation policy triggers. Scope stale-pointer tests are sanitizer-only because the v1 `Scope::create(args…)` API returns a raw `T*`. Coverage must be 100 % of public operations and 100 % of library-managed violation paths. |
| **Sanitizer matrix** | Run all unit tests under multiple configurations: AddressSanitizer (ASan) with UndefinedBehaviourSanitizer (UBSan); ThreadSanitizer (TSan); MemorySanitizer (MSan) for builds with checks enabled; HardwareAddressSanitizer (HWASan) on AArch64 when available; and Valgrind memcheck for the C++17 subset. Sanitizer availability varies by toolchain — see § Test Environment for the per‑platform matrix. Any unexpected sanitizer report fails the slice; sanitizer-only negative tests, such as post-destruction access through a raw `Scope::create(args…)` pointer, pass only when the expected sanitizer or memcheck diagnostic is observed. |
| **Property‑based tests** | For each ownership or borrow type, use a property‑based testing library (`emil-e/rapidcheck`, BSD‑2, header + CMake) to generate random sequences of construction, borrowing, moving and dropping operations and assert that the safety invariant holds after each step. Each CI run must execute at least 10 000 sequences per non‑stateful type (`Owner<T>`, `Ref<T>`, `MutRef<T>`, `Handle<T>`) and at least 1 000 sequences per stateful type (`SlotMap<T>`, `Mutex<T>`, `Arc<T>`, `Scope`), with shrinking applied to all failures. Nightly builds run a 10× multiplier on both the non‑stateful and stateful counts: at least 100 000 sequences per non‑stateful type and at least 10 000 sequences per stateful type. The lower stateful baseline reflects that RapidCheck command‑sequence shrinking is super‑linear in sequence length [3]. |
| **Fuzz tests** | For stateful types (`SlotMap<T>`, `Mutex<T>`, `Arc<T>`), build libFuzzer harnesses that explore state machines and stress unusual sequences. Each harness runs for at least five minutes per change; the nightly budget is 24 CPU‑hours across all harnesses. |
| **Concurrency tests** | For `SyncOwner<T>`, `SyncRef<T>`, `SyncMutRef<T>` and `Mutex<T>`, use **Relacy Race Detector** (`dvyukov/relacy`, header‑only C++11) as the primary permutation‑testing framework to exhaustively explore interleavings of up to two threads with runtime checks enabled [1]. **CDSChecker** or **C11Tester** (UCI PLRG) may be used as secondary options where Relacy's C++11 limitations apply, accepting their non‑header‑only distribution [2]. |
| **Performance benchmarks** | Use `google/benchmark` as a CMake‑integrated dependency linked at test‑binary level. The upstream library is not header‑only and requires linking against `libbenchmark` (`benchmark.lib` on Windows) [4]; this dependency is confined to the test build and is not part of the library distribution. Measure microbenchmarks of each type against raw pointers, `std::unique_ptr` and `std::shared_ptr`. A slice fails if the geometric mean slowdown exceeds 5 % in release builds with checks disabled. |
| **Compile‑time static tests** | Use `static_assert` to verify properties such as `std::is_nothrow_move_constructible_v<Owner<T>>`, `std::is_nothrow_move_constructible_v<SyncOwner<T>>`, `!std::is_copy_constructible_v<Owner<T>>`, `!std::is_copy_constructible_v<MutRef<T>>`, and that `Ref<T>`, `MutRef<T>`, `SyncRef<T>`, `SyncMutRef<T>` and `Handle<T>` are marked `[[nodiscard]]` (verified by a `-Wunused-result` compile‑fail test). Compile‑fail tests verify that ill‑formed uses are rejected by SFINAE/concepts. |
| **Optional back‑end tests** | When a back‑end macro is enabled, run all tests again with the annotations active to ensure that the library integrates with Clang lifetime analysis, Rusty‑C++ and CHERI/MTE back‑ends without false positives. The `MEMSAFE_BACKEND_CLANG` lane additionally verifies that holding a `Ref<T>` or `MutRef<T>` across a `co_await` suspension point is rejected by Clang's lifetimebound analysis. |

## Test Environment
- **Compilers**: GCC and Clang with C++20 support for the main build on Linux and macOS. MSVC for Windows ASan coverage; clang‑cl for additional Windows compiler coverage where useful, but Windows `clang-cl` is not a required TSan/MSan lane. The C++17 compatibility mode must be tested separately.
- **Platforms**:
  - **Linux x86‑64 with GCC and Clang** — mandatory; runs ASan, UBSan and TSan on supported GCC/Clang configurations, MSan on Clang, and Valgrind.
  - **Linux AArch64 with Clang** — recommended; runs HWASan and the standard sanitizer matrix.
  - **macOS with Clang (x86‑64 and Apple Silicon)** — recommended; runs ASan, UBSan and TSan where supported. MSan is not mandatory on macOS.
  - **Windows with MSVC** — mandatory for ASan coverage only. TSan and MSan are not supported by MSVC (verified November 2025) [5] and are excluded from the Windows MSVC lane. Visual Studio 2026 adds MSVC ASan support on ARM64.
  - **Windows with clang‑cl** — optional/recommended for additional Windows compiler coverage and any sanitizer support available in the installed LLVM toolchain. TSan and MSan are not required on Windows unless upstream LLVM adds supported Windows lanes; required TSan/MSan coverage comes from Linux Clang lanes, with macOS TSan as additional coverage where available.
- **Build configurations**: Debug builds with checks enabled (`MEMSAFE_RELEASE_CHECKS=1`) and release builds with checks disabled (`MEMSAFE_RELEASE_CHECKS=0`); each canonical `MEMSAFE_ON_VIOLATION` variant (`MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW`, `MEMSAFE_VIOLATION_HANDLER`) must be tested. The handler lane must install a deterministic test handler and verify that it receives the violation kind, `std::source_location` and diagnostic string.
- **Dependencies**:
  - **Google Test** — unit test framework; CMake‑integrated.
  - **RapidCheck** — property‑based testing; header + CMake; packaged on Conan Center (recipe `cci.20231215`) and in Debian as `librapidcheck-dev` [3].
  - **libFuzzer** — fuzz framework; ships with Clang.
  - **google/benchmark** — performance benchmarks; CMake‑integrated dependency that must be linked at the test‑binary level [4].
  - **Relacy Race Detector** — concurrency permutation testing; header‑only [1]. **CDSChecker** / **C11Tester** as secondary options (dynamic libraries) [2].
  - **Valgrind** — memcheck for the C++17 subset; Linux only.
  - All test dependencies are confined to the test build and are not part of the library distribution.

## Test Cases

### Violation Policy and Diagnostics
1. **Canonical policy names**
   - Compile one lane for each canonical `MEMSAFE_ON_VIOLATION` mode: `MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW` and `MEMSAFE_VIOLATION_HANDLER`. No tests or generated stories may use the removed `assert` / `terminate` policy names.
2. **Abort policy**
   - Under `MEMSAFE_VIOLATION_ABORT`, trigger an invalid mutable borrow and verify the process aborts after emitting the configured diagnostic.
3. **Throw policy**
   - Under `MEMSAFE_VIOLATION_THROW`, trigger a generation mismatch and verify that `memsafe::violation` is thrown with a diagnostic string and `std::source_location` metadata.
4. **Handler policy**
   - Under `MEMSAFE_VIOLATION_HANDLER`, install a test handler, trigger each representative violation class and verify the handler receives the violation kind, diagnostic string and source location.

### Ownership
1. **Owner construction and destruction**
   - Create an `Owner<int>` and verify that `has_value()` is true. Destroy it and ensure the destructor of `int` runs exactly once.
2. **Borrow rules**
   - Create an `Owner<std::string>`, call `borrow()` twice to get two `Ref<T>`; verify both can read. Request `borrow_mut()` and assert that the violation policy triggers because immutable borrows exist.
   - After all `Ref<T>` are destroyed, call `borrow_mut()`, modify the object and verify the change through the owner.
3. **Move semantics**
   - Move an `Owner<T>` into another instance and verify the original reports empty and the new one retains ownership. Attempt to copy and ensure it fails at compile time (`static_assert`).
   - Verify with `static_assert(std::is_nothrow_move_constructible_v<Owner<T>>)` that the move constructor is `noexcept`.
   - Insert `Owner<T>` instances into a `std::vector` and trigger reallocation; verify that the vector moves rather than copies (a copy would fail to compile for the move‑only type) [6].
4. **Discardability**
   - Compile‑fail test: calling `borrow()` and discarding the result with `-Wunused-result -Werror` must fail to compile, confirming the `[[nodiscard]]` annotation on `Ref<T>` is effective. Same for `borrow_mut()` and `MutRef<T>`.
5. **Co_await prohibition (backend lane)**
   - With `MEMSAFE_BACKEND_CLANG` enabled, compile a test case that holds a `Ref<T>` across a `co_await` suspension point and verify that Clang's lifetimebound analysis rejects the construct [7].

### Generational Handles
1. **SlotMap allocation and deallocation**
   - Create a `SlotMap<int>` with capacity N; allocate N objects and store their handles. Deallocate one handle and allocate another; verify that the slot is reused and the handle's generation is incremented (atomically, with no global mutex contention observable under stress).
2. **Use‑after‑free detection**
   - Allocate a handle, deallocate it and then attempt to dereference the handle. Verify that the violation policy triggers due to generation mismatch.
3. **Bounded capacity**
   - Attempt to allocate beyond the slot map's capacity and verify that the library returns an error or triggers the configured violation policy.
4. **Lock‑free allocation under contention**
   - Spawn N threads each allocating and deallocating from the same `SlotMap<T>`; verify that all allocations succeed without deadlock and that generations remain monotonic per slot. Run under Relacy to exhaust interleavings of the Treiber‑stack CAS pop/push pair [1].

### Scope and Concurrency
1. **Scope lifetime**
   - Create a `Scope`, allocate several objects and verify they are accessible within the scope. Destroy the scope and verify that each contained object's destructor runs exactly once.
   - Because `Scope::create(args…)` returns a raw `T*` managed by the scope in v1, direct access through that raw pointer after the scope is destroyed cannot reliably be intercepted by the library violation policy. In sanitizer/death-test lanes only, preserve a raw pointer returned by `create()`, destroy the scope, intentionally access the pointer and verify that ASan, HWASan or Valgrind reports use-after-scope or use-after-free. UBSan may run as part of the sanitizer matrix but is not sufficient as the pass/fail oracle for this case. Non-sanitized lanes must not assert that `MEMSAFE_ON_VIOLATION` triggers for raw-pointer post-scope access unless a future PRD revision replaces the raw pointer with a checked scope handle.
2. **SyncOwner across threads**
   - Spawn two threads: one thread creates a `SyncOwner<int>` and borrows it; the second thread attempts to borrow mutably. Verify that atomic counters maintain correctness and that data races are not introduced when used properly. Run the test under Relacy in the concurrency lane.
3. **Mutex**
   - Wrap a counter in a `Mutex<int>`, spawn two threads that increment it through `lock()`. Verify that increments are correct and no race conditions occur.

### Optional Back‑End and Sanitizer Tests
- Enable `MEMSAFE_BACKEND_CLANG` and run the test suite, ensuring the Clang lifetime annotations do not introduce new failures and that the `Ref`/`MutRef`‑across‑`co_await` test from § Ownership is rejected.
- Enable `MEMSAFE_BACKEND_CHERI` on a CHERI‑capable system and run the sanitizer matrix; treat any capability violation as a test failure.

## Definitions of Done
A vertical slice is considered complete when:
- All unit tests, property‑based tests, fuzz tests, concurrency tests, performance benchmarks, compile‑time checks and optional back‑end tests pass with no unexpected violations detected on the platforms mandatory for the affected types. Expected diagnostics from negative tests must be asserted explicitly by the relevant violation-policy, compile-fail, sanitizer or memcheck harness.
- Sanitizer coverage matches the platform matrix: Linux Clang lanes must provide ASan, UBSan, TSan and MSan coverage; Linux GCC lanes must provide ASan, UBSan and TSan where supported; macOS Clang TSan is recommended where available; absence of TSan/MSan on Windows MSVC and Windows `clang-cl` lanes is acceptable until upstream support exists.
- Test coverage and property‑based test execution thresholds are met: per CI run, 10 000 sequences per non‑stateful type and 1 000 sequences per stateful type; per nightly run, the 10× multiplier is applied to both categories (100 000 per non‑stateful type and 10 000 per stateful type).
- Performance benchmarks meet the stated 5 % threshold.
- Compile‑time `static_assert`s verify `noexcept` move on `Owner<T>` / `SyncOwner<T>` and `[[nodiscard]]` on all borrow/handle types.
- Documentation for the implemented APIs is updated with examples and safety notes.
- A code review confirms adherence to design constraints (no `.cpp` files in the library distribution, proper use of macros, C++20 compliance).

## Maintenance and Regression Testing
All tests become part of the continuous integration pipeline. Nightly builds run the full sanitizer matrix, fuzz harnesses, performance benchmarks and the 10× multiplier on both non‑stateful and stateful property‑based test counts. New contributions must include tests covering added behaviour and must not regress previous safety guarantees.

## References

[1] D. Vyukov, *Relacy Race Detector (RRD)*, header‑only C++ synchronization‑algorithm verifier for relaxed memory models. https://www.1024cores.net/home/relacy-race-detector ; https://github.com/dvyukov/relacy

[2] B. Norris and B. Demsky, "CDSchecker: checking concurrent data structures written with C/C++ atomics," *ACM SIGPLAN Notices* 48 (10), 2013. https://plrg.ics.uci.edu/software_page/42-2/ ; W. Luo and B. Demsky, "C11Tester: A Race Detector for C/C++ Atomics," *ASPLOS '21*. https://dl.acm.org/doi/pdf/10.1145/3445814.3446711

[3] `emil-e/rapidcheck`, BSD‑2 licensed C++ property‑based testing framework. Conan Center recipe `cci.20231215`, last refreshed 2025‑06‑17. https://github.com/emil-e/rapidcheck ; https://conan.io/center/recipes/rapidcheck

[4] `google/benchmark`, Google microbenchmarking library. Requires linking against `libbenchmark`; no header‑only distribution exists. https://github.com/google/benchmark ; https://github.com/google/benchmark/issues/336

[5] MSVC sanitizer support, November 2025: AddressSanitizer only (extended to ARM64 in Visual Studio 2026). ThreadSanitizer is supported on Linux, Darwin, FreeBSD, NetBSD, Android (Clang/GCC); not on Windows. MemorySanitizer is Clang‑only on x86_64/AArch64/PPC64/MIPS64; not on Windows. https://learn.microsoft.com/en-us/cpp/sanitizers/asan ; https://clang.llvm.org/docs/ThreadSanitizer.html ; https://github.com/google/sanitizers/wiki/MemorySanitizer ; https://docs.conan.io/2/security/sanitizers.html

[6] cppreference, `std::vector` reallocation uses `std::move_if_noexcept`. https://en.cppreference.com/w/cpp/container/vector ; https://en.cppreference.com/w/cpp/utility/move_if_noexcept

[7] Rust reference for the Send/Sync‑across‑await pattern that motivates the C++ `co_await` borrow prohibition. https://doc.rust-lang.org/std/pin/index.html ; https://rust-lang.github.io/async-book/07_workarounds/03_send_approximation.html

[F1 SP1, F1 SP2, F1 R11.DEFENSE.Q12] Parent debate `00-final-cpp-mem-safe-ai-debate.md`, terminated round 12, convergence 0.95. R11.DEFENSE.Q12 establishes the testing‑methodology mandate this plan implements.
