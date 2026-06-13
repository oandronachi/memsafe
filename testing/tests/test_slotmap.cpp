/**
 * @file test_slotmap.cpp
 * @brief Unit coverage for Slice 2 `memsafe::SlotMap<T>` generational handles.
 *
 * @details
 * Work package: CPP_MEMSAFE-0210-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0205-FUNC bounded `SlotMap<T>` allocator
 *   through its public API only.
 * - Allocate exactly the configured capacity, prove an additional allocation
 *   reports `violation_kind::capacity_exhausted`, then free and reallocate one
 *   slot.
 * - Verify the freed slot index is reused with the next per-slot generation and
 *   that the stale handle is rejected by the THROW violation policy.
 *
 * Key invariants:
 * - The test never inspects `SlotMap<T>` private free-list state; slot reuse is
 *   observed through `Handle<T>::index()` and generation advancement through
 *   `Handle<T>::generation()`.
 * - The over-capacity case is checked while the map is full, satisfying F2
 *   generational-handle case 3 without relying on allocator internals.
 * - The stale-handle case dereferences only after reallocation of the same
 *   slot, so a false positive caused by a missing generation bump would be
 *   visible as a non-throwing access to the new payload.
 *
 * Ownership and thread-safety:
 * - All state is process-local automatic storage owned by this test executable.
 * - No worker threads are started; concurrency and Treiber interleaving coverage
 *   belong to later Slice 2 work packages.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the THROW violation policy for this SlotMap unit test.
 *
 * @retval MEMSAFE_VIOLATION_THROW Every `MEMSAFE_DETAIL_VIOLATE` report in this
 * translation unit throws `memsafe::violation`.
 * @pre Define this macro before including any memsafe header that consumes the
 * violation policy.
 * @post Capacity exhaustion and stale-handle reports can be asserted without
 * terminating the test process.
 * @invariant The test observes the same policy hook used by production
 * `SlotMap<T>` code rather than calling the violation machinery directly.
 * @throws Nothing directly; later violation reports throw
 * `memsafe::violation`.
 * @note Ownership/thread-safety: this is compile-time configuration and owns no
 * runtime state.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../test_harness.hpp"

#include <memsafe/handle.hpp>

#include <array>
#include <cstddef>

namespace {

/// Number of slots used by this bounded unit-test map.
constexpr std::size_t k_slot_count = 3U;

/**
 * @brief Payload stored in `SlotMap` slots by this unit test.
 *
 * @pre Construct with an integer value meaningful to the active test case.
 * @post Instances own one integer payload and no external resource.
 * @invariant The type has a non-throwing constructor and destructor so every
 * observed exception comes from the memsafe violation policy, not the payload.
 * @throws Nothing; construction is `noexcept` and destruction is implicit.
 * @note Ownership/thread-safety: each instance is owned by exactly one
 * `SlotMap` slot. The test uses it on one thread only.
 */
struct tracked_payload final {
    /**
     * @brief Construct a payload with the supplied observable value.
     *
     * @param initial_value Integer value stored in the payload.
     * @return No value; constructors initialize the receiving object.
     * @pre No precondition beyond ordinary integer value validity.
     * @post `value == initial_value`.
     * @invariant Construction does not allocate and cannot mask SlotMap
     * violation-policy exceptions.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: initializes only the receiving object.
     */
    explicit tracked_payload(int initial_value) noexcept : value(initial_value) {}

    /// Observable payload value used to prove the right slot contents are live.
    int value;
};

/// SlotMap specialization under test.
using slot_map_type = memsafe::SlotMap<tracked_payload, k_slot_count>;

/// Handle type issued by the SlotMap specialization under test.
using handle_type = slot_map_type::handle_type;

/// Generation component type used by the tested handle values.
using generation_type = handle_type::generation_type;

/**
 * @brief Captured result of invoking code expected to report a violation.
 *
 * @pre A default-constructed capture represents "no memsafe violation thrown".
 * @post Test helpers fill the fields after running one operation.
 * @invariant `kind` and `message` are meaningful only when
 * `threw_memsafe_violation == true`.
 * @throws Nothing; the aggregate owns no dynamic resources.
 * @note Ownership/thread-safety: the message pointer is borrowed from the
 * caught `memsafe::violation` object and inspected synchronously before the
 * next operation.
 */
struct violation_capture final {
    /// True when the operation threw `memsafe::violation`.
    bool threw_memsafe_violation = false;
    /// True when the operation threw some non-memsafe exception type.
    bool threw_unexpected_exception = false;
    /// Violation kind observed from `memsafe::violation::kind()`.
    memsafe::violation_kind kind = memsafe::violation_kind::null_access;
    /// Borrowed diagnostic string observed from `memsafe::violation::message()`.
    const char* message = nullptr;
};

/**
 * @brief Return whether a borrowed diagnostic string is present.
 *
 * @param text Borrowed C string pointer; null is accepted.
 * @retval true `text` is non-null and begins with a non-null character.
 * @retval false `text` is null or empty.
 * @pre If `text` is non-null, it points to a valid null-terminated string.
 * @post No state is modified.
 * @invariant The helper never dereferences a null pointer.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the pointer is borrowed only for this
 * synchronous check.
 */
bool has_text(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

/**
 * @brief Run an operation and capture the violation outcome.
 *
 * @tparam Operation Callable type invocable with no arguments.
 * @param operation Operation expected to trigger the active THROW policy.
 * @return Captured memsafe violation kind/message or unexpected-exception
 * marker.
 * @pre `operation` is callable exactly once and any referenced test objects
 * outlive this call.
 * @post The operation has been invoked. No exception escapes this helper.
 * @invariant The helper does not synthesize violations; it records only the
 * result of public `SlotMap<T>` operations.
 * @throws Nothing; all exceptions from `operation` are caught.
 * @note Ownership/thread-safety: the callable and any captures are used on the
 * calling thread only.
 *
 * Example:
 * @code
 * auto capture = capture_violation([&map] {
 *     const auto h = map.allocate(42);
 *     (void)h;
 * });
 * @endcode
 */
template <typename Operation>
violation_capture capture_violation(Operation operation) noexcept {
    violation_capture capture;
    try {
        operation();
    } catch (const memsafe::violation& caught) {
        capture.threw_memsafe_violation = true;
        capture.kind = caught.kind();
        capture.message = caught.message();
    } catch (...) {
        capture.threw_unexpected_exception = true;
    }
    return capture;
}

/**
 * @brief Assert that a captured operation produced a specific violation kind.
 *
 * @param capture Captured outcome to inspect.
 * @param expected_kind Violation kind required by the current test case.
 * @return Nothing.
 * @pre `capture` was returned by `capture_violation`.
 * @post Harness failures are recorded for missing, unexpected, wrong-kind, or
 * empty-message outcomes.
 * @invariant The helper checks both type and kind so a generic throw cannot
 * satisfy the SlotMap unit-test acceptance criteria.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads only caller-owned capture data and
 * writes to the process-local test harness failure count.
 */
void expect_violation_kind(const violation_capture& capture,
                           memsafe::violation_kind expected_kind) noexcept {
    CHECK(capture.threw_memsafe_violation);
    CHECK(!capture.threw_unexpected_exception);
    if (capture.threw_memsafe_violation) {
        CHECK(capture.kind == expected_kind);
        CHECK(has_text(capture.message));
    }
}

/**
 * @brief Return the next generation expected after one successful deallocation.
 *
 * @param generation Generation observed on the handle before deallocation.
 * @return `generation + 1` in the unsigned handle-generation domain.
 * @pre `generation` is not the reserved invalid generation and is not the
 * maximum representable value in this deterministic unit test.
 * @post No state is modified.
 * @invariant The test uses early slot generations only, so wraparound and the
 * reserved zero generation are outside this unit case and remain covered by
 * lower-level generation primitive tests.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: pure scalar calculation.
 */
generation_type next_generation_after(generation_type generation) noexcept {
    return static_cast<generation_type>(generation + generation_type{1U});
}

/**
 * @brief Allocate the full map and verify the bounded-capacity violation.
 *
 * @param map SlotMap instance to fill to capacity.
 * @param handles Output array receiving each issued handle.
 * @return Nothing.
 * @pre `map.size() == 0` and `handles.size() == k_slot_count`.
 * @post `map.size() == map.capacity() == k_slot_count`; every handle in
 * `handles` is intrinsically valid, and an extra allocation has thrown
 * `capacity_exhausted`.
 * @invariant This directly covers F2 generational-handle case 3 while leaving
 * the map full for the reuse test.
 * @throws Nothing intentionally; unexpected payload or violation exceptions
 * escape as test-process failures.
 * @note Ownership/thread-safety: `map` and `handles` are borrowed by one test
 * thread for the duration of the call.
 */
void fill_to_capacity_and_check_exhaustion(
    slot_map_type& map,
    std::array<handle_type, k_slot_count>& handles) {
    CHECK(map.capacity() == k_slot_count);
    CHECK(map.size() == 0U);

    for (std::size_t i = 0U; i < handles.size(); ++i) {
        handles[i] = map.allocate(static_cast<int>(100U + i));
        CHECK(handles[i].is_valid());
        CHECK(map.deref(handles[i]).value == static_cast<int>(100U + i));
    }

    CHECK(map.size() == k_slot_count);

    const violation_capture exhausted = capture_violation([&map] {
        /*
         * F2 case 3 requires bounded capacity to fail through the violation
         * policy. Keeping the returned handle in a local avoids nodiscard
         * diagnostics in compilers that warn on discarded allocation results.
         */
        const handle_type extra = map.allocate(999);
        (void)extra;
    });
    expect_violation_kind(exhausted, memsafe::violation_kind::capacity_exhausted);
    CHECK(map.size() == k_slot_count);
}

/**
 * @brief Deallocate one handle, reallocate, and verify generation safety.
 *
 * @param map Full SlotMap instance produced by
 * `fill_to_capacity_and_check_exhaustion`.
 * @param handles Handles for every live slot in `map`.
 * @return Nothing.
 * @pre `map.size() == k_slot_count`, `handles[1]` names a live slot, and
 * `handles[0]` plus `handles[2]` remain live through the call.
 * @post The slot previously named by `handles[1]` has been reused with an
 * incremented generation, the stale handle has been rejected under checks-on
 * builds, and unrelated live handles still read their original payloads.
 * @invariant The stale dereference is attempted after reuse so the test proves
 * generation comparison, not merely an occupied-bit check, protects the slot.
 * @throws Nothing intentionally; unexpected exceptions escape as
 * test-process failures.
 * @note Ownership/thread-safety: all operations are single-threaded and use
 * only the public SlotMap API.
 */
void check_reuse_generation_bump_and_stale_deref(
    slot_map_type& map,
    const std::array<handle_type, k_slot_count>& handles) {
    const handle_type stale = handles[1U];
    const generation_type old_generation = stale.generation();

    map.deallocate(stale);
    CHECK(map.size() == k_slot_count - 1U);

    const handle_type reused = map.allocate(777);
    CHECK(map.size() == k_slot_count);
    CHECK(reused.is_valid());
    CHECK(reused.index() == stale.index());
    CHECK(reused.generation() != old_generation);
    CHECK(reused.generation() == next_generation_after(old_generation));
    CHECK(map.deref(reused).value == 777);

#if MEMSAFE_RELEASE_CHECKS
    const violation_capture stale_deref = capture_violation([&map, stale] {
        /*
         * F2 case 2 is intentionally checked after the index has been reused:
         * a SlotMap that forgot to bump the per-slot generation would return
         * the new payload instead of throwing here.
         */
        const tracked_payload& value = map.deref(stale);
        (void)value;
    });
    expect_violation_kind(stale_deref, memsafe::violation_kind::use_after_free);
#else
    /*
     * When runtime generation checks are compiled out, `get()` is the safe
     * observation point: `deref()` would have no object to reference after the
     * check-free invalid path returns null.
     */
    CHECK(map.get(stale) == nullptr);
#endif

    CHECK(map.deref(handles[0U]).value == 100);
    CHECK(map.deref(handles[2U]).value == 102);
}

} // namespace

/**
 * @brief Run the SlotMap generational-handle unit tests.
 *
 * @retval 0 All SlotMap unit checks passed.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built by the F4 convention-based runtime-test lane
 * with project headers on the include path.
 * @post The test harness prints a summary and returns its exit-code verdict.
 * @invariant Exactly one bounded map is exercised through public API calls for
 * CPP_MEMSAFE-0210-TEST: allocate-to-capacity, capacity exhaustion, free/reuse,
 * generation bump, and stale-handle dereference violation.
 * @throws Nothing intentionally; unexpected exceptions are allowed to terminate
 * the process as validation failures.
 * @note Ownership/thread-safety: test state is automatic storage in one
 * process and one thread.
 */
int main() {
    slot_map_type map;
    std::array<handle_type, k_slot_count> handles{};

    fill_to_capacity_and_check_exhaustion(map, handles);
    check_reuse_generation_bump_and_stale_deref(map, handles);

    RUN_TESTS("test_slotmap");
}
