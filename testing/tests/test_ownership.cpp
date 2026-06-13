/**
 * @file test_ownership.cpp
 * @brief Runtime unit tests for Slice 1 `Owner`, `Ref`, and `MutRef`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0110-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0100-FUNC ownership primitives through
 *   the public `memsafe::Owner<T>`, `memsafe::Ref<T>`, and
 *   `memsafe::MutRef<T>` API.
 * - Verify that an `Owner<int>` is constructed with a live value and that the
 *   same owner destruction path destroys an instrumented payload exactly once.
 * - Verify F2 Ownership case 2: two immutable `Ref<T>` reads can coexist,
 *   `borrow_mut()` reports `violation_kind::borrow_exclusivity` while either
 *   immutable borrow is live, and `borrow_mut()` succeeds and mutates after the
 *   immutable borrows have ended.
 *
 * Key invariants:
 * - This translation unit forces runtime borrow checks on and selects the
 *   THROW violation policy so the negative borrow case is an in-process oracle.
 * - The immutable-borrow scope ends before the successful mutable borrow is
 *   attempted, proving the counter is released by `Ref<T>` destruction.
 * - The destruction probe ledger is stack-owned and outlives the
 *   `Owner<tracked_payload>` that reports into it.
 *
 * Ownership and thread-safety:
 * - All owners and borrow handles are local to the test process and used on one
 *   thread, matching the unsynchronized Slice 1 contract. Cross-thread
 *   variants are tested by later Slice 4 packages.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force Slice 1 runtime borrow counters for this ownership unit test.
 *
 * @retval 1 Enables the checked branches in `memsafe::Owner<T>` and its borrow
 * handles for every infra lane that compiles this file.
 * @pre This macro must be defined before including any memsafe feature header.
 * @post `Owner<T>::borrow_mut()` reports a violation when immutable borrows are
 * live, even when the surrounding infra lane is a release build.
 * @invariant CPP_MEMSAFE-0110-TEST needs the checked THROW oracle named by the
 * command center; release no-check behavior is covered by the generic release
 * lane and does not change this package's source-level oracle.
 * @throws Nothing directly; it is a preprocessor configuration token.
 * @note Ownership/thread-safety: the macro owns no storage and only affects
 * compile-time selection of check call sites in this translation unit.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception throwing for ownership violation checks.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs Slice 1 borrow-rule violations to
 * throw `memsafe::violation`.
 * @pre This macro must be defined before including `<memsafe/owner.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post The live-`Ref<T>` mutable-borrow attempt can be observed with ordinary
 * C++ exception handling inside the process.
 * @invariant The test uses the canonical Slice 0 policy token rather than a
 * custom hook, keeping the oracle aligned with the F4 harness.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: this preprocessor selection owns no runtime
 * state and does not install a handler.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../test_harness.hpp"

#include <memsafe/owner.hpp>
#include <memsafe/violation.hpp>

#include <string>

namespace {

/**
 * @brief Mutable ledger used to verify that owner destruction releases a
 * payload exactly once.
 *
 * @pre The ledger must outlive every `tracked_payload` that stores its address.
 * @post Construction and destruction of tracked payloads update the counters
 * directly.
 * @invariant `destroyed` never exceeds `constructed` in the successful test
 * path, and `last_destroyed_payload` records the payload value seen by the
 * most recent destructor.
 * @throws This aggregate owns no resources and does not throw.
 * @note Ownership/thread-safety: the ledger is stack-owned by `main` and is
 * touched only by the single runtime-test thread.
 */
struct destructor_counters final {
    /// Number of tracked payload constructors that completed successfully.
    int constructed = 0;
    /// Number of tracked payload destructors observed so far.
    int destroyed = 0;
    /// Payload value recorded by the most recent tracked payload destructor.
    int last_destroyed_payload = 0;
};

/**
 * @brief Owner-managed payload that records construction and destruction.
 *
 * @details
 * `int` has no observable user-defined destructor, so the test separately uses
 * this instrumented payload to prove the same `Owner<T>` destructor path drops
 * its heap object exactly once. The class intentionally has no public copy or
 * move operations; it is constructed in place by `Owner<tracked_payload>`.
 *
 * Example:
 * @code
 * destructor_counters counters;
 * {
 *     memsafe::Owner<tracked_payload> owner(counters, 7);
 * }
 * CHECK(counters.destroyed == 1);
 * @endcode
 *
 * @pre The referenced `destructor_counters` object outlives this payload.
 * @post Construction increments `constructed`; destruction increments
 * `destroyed` once and records the payload value.
 * @invariant `counters_` points to the ledger supplied at construction for the
 * entire object lifetime.
 * @throws Nothing; construction, destruction, and observation are `noexcept`.
 * @note Ownership/thread-safety: instances are uniquely owned by
 * `memsafe::Owner<tracked_payload>` and used on one thread in this test.
 */
class tracked_payload final {
public:
    /**
     * @brief Construct a tracked payload and record the construction event.
     *
     * @param counters Ledger that receives construction and destruction counts.
     * @param payload Integer value retained for destruction verification.
     * @return Constructors do not return a value.
     * @pre `counters` outlives this payload.
     * @post `counters.constructed` has increased by one and `payload()` returns
     * `payload`.
     * @invariant The stored ledger pointer and payload value are stable until
     * destruction.
     * @throws Nothing.
     * @note Ownership/thread-safety: the constructed object is intended to be
     * uniquely owned by one `memsafe::Owner<tracked_payload>`.
     */
    tracked_payload(destructor_counters& counters, int payload) noexcept
        : counters_(&counters), payload_(payload) {
        ++counters_->constructed;
    }

    /**
     * @brief Record one payload destruction event.
     *
     * @return Destructors do not return a value.
     * @pre `counters_` points to a live `destructor_counters` object.
     * @post `destroyed` has increased by one and `last_destroyed_payload`
     * matches this object's stored payload.
     * @invariant Each `tracked_payload` instance records exactly one
     * destruction event when C++ calls its destructor.
     * @throws Nothing.
     * @note Ownership/thread-safety: invoked during single-threaded owner
     * teardown in this test.
     */
    ~tracked_payload() noexcept {
        ++counters_->destroyed;
        counters_->last_destroyed_payload = payload_;
    }

    /**
     * @brief Return the retained integer payload.
     *
     * @return Payload value supplied at construction.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The returned value is immutable for the object's lifetime.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; callers must respect the
     * owning `Owner<tracked_payload>` lifetime.
     */
    [[nodiscard]] int payload() const noexcept {
        return payload_;
    }

private:
    tracked_payload(const tracked_payload&) = delete;
    tracked_payload& operator=(const tracked_payload&) = delete;

    /// Non-owning pointer to the stack ledger that outlives this payload.
    destructor_counters* counters_;
    /// Immutable payload value copied into the destruction ledger.
    int payload_;
};

/**
 * @brief Return whether a diagnostic C string is present and non-empty.
 *
 * @param text Borrowed C string pointer to inspect; null is accepted.
 * @return `true` when `text` is non-null and points at a non-empty string;
 * otherwise `false`.
 * @pre Non-null `text` points to a valid null-terminated string.
 * @post No state is modified.
 * @invariant Null input is handled before the pointer is dereferenced.
 * @throws Nothing.
 * @note Ownership/thread-safety: the pointer is borrowed only for this call and
 * no shared state is touched.
 */
bool has_text(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

/**
 * @brief Verify owner construction and destruction behavior.
 *
 * @return Nothing.
 * @pre The test executable is compiled with the project include directory and
 * the local THROW policy selected.
 * @post The harness records failed checks for any construction, observation,
 * or destruction-count mismatch.
 * @invariant The `Owner<int>` assertion covers the package's concrete integer
 * construction requirement, while `Owner<tracked_payload>` covers the
 * observable destructor-once requirement through the same template owner path.
 * @throws Nothing intentionally; unexpected exceptions surface as process
 * failures under the runner's exit-code model.
 * @note Ownership/thread-safety: all objects are stack-owned and single
 * threaded except for heap payloads uniquely owned by their `Owner<T>`.
 */
void verify_owner_construction_and_destruction() {
    destructor_counters counters;

    {
        memsafe::Owner<int> integer_owner(42);
        CHECK(integer_owner.has_value());
        CHECK(*integer_owner == 42);

        memsafe::Owner<tracked_payload> tracked_owner(counters, 73);
        CHECK(tracked_owner.has_value());
        CHECK(tracked_owner->payload() == 73);
        CHECK(counters.constructed == 1);
        CHECK(counters.destroyed == 0);
    }

    CHECK(counters.constructed == 1);
    CHECK(counters.destroyed == 1);
    CHECK(counters.last_destroyed_payload == 73);
}

/**
 * @brief Verify that a live immutable borrow blocks mutable borrowing.
 *
 * @param owner Owner whose payload currently has live immutable borrows.
 * @return Nothing.
 * @pre At least one `Ref<std::string>` from `owner` is live and runtime checks
 * are enabled.
 * @post The harness records that `borrow_mut()` threw `memsafe::violation`
 * with `violation_kind::borrow_exclusivity`.
 * @invariant A failed mutable borrow attempt must not mutate the string or
 * leave a mutable token behind.
 * @throws Nothing intentionally; the expected `memsafe::violation` is caught
 * and converted into CHECK results.
 * @note Ownership/thread-safety: `owner` is borrowed only by the current thread
 * during the synchronous check.
 */
void verify_live_ref_blocks_mut(memsafe::Owner<std::string>& owner) {
    bool saw_violation = false;

    try {
        memsafe::MutRef<std::string> unexpected_mutable_borrow =
            owner.borrow_mut();
        (void)unexpected_mutable_borrow;
        CHECK(false);
    } catch (const memsafe::violation& caught) {
        saw_violation = true;
        CHECK(caught.kind() == memsafe::violation_kind::borrow_exclusivity);
        CHECK(has_text(caught.message()));
        CHECK(has_text(caught.file()));
        CHECK(caught.line() > 0);
        CHECK(has_text(caught.function()));
    } catch (...) {
        CHECK(false);
    }

    CHECK(saw_violation);
}

/**
 * @brief Verify immutable coexistence, mutable exclusion, and later mutation.
 *
 * @return Nothing.
 * @pre The test executable is compiled with checked ownership and THROW policy
 * selected before memsafe headers are included.
 * @post The harness records failed checks for any borrow-rule mismatch.
 * @invariant The two immutable refs read the same payload concurrently; after
 * both refs leave scope, exactly one mutable borrow can update the payload.
 * @throws Nothing intentionally; expected borrow-rule exceptions are caught
 * and unexpected exceptions surface as process failures.
 * @note Ownership/thread-safety: this is the unsynchronized Slice 1 owner path
 * and is intentionally single-threaded.
 */
void verify_borrow_rules() {
    memsafe::Owner<std::string> owner(std::string("daily"));

    {
        memsafe::Ref<std::string> first_read = owner.borrow();
        memsafe::Ref<std::string> second_read = owner.borrow();

        CHECK(*first_read == "daily");
        CHECK(*second_read == "daily");

        /*
         * F2 Ownership case 2 requires the mutable borrow attempt to be a
         * violation while immutable borrows are live. Keeping both refs in this
         * lexical block proves the checked counter observes multiple readers.
         */
        verify_live_ref_blocks_mut(owner);

        CHECK(*first_read == "daily");
        CHECK(*second_read == "daily");
    }

    {
        memsafe::MutRef<std::string> mutable_read = owner.borrow_mut();
        *mutable_read = "dailygrind";
        CHECK(*mutable_read == "dailygrind");
    }

    CHECK(*owner == "dailygrind");
}

} // namespace

/**
 * @brief Run the Slice 1 ownership unit tests.
 *
 * @return Zero when all ownership checks pass; nonzero when the harness
 * recorded any failed CHECK.
 * @pre The executable is built as a standalone F4 runtime test with
 * `testing/test_harness.hpp` and the project include directory available.
 * @post The test harness prints a summary and returns its process verdict.
 * @invariant Coverage is limited to CPP_MEMSAFE-0110-TEST acceptance: owner
 * construction/destruction and basic immutable/mutable borrow rules.
 * @throws Nothing intentionally; unexpected exceptions are allowed to terminate
 * the process under the runner's exit-code model.
 * @note Ownership/thread-safety: the test starts no worker threads and uses
 * only local owner and borrow objects.
 */
int main() {
    verify_owner_construction_and_destruction();
    verify_borrow_rules();

    RUN_TESTS("test_ownership");
}
