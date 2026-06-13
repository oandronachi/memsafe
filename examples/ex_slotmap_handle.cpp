/**
 * @file ex_slotmap_handle.cpp
 * @brief Standalone smoke example for `memsafe::SlotMap` and
 * `memsafe::Handle`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0900-TEST.
 *
 * Purpose:
 * - Demonstrate the finalized CPP_MEMSAFE-0205-FUNC bounded slot-map allocator
 *   through its public API.
 * - Allocate values, access them through generation-carrying handles, free one
 *   slot, and allocate again to observe slot reuse with a new generation.
 * - Return an ordinary process exit code for the F4 `examples/*.cpp` smoke
 *   discovery path.
 *
 * Key invariants:
 * - Every handle used for `get`, `deref`, or `deallocate` is live and issued by
 *   the same `SlotMap<T, Capacity>` instance.
 * - The stale handle from the deallocated slot is never dereferenced; stale
 *   access is covered by dedicated negative tests, while this example stays on
 *   the valid API path.
 * - The map's capacity is fixed at compile time for the example, matching the
 *   v1 bounded-capacity PRD contract.
 *
 * Ownership and thread-safety:
 * - The `SlotMap` owns all payload lifetimes until each handle is deallocated
 *   or the map is destroyed.
 * - Handle values are non-owning scalar tokens and are used on one thread in
 *   this smoke executable.
 */
#include <memsafe/handle.hpp>

#include <cstddef>

/**
 * @brief Run the slot-map/handle smoke example.
 *
 * @retval 0 The example allocated, accessed, reused, and deallocated slots
 * successfully.
 * @retval 1 The map reported an unexpected initial capacity or size.
 * @retval 2 One of the initial allocations did not return an intrinsically
 * valid handle.
 * @retval 3 The map size did not reflect the two live allocations.
 * @retval 4 Generation-checked lookup did not return the expected first value.
 * @retval 5 Mutable dereference did not update the second value.
 * @retval 6 Deallocation did not reduce the live slot count.
 * @retval 7 Reallocation did not return an intrinsically valid handle.
 * @retval 8 The freed slot was not reused by the immediate subsequent
 * allocation.
 * @retval 9 The reused slot did not advance its generation.
 * @retval 10 The reused slot did not contain the expected payload.
 * @retval 11 Final deallocation did not return the map to an empty state.
 * @pre The executable is built with the project `include/` directory on the
 * compiler include path and a C++17-or-newer compiler.
 * @post Every live slot allocated by this example has been explicitly
 * deallocated before `main` returns.
 * @invariant The example uses only handles that are live in the owning map at
 * the point of access.
 * @throws The example does not intentionally throw. Allocation failure,
 * payload construction failure, or an unexpected library exception is allowed
 * to terminate the smoke executable under the runner's exit-code model.
 * @note Ownership/thread-safety: the map, payloads, and handles are local to
 * `main` and are used on a single thread.
 */
int main() {
    constexpr std::size_t capacity = 2U;

    memsafe::SlotMap<int, capacity> values;
    if (values.capacity() != capacity || values.size() != 0U) {
        return 1;
    }

    memsafe::SlotMap<int, capacity>::handle_type first =
        values.allocate(10);
    memsafe::SlotMap<int, capacity>::handle_type second =
        values.allocate(20);

    if (!first.is_valid() || !second.is_valid()) {
        return 2;
    }

    if (values.size() != capacity) {
        return 3;
    }

    const int* const first_value = values.get(first);
    if (first_value == nullptr || *first_value != 10) {
        return 4;
    }

    values.deref(second) += 2;
    if (values.deref(second) != 22) {
        return 5;
    }

    values.deallocate(first);
    if (values.size() != 1U) {
        return 6;
    }

    /*
     * CPP_MEMSAFE-0205-FUNC makes a freed slot immediately reusable through
     * the Treiber free list. The old handle is intentionally not used after
     * deallocation; only the new handle is valid for the reused slot.
     */
    memsafe::SlotMap<int, capacity>::handle_type third =
        values.allocate(30);

    if (!third.is_valid()) {
        return 7;
    }

    if (third.index() != first.index()) {
        return 8;
    }

    if (third.generation() == first.generation()) {
        return 9;
    }

    if (values.deref(third) != 30) {
        return 10;
    }

    values.deallocate(second);
    values.deallocate(third);

    if (values.size() != 0U) {
        return 11;
    }

    return 0;
}
