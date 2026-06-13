/**
 * @file test_sync_static_asserts.cpp
 * @brief Compile-time trait checks for Slice 4 sync owner and borrow types.
 *
 * @details
 * Work package: CPP_MEMSAFE-0420-TEST.
 *
 * Purpose:
 * - Verify the finalized CPP_MEMSAFE-0400-FUNC `memsafe::SyncOwner<T>` move
 *   contract with `std::is_nothrow_move_constructible_v`, matching the F1
 *   acceptance criterion and the F3 `Move_Noexcept` architecture property.
 * - Verify that `memsafe::SyncMutRef<int>` is non-copyable, so an exclusive
 *   mutable sync borrow cannot be duplicated.
 * - Verify that `SyncOwner<int>::borrow()` and `SyncOwner<int>::borrow_mut()`
 *   return the exact `memsafe::SyncRef<int>` and `memsafe::SyncMutRef<int>`
 *   wrapper types by value. Those wrapper declarations carry the required
 *   type-level `MEMSAFE_NODISCARD` annotation in the finalized sync header.
 *
 * Key invariants:
 * - Every assertion is intentionally at namespace scope so a broken Slice 4
 *   type contract fails translation-unit compilation before any runtime code
 *   can execute.
 * - The concrete `int` instantiations match the package acceptance criteria
 *   and keep this TEST artifact from adding unrelated FUNC requirements.
 * - Standard C++ has no portable reflection trait for type-level
 *   `[[nodiscard]]`; this file therefore statically pins the public functions
 *   to the annotated sync borrow handle types and documents that observable
 *   unused-result diagnostics are a compiler-warning behavior rather than a
 *   type trait.
 * - The executable entry point returns success only after the translation unit
 *   has compiled, making the F4 harness' exit-code convention a compile-time
 *   oracle for these assertions.
 *
 * Ownership and thread-safety:
 * - This file constructs no `SyncOwner`, `SyncRef`, or `SyncMutRef` object at
 *   runtime. Trait queries and unevaluated `decltype` expressions introduce no
 *   ownership, borrowing, allocation, or synchronization side effects.
 */

#include <memsafe/sync.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_move_constructible_v<memsafe::SyncOwner<int>>,
              "SyncOwner<int> must be nothrow move-constructible for the F3 "
              "Move_Noexcept property and move-only container friendliness");

static_assert(std::is_nothrow_move_assignable_v<memsafe::SyncOwner<int>>,
              "SyncOwner<int> must be nothrow move-assignable so ownership "
              "transfer preserves the Slice 4 noexcept move contract");

static_assert(!std::is_copy_constructible_v<memsafe::SyncOwner<int>>,
              "SyncOwner<int> must not be copy-constructible because the "
              "atomic borrow ledger has unique ownership");

static_assert(!std::is_copy_constructible_v<memsafe::SyncMutRef<int>>,
              "SyncMutRef<int> must not be copy-constructible because an "
              "exclusive mutable sync borrow cannot be duplicated");

static_assert(!std::is_copy_assignable_v<memsafe::SyncMutRef<int>>,
              "SyncMutRef<int> must not be copy-assignable because assigning "
              "would duplicate or overwrite an exclusive mutable sync borrow");

static_assert(std::is_same_v<decltype(std::declval<memsafe::SyncOwner<int>&>().borrow()),
                             memsafe::SyncRef<int>>,
              "SyncOwner<int>::borrow() must return SyncRef<int> by value so "
              "the type-level MEMSAFE_NODISCARD annotation protects shared "
              "sync borrows from accidental discard");

static_assert(std::is_same_v<decltype(std::declval<memsafe::SyncOwner<int>&>().borrow_mut()),
                             memsafe::SyncMutRef<int>>,
              "SyncOwner<int>::borrow_mut() must return SyncMutRef<int> by "
              "value so the type-level MEMSAFE_NODISCARD annotation protects "
              "exclusive sync borrows from accidental discard");

/*
 * F1 and F2 require `SyncRef<T>` and `SyncMutRef<T>` to be nodiscard types.
 * C++ exposes support detection for the `[[nodiscard]]` attribute but no
 * standard trait that reflects whether a particular class declaration bears
 * that attribute. The two return-type assertions above are therefore the
 * compile-time portion owned by this package: they prevent the public borrow
 * functions from drifting away from the annotated handle types in
 * `<memsafe/sync.hpp>`.
 */

/**
 * @brief Return the runtime verdict for the sync static-assert translation
 * unit.
 *
 * @retval 0 The translation unit compiled, so all namespace-scope trait and
 * return-type assertions above passed.
 * @pre The build provides the project include directory containing
 * `<memsafe/sync.hpp>`.
 * @post No runtime state is created or modified.
 * @invariant Runtime success is intentionally trivial because the package
 * oracle is compile-time failure on a violated Slice 4 type contract.
 * @throws Nothing; the function is declared `noexcept` and performs no
 * throwing operation.
 * @note Ownership/thread-safety: no memsafe object is instantiated, and no
 * shared state is accessed.
 */
int main() noexcept {
    return 0;
}
