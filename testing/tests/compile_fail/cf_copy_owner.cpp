/**
 * @file cf_copy_owner.cpp
 * @brief Negative compile test proving `memsafe::Owner<T>` is move-only.
 *
 * @details
 * Work package: CPP_MEMSAFE-0130-TEST.
 *
 * Purpose:
 * - Exercise the Slice 1 ownership contract that `memsafe::Owner<T>` cannot be
 *   copied because it uniquely owns one heap allocation and its borrow ledger.
 * - Provide the command-center artifact
 *   `testing/tests/compile_fail/cf_copy_owner.cpp` as a complete source file
 *   discovered by the F4 compile-fail harness.
 * - Pin the accepted build failure to the deleted-copy diagnostic instead of
 *   accepting an arbitrary compile error.
 *
 * Key invariants:
 * - The only diagnostic accepted by `INFRA_EXPECT_FAIL` is the compiler's
 *   deleted-copy diagnostic for `memsafe::Owner<int>`, or MSVC C2280.
 * - A namespace-scope trait guard documents the same move-only property but
 *   uses a sentinel message that intentionally does not match the expected
 *   regex. If `Owner<T>` accidentally becomes copyable, the compile-fail driver
 *   reports the regression instead of treating the guard as a valid failure.
 * - The runtime tail contains no dereference, `has_value()` query, discarded
 *   nodiscard result, syntax error, missing include, or unrelated type mismatch,
 *   so the compile-fail oracle remains attributable to the F1 move-only
 *   requirement and AADL `Copyable => false` property.
 *
 * Ownership and thread-safety:
 * - The attempted copy would duplicate a unique owner and its single-threaded
 *   borrow ledger, which is exactly the state split the Slice 1 contract
 *   forbids. No executable should be produced by a conforming validation lane.
 *
 * Traceability:
 * - F1 PRD `Ownership Types`: `Owner<T>` cannot be copied; moving transfers
 *   ownership.
 * - F1 non-functional `Move semantics`: `Owner<T>` is move-only and nothrow
 *   movable for container reallocation.
 * - F2 `Ownership` copy case: attempts to copy an owner fail at compile time.
 * - F3 AADL `data Owner`: `Copyable => false`.
 */
// INFRA_EXPECT_FAIL: (deleted|C2280)

#include <memsafe/owner.hpp>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<memsafe::Owner<int>>,
              "MEMSAFE_OWNER_UNIQUE_STATIC_ASSERT_SENTINEL");

/**
 * @brief Intentionally attempt to copy a unique owner.
 *
 * @retval 0 Unreachable in conforming builds; would indicate that the deleted
 * copy-constructor check no longer rejects this translation unit.
 * @pre The compile-fail target is built with the project include directory and
 * warning-as-error flags supplied by the F4 harness.
 * @post A conforming Slice 1 implementation rejects the translation unit before
 * an executable exists.
 * @invariant Copying `memsafe::Owner<int>` must remain ill-formed so one heap
 * payload and borrow ledger never have two owners.
 * @throws Nothing intentionally; compilation is expected to fail before runtime.
 * @note Ownership/thread-safety: `original` is the sole valid owner. The
 * attempted `copied` construction is deliberately rejected to preserve unique
 * ownership and single-threaded borrow-ledger integrity.
 *
 * Example:
 * @code
 * memsafe::Owner<int> original(42);
 * memsafe::Owner<int> copied = original; // expected compile-time failure
 * @endcode
 */
int main() {
    memsafe::Owner<int> original(42);

    /*
     * Spec rationale: F1 and F3 require Owner to be move-only (`Copyable=false`).
     * Copy-initialization from a named lvalue selects the deleted copy
     * constructor directly. The only accepted diagnostic is the compiler's
     * deleted-function report, keeping this negative test tied to ownership
     * duplication rather than any later runtime observer.
     */
    memsafe::Owner<int> copied = original;

    (void)copied;
    return 0;
}
