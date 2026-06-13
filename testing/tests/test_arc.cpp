/**
 * @file test_arc.cpp
 * @brief Unit coverage for Slice 5 `memsafe::Arc<T>` shared ownership.
 *
 * @details
 * Work package: CPP_MEMSAFE-0510-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0500-FUNC `memsafe::Arc<T>` API through
 *   every public operation: construction, destruction, copy/move construction,
 *   copy/move assignment, `has_value()`, `operator bool`, `strong_count()`,
 *   `use_count()`, `get()`, `operator*`, `operator->`, and `reset()`.
 * - Verify the F1 Slice 5 Arc requirement that copies share one atomic strong
 *   count and one payload rather than cloning the payload.
 * - Verify the F2 Arc unit-test oracle that the payload is destroyed exactly
 *   once, and only when the final strong owner releases the control block.
 * - Verify the reachable library-managed Arc violation path: empty dereference
 *   reports `violation_kind::null_access` under the THROW policy when release
 *   checks are enabled.
 *
 * Key invariants:
 * - The test never inspects `memsafe::detail::arc_ctrl<T>` internals. Strong
 *   ownership is observed only through public count snapshots and payload
 *   address identity.
 * - Destruction is observed through an instrumented payload because an `int`
 *   payload would make the final-release event invisible.
 * - Every copy that should retain ownership is kept in a lexical scope with
 *   explicit count checks before and after the scope ends.
 * - Empty dereference negative tests are compiled only through public Arc
 *   operations and are guarded by `MEMSAFE_RELEASE_CHECKS`, matching the
 *   library contract that no-check lanes omit the runtime null branch.
 *
 * Ownership and thread-safety:
 * - All test state is automatic storage owned by this process and used on one
 *   thread. Cross-thread interleavings belong to CPP_MEMSAFE-0530-TEST.
 * - The payload ledger outlives every `tracked_payload` that points at it, so
 *   destructor observation does not introduce dangling test state.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select THROW policy so Arc negative unit cases can assert violations.
 *
 * @retval MEMSAFE_VIOLATION_THROW Causes library-managed Arc invariant
 * violations in this translation unit to throw `memsafe::violation`.
 * @pre This macro must be defined before any memsafe header includes
 * `<memsafe/violation.hpp>`.
 * @post Empty Arc dereference can be checked without terminating the test
 * process.
 * @invariant The selection affects only this standalone test translation unit.
 * @throws Nothing directly; the selected policy controls later library
 * violation reports.
 * @note Ownership/thread-safety: this preprocessor selector owns no runtime
 * state and does not install a global handler.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../test_harness.hpp"

#include <memsafe/sync.hpp>
#include <memsafe/violation.hpp>

#include <cstring>
#include <utility>

namespace {

/**
 * @brief Mutable ledger that records construction and destruction events.
 *
 * @pre A ledger object must outlive every `tracked_payload` constructed with
 * its address.
 * @post Payload construction and destruction update the counters directly.
 * @invariant `destroyed` must not exceed `constructed` in a passing test, and
 * `last_destroyed_payload` records the value stored by the most recently
 * destroyed payload.
 * @throws This aggregate owns only integers and does not throw.
 * @note Ownership/thread-safety: stack-owned by the active unit-test function
 * and touched only on the main test thread.
 */
struct destruction_ledger final {
    /// Number of payload constructors that completed successfully.
    int constructed = 0;
    /// Number of payload destructors observed so far.
    int destroyed = 0;
    /// Value stored in the most recently destroyed payload.
    int last_destroyed_payload = 0;
};

/**
 * @brief Arc-managed payload with observable lifetime and shared mutation.
 *
 * @details
 * `memsafe::Arc<T>` copies must retain the same control block rather than
 * copying `T`. This payload is intentionally non-copyable so the test proves
 * shared ownership through `Arc<T>` handle copies, while its setter lets one
 * handle mutate state that another handle must observe.
 *
 * Example:
 * @code
 * destruction_ledger ledger;
 * {
 *     memsafe::Arc<tracked_payload> first(ledger, 7);
 *     auto second = first;
 *     second->set_value(9);
 *     CHECK((*first).value() == 9);
 * }
 * CHECK(ledger.destroyed == 1);
 * @endcode
 *
 * @pre The referenced `destruction_ledger` outlives the payload.
 * @post Construction increments `constructed`; destruction increments
 * `destroyed` once and records the last stored payload value.
 * @invariant `ledger_` is a stable non-owning pointer for the payload lifetime.
 * @throws Nothing; construction, destruction, observation, and mutation are
 * `noexcept`.
 * @note Ownership/thread-safety: instances are owned by one `Arc` control
 * block. The unit test performs payload mutation on one thread only.
 */
class tracked_payload final {
public:
    /**
     * @brief Construct a tracked payload and record the construction event.
     *
     * @param ledger Ledger that receives construction and destruction counts.
     * @param initial_value Integer payload value retained by the object.
     * @return Constructors do not return a value.
     * @pre `ledger` outlives this payload.
     * @post `ledger.constructed` has increased by one and `value()` returns
     * `initial_value`.
     * @invariant The ledger pointer and payload value remain valid until the
     * destructor runs.
     * @throws Nothing.
     * @note Ownership/thread-safety: initialization touches only caller-owned
     * ledger state on the constructing thread.
     */
    tracked_payload(destruction_ledger& ledger, int initial_value) noexcept
        : ledger_(&ledger), value_(initial_value) {
        ++ledger_->constructed;
    }

    /**
     * @brief Record one payload destruction event.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post `ledger_->destroyed` has increased by one and
     * `last_destroyed_payload` equals the final stored value.
     * @invariant C++ invokes this destructor at most once for this object; the
     * ledger converts that language guarantee into an observable test oracle.
     * @throws Nothing.
     * @note Ownership/thread-safety: called by the final `Arc<T>` release in
     * this single-threaded unit test.
     */
    ~tracked_payload() noexcept {
        ++ledger_->destroyed;
        ledger_->last_destroyed_payload = value_;
    }

    /**
     * @brief Return the current observable payload value.
     *
     * @return Integer value stored in the payload.
     * @pre The payload object is alive.
     * @post No state is modified.
     * @invariant All `Arc<tracked_payload>` copies that share this object must
     * observe the same value.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; callers must still obey
     * ordinary C++ data-race rules for shared payload access.
     */
    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    /**
     * @brief Replace the observable payload value.
     *
     * @param next_value New value to store in the payload.
     * @return Nothing.
     * @pre The payload object is alive and the caller has exclusive logical
     * access for mutation.
     * @post `value()` returns `next_value`.
     * @invariant Mutating through one `Arc<T>` handle must be visible through
     * every copied handle because all handles share one payload address.
     * @throws Nothing.
     * @note Ownership/thread-safety: this test uses the setter only
     * single-threaded; `Arc<T>` itself does not serialize mutable payload
     * access.
     */
    void set_value(int next_value) noexcept {
        value_ = next_value;
    }

private:
    tracked_payload(const tracked_payload& other) = delete;
    tracked_payload& operator=(const tracked_payload& other) = delete;

    /// Non-owning pointer to the ledger that outlives this payload.
    destruction_ledger* ledger_;
    /// Mutable value used to prove all `Arc<T>` copies share one payload.
    int value_;
};

/**
 * @brief Return whether a borrowed C string has visible content.
 *
 * @param text String pointer to inspect; null is accepted.
 * @return `true` when `text` is non-null and begins with a non-NUL character;
 * otherwise `false`.
 * @pre Non-null pointers must name valid NUL-terminated strings.
 * @post No state is modified.
 * @invariant The helper never dereferences a null pointer.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the string is borrowed only for the duration
 * of the call.
 */
bool has_text(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

/**
 * @brief Compare two borrowed C strings for exact equality.
 *
 * @param lhs Left-hand string pointer; null is accepted.
 * @param rhs Right-hand string pointer; null is accepted.
 * @return `true` when both pointers are non-null and their string contents
 * compare equal; otherwise `false`.
 * @pre Non-null pointers must name valid NUL-terminated strings.
 * @post No state is modified.
 * @invariant `std::strcmp` is called only after both pointers pass the null
 * check.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: all string data is borrowed synchronously and
 * no shared state is touched.
 */
bool same_text(const char* lhs, const char* rhs) noexcept {
    return lhs != nullptr && rhs != nullptr && std::strcmp(lhs, rhs) == 0;
}

/**
 * @brief Verify the exception payload produced by empty Arc dereference.
 *
 * @param caught Exception thrown by `memsafe::Arc<T>::operator*()` or
 * `memsafe::Arc<T>::operator->()` on an empty handle.
 * @return Nothing.
 * @pre The exception was produced by a public Arc dereference operation in
 * this translation unit.
 * @post The harness records any mismatch in kind, policy, message, or source
 * metadata as a failure.
 * @invariant F2 negative unit tests must assert the configured violation path,
 * not merely observe that control left the expression.
 * @throws Nothing intentionally; all checks are converted to harness failures.
 * @note Ownership/thread-safety: the exception object is inspected
 * synchronously before it goes out of scope.
 */
void verify_arc_null_access_payload(const memsafe::violation& caught) {
    const memsafe::violation_info& info = caught.info();

    CHECK(caught.kind() == memsafe::violation_kind::null_access);
    CHECK(info.kind == memsafe::violation_kind::null_access);
    CHECK(info.policy == memsafe::violation_policy::throw_exception);

    CHECK(has_text(caught.what()));
    CHECK(has_text(caught.message()));
    CHECK(has_text(info.message));
    CHECK(same_text(caught.what(), "cannot dereference an empty Arc"));
    CHECK(same_text(caught.message(), "cannot dereference an empty Arc"));
    CHECK(same_text(info.message, "cannot dereference an empty Arc"));

    CHECK(has_text(caught.file()));
    CHECK(caught.line() > 0);
    CHECK(has_text(caught.function()));
    CHECK(has_text(info.file));
    CHECK(info.line > 0);
    CHECK(has_text(info.function));
}

/**
 * @brief Verify that default-constructed Arc handles are empty and count zero.
 *
 * @return Nothing.
 * @pre The executable is built with the project include directory and F4
 * standalone harness.
 * @post Harness failures are recorded for any non-empty default handle or
 * nonzero count snapshot.
 * @invariant Empty handles own no release obligation and therefore cannot
 * affect any payload lifetime ledger.
 * @throws Nothing intentionally; unexpected exceptions would fail the test
 * process under the runner's exit-code model.
 * @note Ownership/thread-safety: all observed state belongs to one local empty
 * handle on the main test thread.
 */
void verify_default_empty_arc() {
    memsafe::Arc<tracked_payload> empty;

    CHECK(!empty.has_value());
    CHECK(!empty);
    CHECK(empty.get() == nullptr);
    CHECK(empty.strong_count() == 0U);
    CHECK(empty.use_count() == 0U);

    empty.reset();
    CHECK(!empty.has_value());
    CHECK(empty.get() == nullptr);
    CHECK(empty.strong_count() == 0U);
    CHECK(empty.use_count() == 0U);
}

/**
 * @brief Verify copy and move operations from empty Arc handles stay empty.
 *
 * @return Nothing.
 * @pre Empty source handles are live for the duration of the copy and move
 * operations.
 * @post Every destination created from or assigned from an empty source reports
 * no payload and no strong count.
 * @invariant Empty-handle operations are the negative public-operation cases:
 * they must be no-ops for refcounts and never create or destroy a payload.
 * @throws Nothing intentionally; unexpected exceptions fail the process under
 * the runner's exit-code model.
 * @note Ownership/thread-safety: all handles are stack-local and used on the
 * main test thread.
 */
void verify_empty_arc_copy_move_and_assignment_cases() {
    memsafe::Arc<tracked_payload> empty;
    memsafe::Arc<tracked_payload> copy_constructed(empty);

    CHECK(!copy_constructed.has_value());
    CHECK(copy_constructed.get() == nullptr);
    CHECK(copy_constructed.strong_count() == 0U);

    memsafe::Arc<tracked_payload> move_constructed(std::move(empty));

    CHECK(!empty.has_value());
    CHECK(!move_constructed.has_value());
    CHECK(move_constructed.use_count() == 0U);

    destruction_ledger assigned_away_ledger;
    memsafe::Arc<tracked_payload> assigned_away(assigned_away_ledger, 88);
    memsafe::Arc<tracked_payload> empty_source;

    CHECK(assigned_away.strong_count() == 1U);
    assigned_away = empty_source;
    CHECK(!assigned_away.has_value());
    CHECK(assigned_away.get() == nullptr);
    CHECK(assigned_away.strong_count() == 0U);
    CHECK(assigned_away_ledger.destroyed == 1);
    CHECK(assigned_away_ledger.last_destroyed_payload == 88);

    destruction_ledger move_assigned_away_ledger;
    memsafe::Arc<tracked_payload> move_assigned_away(
        move_assigned_away_ledger, 91);
    memsafe::Arc<tracked_payload> empty_move_source;

    CHECK(move_assigned_away.strong_count() == 1U);
    move_assigned_away = std::move(empty_move_source);
    CHECK(!move_assigned_away.has_value());
    CHECK(move_assigned_away.get() == nullptr);
    CHECK(move_assigned_away.use_count() == 0U);
    CHECK(move_assigned_away_ledger.destroyed == 1);
    CHECK(move_assigned_away_ledger.last_destroyed_payload == 91);
}

/**
 * @brief Verify copy construction shares payload and adjusts strong counts.
 *
 * @return Nothing.
 * @pre `tracked_payload` is constructible from a ledger reference and integer
 * value, and every created `Arc<tracked_payload>` remains in lexical scope
 * until its expected release point.
 * @post The harness records failures for count mismatches, payload-address
 * mismatches, missing shared mutation, premature destruction, or missing final
 * destruction.
 * @invariant F1 Slice 5 requires Arc copies and destroys to adjust the atomic
 * count while the payload is deleted only at count zero; this function checks
 * the visible count sequence 1 -> 2 -> 3 -> 2 -> 1 -> 0.
 * @throws Nothing intentionally; allocation or unexpected library exceptions
 * are allowed to terminate the executable as validation failures.
 * @note Ownership/thread-safety: all Arc handles are local objects used on one
 * thread; concurrency belongs to the later Slice 5 concurrency work package.
 */
void verify_copy_construction_refcounts_and_final_release() {
    destruction_ledger ledger;

    {
        memsafe::Arc<tracked_payload> root(ledger, 41);
        tracked_payload* const root_payload = root.get();

        CHECK(root.has_value());
        CHECK(static_cast<bool>(root));
        CHECK(root_payload != nullptr);
        CHECK(root.strong_count() == 1U);
        CHECK(root.use_count() == root.strong_count());
        CHECK((*root).value() == 41);
        CHECK(root->value() == 41);
        CHECK(ledger.constructed == 1);
        CHECK(ledger.destroyed == 0);

        {
            memsafe::Arc<tracked_payload> first_copy(root);

            CHECK(first_copy.has_value());
            CHECK(first_copy.get() == root_payload);
            CHECK(root.strong_count() == 2U);
            CHECK(root.use_count() == 2U);
            CHECK(first_copy.strong_count() == 2U);
            CHECK(first_copy.use_count() == 2U);

            /*
             * F1 Slice 5 models Arc as shared ownership of one payload, not a
             * copy-on-write or value-cloning owner. A mutation through the copy
             * must be visible through the original handle.
             */
            (*first_copy).set_value(73);
            CHECK(root->value() == 73);

            {
                memsafe::Arc<tracked_payload> second_copy(first_copy);

                CHECK(second_copy.has_value());
                CHECK(static_cast<bool>(second_copy));
                CHECK(second_copy.get() == root_payload);
                CHECK(root.strong_count() == 3U);
                CHECK(first_copy.use_count() == 3U);
                CHECK(second_copy.strong_count() == 3U);
                CHECK(second_copy.use_count() == 3U);
                CHECK((*second_copy).value() == 73);
                CHECK(second_copy->value() == 73);
                CHECK(ledger.destroyed == 0);
            }

            CHECK(root.strong_count() == 2U);
            CHECK(first_copy.strong_count() == 2U);
            CHECK(ledger.destroyed == 0);
        }

        CHECK(root.strong_count() == 1U);
        CHECK(root->value() == 73);
        CHECK(ledger.destroyed == 0);

        root.reset();
        CHECK(!root.has_value());
        CHECK(root.get() == nullptr);
        CHECK(root.strong_count() == 0U);
        CHECK(root.use_count() == 0U);
        CHECK(ledger.destroyed == 1);
        CHECK(ledger.last_destroyed_payload == 73);
    }

    CHECK(ledger.constructed == 1);
    CHECK(ledger.destroyed == 1);
}

/**
 * @brief Verify copy assignment retains the new block and releases the old one.
 *
 * @return Nothing.
 * @pre Source and destination Arc handles are live and distinct when the copy
 * assignment is performed.
 * @post The destination shares the source payload, the old destination payload
 * has been destroyed exactly once, and the shared payload is not destroyed
 * until the final shared owner resets.
 * @invariant Copy assignment must retain the incoming control block and
 * release the outgoing control block exactly once, preserving the F1 Slice 5
 * strong-count accounting across reassignment.
 * @throws Nothing intentionally; allocation or unexpected library exceptions
 * fail the process under the runner's exit-code contract.
 * @note Ownership/thread-safety: assignment mutates one handle object on the
 * main test thread; atomic cross-thread retain/release stress belongs to
 * CPP_MEMSAFE-0530-TEST.
 */
void verify_copy_assignment_releases_replaced_payload_once() {
    destruction_ledger shared_ledger;
    destruction_ledger replaced_ledger;

    memsafe::Arc<tracked_payload> shared(shared_ledger, 100);
    memsafe::Arc<tracked_payload> shared_copy(shared);
    tracked_payload* const shared_payload = shared.get();

    CHECK(shared.strong_count() == 2U);
    CHECK(shared_copy.strong_count() == 2U);
    CHECK(shared_ledger.destroyed == 0);

    shared = shared;
    CHECK(shared.get() == shared_payload);
    CHECK(shared.strong_count() == 2U);
    CHECK(shared_copy.strong_count() == 2U);
    CHECK(shared_ledger.destroyed == 0);

    {
        memsafe::Arc<tracked_payload> target(replaced_ledger, 5);

        CHECK(target.strong_count() == 1U);
        CHECK(target->value() == 5);
        CHECK(replaced_ledger.constructed == 1);
        CHECK(replaced_ledger.destroyed == 0);

        target = shared;

        CHECK(target.has_value());
        CHECK(static_cast<bool>(target));
        CHECK(target.get() == shared_payload);
        CHECK(target.strong_count() == 3U);
        CHECK(shared.strong_count() == 3U);
        CHECK(shared_copy.strong_count() == 3U);
        CHECK(target->value() == 100);
        CHECK(replaced_ledger.destroyed == 1);
        CHECK(replaced_ledger.last_destroyed_payload == 5);

        target->set_value(144);
        CHECK((*shared).value() == 144);
        CHECK(shared_copy->value() == 144);
    }

    CHECK(shared.strong_count() == 2U);
    CHECK(shared_copy.strong_count() == 2U);
    CHECK(shared_ledger.destroyed == 0);
    CHECK(replaced_ledger.destroyed == 1);

    shared_copy.reset();
    CHECK(shared.strong_count() == 1U);
    CHECK(shared_ledger.destroyed == 0);

    shared.reset();
    CHECK(!shared.has_value());
    CHECK(shared_ledger.constructed == 1);
    CHECK(shared_ledger.destroyed == 1);
    CHECK(shared_ledger.last_destroyed_payload == 144);
}

/**
 * @brief Verify move operations transfer ownership without changing counts.
 *
 * @return Nothing.
 * @pre The source Arc handles are live and not concurrently accessed while
 * they are moved.
 * @post Moved-from handles are empty, the destination owns the original
 * payload, replaced move-assignment payloads are released once, and exactly one
 * original payload destruction occurs after the last reset.
 * @invariant Moving an `Arc<T>` transfers an existing release obligation; it
 * must not retain a new reference or drop the payload early.
 * @throws Nothing intentionally; unexpected exceptions fail the process under
 * the runner's exit-code contract.
 * @note Ownership/thread-safety: moves are handle-object mutations performed
 * on one thread with ordinary C++ sequencing.
 */
void verify_moves_do_not_create_extra_references() {
    destruction_ledger ledger;

    memsafe::Arc<tracked_payload> source(ledger, 12);
    tracked_payload* const payload = source.get();

    CHECK(source.strong_count() == 1U);

    memsafe::Arc<tracked_payload> moved_constructed(std::move(source));

    CHECK(!source.has_value());
    CHECK(source.strong_count() == 0U);
    CHECK(moved_constructed.get() == payload);
    CHECK(moved_constructed.strong_count() == 1U);
    CHECK(ledger.destroyed == 0);

    destruction_ledger replaced_ledger;
    memsafe::Arc<tracked_payload> moved_assigned(replaced_ledger, 33);

    CHECK(moved_assigned.strong_count() == 1U);
    moved_assigned = std::move(moved_constructed);

    CHECK(!moved_constructed.has_value());
    CHECK(moved_constructed.strong_count() == 0U);
    CHECK(moved_assigned.get() == payload);
    CHECK(moved_assigned.strong_count() == 1U);
    CHECK(moved_assigned->value() == 12);
    CHECK(ledger.destroyed == 0);
    CHECK(replaced_ledger.constructed == 1);
    CHECK(replaced_ledger.destroyed == 1);
    CHECK(replaced_ledger.last_destroyed_payload == 33);

    moved_assigned = std::move(moved_assigned);
    CHECK(moved_assigned.get() == payload);
    CHECK(moved_assigned.strong_count() == 1U);
    CHECK(ledger.destroyed == 0);

    moved_assigned.reset();
    CHECK(ledger.constructed == 1);
    CHECK(ledger.destroyed == 1);
    CHECK(ledger.last_destroyed_payload == 12);
}

/**
 * @brief Verify empty Arc dereference reports the null-access violation path.
 *
 * @return Nothing.
 * @pre This translation unit selects `MEMSAFE_VIOLATION_THROW` before including
 * memsafe headers.
 * @post When release checks are enabled, both `operator*` and `operator->`
 * on an empty Arc are observed to throw `memsafe::violation` with
 * `violation_kind::null_access`. In no-check lanes the test verifies the empty
 * state only and deliberately avoids undefined null dereference.
 * @invariant F2 requires negative tests for library-managed invariants. Arc's
 * public dereference operations are the reachable null-owner invariant checks.
 * @throws Nothing intentionally; unexpected exception types are converted into
 * harness failures.
 * @note Ownership/thread-safety: no payload is owned in this scenario, and all
 * checks run on the main test thread.
 */
void verify_empty_dereference_reports_null_access_violation() {
    memsafe::Arc<tracked_payload> empty;

    CHECK(!empty.has_value());
    CHECK(empty.get() == nullptr);

#if MEMSAFE_RELEASE_CHECKS
    bool star_threw = false;
    try {
        static_cast<void>(*empty);
        CHECK(false);
    } catch (const memsafe::violation& caught) {
        star_threw = true;
        verify_arc_null_access_payload(caught);
    } catch (...) {
        CHECK(false);
    }
    CHECK(star_threw);

    bool arrow_threw = false;
    try {
        static_cast<void>(empty.operator->());
        CHECK(false);
    } catch (const memsafe::violation& caught) {
        arrow_threw = true;
        verify_arc_null_access_payload(caught);
    } catch (...) {
        CHECK(false);
    }
    CHECK(arrow_threw);
#endif
}

} // namespace

/**
 * @brief Run the Slice 5 Arc unit tests.
 *
 * @retval 0 All Arc unit checks passed.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built by the F4 convention-based runtime-test lane
 * with the project include directory and `testing/test_harness.hpp` available.
 * @post The test harness prints a summary and returns its process verdict.
 * @invariant Coverage is limited to CPP_MEMSAFE-0510-TEST acceptance:
 * shared ownership across copies, correct public refcount snapshots, exactly
 * once payload destruction at final release, and public Arc violation behavior.
 * @throws Nothing intentionally; unexpected exceptions are allowed to
 * terminate the process as validation failures.
 * @note Ownership/thread-safety: all scenarios run on one thread. Arc
 * interleaving and synchronization stress belongs to later Slice 5 TEST work
 * packages.
 */
int main() {
    verify_default_empty_arc();
    verify_empty_arc_copy_move_and_assignment_cases();
    verify_copy_construction_refcounts_and_final_release();
    verify_copy_assignment_releases_replaced_payload_once();
    verify_moves_do_not_create_extra_references();
    verify_empty_dereference_reports_null_access_violation();

    RUN_TESTS("test_arc");
}
