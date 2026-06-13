/**
 * @file cf_copy_mutref.cpp
 * @brief Negative compile test proving `memsafe::MutRef<T>` is non-copyable.
 *
 * @details
 * Work package: CPP_MEMSAFE-0130-TEST.
 *
 * Purpose:
 * - Exercise the exclusive-borrow contract that `memsafe::MutRef<T>` cannot be
 *   copied because copying would duplicate mutable access to the same payload.
 * - Provide the command-center artifact
 *   `testing/tests/compile_fail/cf_copy_mutref.cpp` as a complete source file
 *   discovered by the F4 compile-fail harness.
 * - Pin the accepted build failure to the deleted-copy diagnostic instead of
 *   accepting an arbitrary compile error.
 *
 * Key invariants:
 * - The source first acquires a valid mutable borrow from a live owner, then
 *   performs the single intentional error: copy construction of that borrow.
 * - A namespace-scope trait guard documents the same non-copyable property but
 *   uses a sentinel message that intentionally does not match the expected
 *   regex. If `MutRef<T>` accidentally becomes copyable, the compile-fail
 *   driver reports the regression instead of treating the guard as a valid
 *   failure.
 * - The file does not discard any nodiscard result, so its failure reason stays
 *   separate from the discardability coverage in `cf_discard_borrow.cpp`.
 * - If `MutRef<T>` accidentally becomes copy-constructible, this translation
 *   unit either compiles or fails for the guard's unmatched sentinel, and the
 *   F4 compile-fail driver reports the regression.
 *
 * Ownership and thread-safety:
 * - `MutRef<T>` is a non-owning, single-threaded exclusive borrow. Copying it
 *   would create two handles for one exclusive token and violate the Slice 1
 *   borrow model.
 *
 * Traceability:
 * - F1 PRD `Ownership Types`: `MutRef<T>` is not copyable.
 * - F2 compile-time static tests: non-copyable borrow types must be rejected by
 *   the compiler.
 * - F3 AADL `data MutRef`: `Nodiscard => true` and `Copyable => false`.
 */
// INFRA_EXPECT_FAIL: (deleted|C2280)

#include <memsafe/owner.hpp>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<memsafe::MutRef<int>>,
              "MEMSAFE_MUTREF_EXCLUSIVE_STATIC_ASSERT_SENTINEL");

/**
 * @brief Intentionally attempt to copy an exclusive mutable borrow.
 *
 * @retval 0 Unreachable in conforming builds; would indicate that the deleted
 * copy-constructor check no longer rejects this translation unit.
 * @pre The compile-fail target is built with the project include directory and
 * warning-as-error flags supplied by the F4 harness.
 * @post A conforming Slice 1 implementation rejects the translation unit before
 * an executable exists.
 * @invariant A mutable borrow token must have exactly one live `MutRef<T>`
 * handle, transferred only by move and never duplicated by copy.
 * @throws Nothing intentionally; compilation is expected to fail before runtime.
 * @note Ownership/thread-safety: `mutable_borrow` is a single-threaded,
 * non-owning exclusive borrow into `owner`; copying it is deliberately
 * ill-formed.
 *
 * Example:
 * @code
 * memsafe::Owner<int> owner(11);
 * auto mutable_borrow = owner.borrow_mut();
 * memsafe::MutRef<int> copied(mutable_borrow); // expected failure
 * @endcode
 */
int main() {
    memsafe::Owner<int> owner(11);
    memsafe::MutRef<int> mutable_borrow = owner.borrow_mut();

    /*
     * Spec rationale: F1 and F3 require MutRef to be exclusive and
     * `Copyable=false`. Copy construction is the narrowest compile-fail trigger
     * for that contract and avoids conflating this case with nodiscard.
     */
    memsafe::MutRef<int> copied(mutable_borrow);

    (void)copied;
    return 0;
}
