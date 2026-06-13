/**
 * @file cf_discard_borrow.cpp
 * @brief Negative compile test for discarding `Owner<T>::borrow()` and
 * `Owner<T>::borrow_mut()`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0130-TEST.
 *
 * Purpose:
 * - Verify the PRD discardability rule that immutable and mutable borrow
 *   handles are observable `[[nodiscard]]` results.
 * - Provide the command-center artifact
 *   `testing/tests/compile_fail/cf_discard_borrow.cpp` as a complete source
 *   discovered by the F4 compile-fail harness.
 * - Ensure `-Wunused-result -Werror` turns both required F2 Ownership case 4
 *   probes into build failures: the discarded `borrow()`/`Ref<T>` path and the
 *   discarded `borrow_mut()`/`MutRef<T>` path.
 * - Pin the F4 expected-diagnostic regex to the two virtual probe filenames so
 *   the CTest only passes when diagnostics are emitted for both discard sites.
 *
 * Key invariants:
 * - The intentional diagnostics are exactly the two bare expressions required
 *   by F2 Ownership case 4: `immutable_owner.borrow();` for `Ref<T>` and
 *   `mutable_owner.borrow_mut();` for `MutRef<T>`.
 * - The owners are otherwise valid and live, so the test is about compile-time
 *   discardability rather than runtime borrow exclusivity.
 * - Separate owners keep the immutable and mutable discard checks independent:
 *   neither result can affect the other's borrow ledger if this file is ever
 *   compiled without warning-as-error flags during diagnosis.
 * - The `INFRA_EXPECT_FAIL` directive requires both probe filenames and
 *   nodiscard/unused-result diagnostics in source order. If either discard path
 *   loses nodiscard coverage, that probe disappears from the compiler output
 *   and the F4 driver reports the regression.
 *
 * Ownership and thread-safety:
 * - The source creates automatic, single-threaded `Owner<int>` instances. Each
 *   borrow result is intentionally discarded before any runtime use, which is
 *   the exact misuse the public API annotation must catch.
 *
 * Traceability:
 * - F1 PRD `Discardability`: `Ref<T>`, `MutRef<T>`, `Owner::borrow()`, and
 *   `Owner::borrow_mut()` are `[[nodiscard]]`.
 * - F2 `Ownership` case 4: discarding `borrow()`/`Ref<T>` and
 *   `borrow_mut()`/`MutRef<T>` under `-Wunused-result -Werror` must fail to
 *   compile.
 * - F3 AADL `Nodiscard` on `Ref`, `MutRef`, `Borrow`, and `Borrow_Mut`.
 */
// INFRA_EXPECT_FAIL: cf_discard_borrow_ref_probe.cpp.*(nodiscard|unused-result|C4834|C4858).*cf_discard_borrow_mutref_probe.cpp.*(nodiscard|unused-result|C4834|C4858)

#include <memsafe/owner.hpp>

/**
 * @brief Intentionally discard immutable and mutable borrow results.
 *
 * @retval 0 Unreachable in conforming warning-as-error validation lanes.
 * @pre The compile-fail target is built with `-Wunused-result -Werror` on
 * GCC/Clang or the MSVC warning-as-error equivalent supplied by the F4 harness.
 * @post A conforming Slice 1 implementation emits nodiscard/unused-result
 * diagnostics and no executable is produced.
 * @invariant `Owner<T>::borrow()` must return a non-discardable `Ref<T>` by
 * value, and `Owner<T>::borrow_mut()` must return a non-discardable
 * `MutRef<T>` by value. The file keeps both expressions present so a reviewer
 * can trace the immutable and mutable discard diagnostics separately to F2
 * Ownership case 4; virtual `#line` names give the F4 regex stable probe names
 * without adding any unrelated ill-formed code.
 * @throws Nothing intentionally; compilation is expected to fail before runtime.
 * @note Ownership/thread-safety: both owners are automatic and single-threaded.
 * The discarded borrows are non-owning and should never reach runtime in this
 * test lane.
 *
 * Example:
 * @code
 * memsafe::Owner<int> immutable_owner(7);
 * immutable_owner.borrow(); // expected unused-result/nodiscard failure
 *
 * memsafe::Owner<int> mutable_owner(9);
 * mutable_owner.borrow_mut(); // expected unused-result/nodiscard failure
 * @endcode
 */
int main() {
    memsafe::Owner<int> immutable_owner(7);
    memsafe::Owner<int> mutable_owner(9);

    /*
     * Spec rationale: F1 discardability and F2 Ownership case 4 require both
     * borrow-returning functions to be consumed. The `#line` markers give the
     * F4 regex stable probe names for each diagnostic, but the regex deliberately
     * uses simple `.*` spans to avoid the expensive multiline alternation that
     * caused the prior full-validation failure.
     */
#line 101 "cf_discard_borrow_ref_probe.cpp"
    immutable_owner.borrow();

#line 201 "cf_discard_borrow_mutref_probe.cpp"
    mutable_owner.borrow_mut();

#line 102 "cf_discard_borrow.cpp"
    return 0;
}
