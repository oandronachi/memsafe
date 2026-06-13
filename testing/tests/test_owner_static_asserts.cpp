/**
 * @file test_owner_static_asserts.cpp
 * @brief Compile-time trait checks for Slice 1 owner and borrow types.
 *
 * @details
 * Work package: CPP_MEMSAFE-0120-TEST.
 *
 * Purpose:
 * - Verify the finalized CPP_MEMSAFE-0100-FUNC `memsafe::Owner<T>` move
 *   contract with `std::is_nothrow_move_constructible_v` and
 *   `std::is_nothrow_move_assignable_v`.
 * - Verify the Slice 1 non-copyable contract for `memsafe::Owner<int>` and
 *   `memsafe::MutRef<int>`.
 * - Document the nodiscard boundary for this package: C++ exposes no portable
 *   type trait for class-level `[[nodiscard]]`, so CPP_MEMSAFE-0130-TEST owns
 *   the behavioral `-Wunused-result -Werror` compile-fail proof required by
 *   the command center.
 *
 * Key invariants:
 * - Every assertion is intentionally at namespace scope so a broken type
 *   contract fails translation-unit compilation before any runtime code can
 *   execute.
 * - The concrete `int` instantiations match the package acceptance criteria
 *   and avoid adding broader FUNC requirements in this TEST-only artifact.
 * - The executable entry point returns success only after the translation unit
 *   has compiled, making the generic test harness' exit-code convention a
 *   simple compile-time oracle.
 *
 * Ownership and thread-safety:
 * - This file constructs no `Owner`, `Ref`, or `MutRef` object at runtime.
 *   Trait queries are purely compile-time and introduce no ownership or
 *   synchronization side effects.
 */

#include <memsafe/owner.hpp>

#include <type_traits>

static_assert(std::is_nothrow_move_constructible_v<memsafe::Owner<int>>,
              "Owner<int> must be nothrow move-constructible for vector "
              "reallocation and the F3 Move_Noexcept property");

static_assert(std::is_nothrow_move_assignable_v<memsafe::Owner<int>>,
              "Owner<int> must be nothrow move-assignable for the Slice 1 "
              "move-only owner contract");

static_assert(!std::is_copy_constructible_v<memsafe::Owner<int>>,
              "Owner<int> must not be copy-constructible because ownership is "
              "unique and copying would duplicate the borrow ledger");

static_assert(!std::is_copy_constructible_v<memsafe::MutRef<int>>,
              "MutRef<int> must not be copy-constructible because an "
              "exclusive mutable borrow cannot be duplicated");

/*
 * The PRD and test plan require type-level [[nodiscard]] on borrow handles.
 * Standard C++ does not provide a reflection trait for that attribute, so this
 * static-assert package records the intent and relies on CPP_MEMSAFE-0130-TEST
 * to verify the observable discard diagnostic with -Wunused-result -Werror.
 */

/**
 * @brief Return the runtime verdict for the static-assert translation unit.
 *
 * @retval 0 The translation unit compiled, so all namespace-scope trait
 * assertions above passed.
 * @pre The build provides the project include directory containing
 * `<memsafe/owner.hpp>`.
 * @post No runtime state is created or modified.
 * @invariant Runtime success is intentionally trivial because the package
 * oracle is compile-time failure on a violated trait.
 * @throws Nothing; the function is declared `noexcept` and performs no
 * throwing operation.
 * @note Ownership/thread-safety: no memsafe object is instantiated, and no
 * shared state is accessed.
 */
int main() noexcept {
    return 0;
}
