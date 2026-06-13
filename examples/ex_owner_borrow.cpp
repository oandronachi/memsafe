/**
 * @file ex_owner_borrow.cpp
 * @brief Standalone smoke example for `memsafe::Owner`, `memsafe::Ref`, and
 * `memsafe::MutRef`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0900-TEST.
 *
 * Purpose:
 * - Demonstrate the finalized CPP_MEMSAFE-0100-FUNC ownership API from a
 *   self-contained example executable.
 * - Show unique ownership through `Owner<T>`, shared immutable borrows through
 *   `Ref<T>`, and one exclusive mutable borrow through `MutRef<T>`.
 * - Return an ordinary process exit code so the F4 `examples/*.cpp` discovery
 *   lane can treat this file as a smoke test.
 *
 * Key invariants:
 * - Immutable borrow handles are kept in a lexical block and destroyed before
 *   `borrow_mut()` is called, preserving the Slice 1 borrow-exclusivity rule.
 * - No violation path is intentionally triggered; the example is valid in
 *   checked, release, sanitizer, and C++17 compatibility lanes.
 * - Moving an `Owner<T>` transfers ownership and leaves the source empty, which
 *   mirrors the PRD's move-only RAII owner contract.
 *
 * Ownership and thread-safety:
 * - All objects are automatic variables in one process and one thread.
 * - Borrow handles are non-owning views into their source `Owner<T>` and do not
 *   outlive that owner.
 */
#include <memsafe/owner.hpp>

#include <string>
#include <utility>

/**
 * @brief Run the owner/borrow smoke example.
 *
 * @retval 0 The example exercised construction, immutable borrowing, mutable
 * borrowing, and move transfer successfully.
 * @retval 1 The initial owner did not contain a value after construction.
 * @retval 2 Immutable borrows did not observe the expected payload.
 * @retval 3 The mutable borrow did not update the payload as expected.
 * @retval 4 The owner did not retain the post-borrow mutation.
 * @retval 5 Moving an owner did not leave the source empty.
 * @retval 6 The destination owner did not receive the moved payload.
 * @pre The executable is built with the project `include/` directory on the
 * compiler include path and a C++17-or-newer compiler.
 * @post No global state is retained; all owner-managed payloads have been
 * destroyed by normal RAII cleanup before process exit.
 * @invariant The example never requests a mutable borrow while an immutable
 * borrow is live.
 * @throws The example does not intentionally throw. Allocation failure or an
 * unexpected library exception is allowed to terminate the smoke executable
 * under the runner's exit-code model.
 * @note Ownership/thread-safety: all `Owner`, `Ref`, and `MutRef` values are
 * local to `main` and are used on a single thread.
 */
int main() {
    memsafe::Owner<std::string> title(std::string("daily"));
    if (!title.has_value()) {
        return 1;
    }

    {
        memsafe::Ref<std::string> first_read = title.borrow();
        memsafe::Ref<std::string> second_read = first_read;

        if (*first_read != "daily" || second_read->size() != 5U) {
            return 2;
        }
    }

    {
        memsafe::MutRef<std::string> write = title.borrow_mut();
        write->append(" grind");

        if (*write != "daily grind") {
            return 3;
        }
    }

    if (*title != "daily grind") {
        return 4;
    }

    memsafe::Owner<int> source(42);
    memsafe::Owner<int> destination(std::move(source));

    if (source.has_value()) {
        return 5;
    }

    if (!destination.has_value() || *destination != 42) {
        return 6;
    }

    return 0;
}
