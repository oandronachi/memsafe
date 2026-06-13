/**
 * @file test_sync_concurrency.cpp
 * @brief Relacy Race Detector coverage for `memsafe::SyncOwner<T>` borrow
 * accounting under two-thread `borrow()` / `borrow_mut()` interleavings.
 *
 * @details
 * Work package: CPP_MEMSAFE-0430-TEST.
 *
 * Purpose:
 * - Exercise the Slice 4 `SyncOwner Atomic_Refcount` state machine required by
 *   CPP_MEMSAFE-0400-FUNC under Dmitry Vyukov's Relacy Race Detector, consumed
 *   from the CPP_MEMSAFE-0035-TEST vendor path.
 * - Exhaust all two-thread pairings of shared and mutable sync borrow
 *   lifetimes: shared/shared, shared/mutable, mutable/shared, and
 *   mutable/mutable.
 * - Use Relacy `rl::atomic` operations for the modelled borrow ledger and
 *   Relacy `rl::var` operations for the guarded payload so the Relacy scheduler
 *   owns both the interleaving search and the data-race oracle.
 *
 * Key invariants:
 * - This file has a hard dependency on an upstream Relacy header snapshot at
 *   `testing/tests/concurrency/vendor/relacy/relacy/relacy.hpp`; there is no
 *   README-only probing or local compatibility-shim success path. A clean
 *   checkout without the vendored Relacy Race Detector fails to
 *   build this test instead of producing a false exit-0 verdict.
 * - The modelled ledger mirrors `memsafe::detail::sync_borrow_ctrl<T>`:
 *   `0` means unborrowed, `0x80000000` is the exclusive mutable token, and
 *   lower nonzero values count immutable sync borrows.
 * - Shared borrows may coexist only with shared borrows. A mutable borrow may
 *   be acquired only from the unborrowed state. Every successful acquisition is
 *   released before the Relacy test iteration completes.
 * - Payload reads and writes occur only after the corresponding borrow token is
 *   acquired. If the atomic ledger allowed a shared/mutable overlap, Relacy's
 *   non-atomic `rl::var` payload access would report a data race.
 *
 * Ownership and thread-safety:
 * - The Relacy test owns only test-local model state and starts no operating
 *   system threads. Relacy runs two logical threads and exhaustively schedules
 *   their instrumented operations.
 * - The public `memsafe::SyncOwner<int>` smoke check at the end uses ordinary
 *   single-threaded API calls to tie the Relacy model back to the finalized
 *   public type without reaching into private production members.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force checked borrow accounting on for the public SyncOwner smoke.
 *
 * @retval 1 Runtime borrow checks are enabled for production SyncOwner calls in
 * this translation unit.
 * @pre Define before including `<memsafe/sync.hpp>`.
 * @post `SyncOwner<T>::borrow()` and `borrow_mut()` exercise the checked
 * atomic-borrow ledger when the public smoke test runs.
 * @invariant The concurrency lane validates checked Slice 4 behavior even if a
 * build lane's default would otherwise compile checks out.
 * @throws Nothing directly; the macro only configures inline library code.
 * @note Ownership/thread-safety: preprocessor configuration only.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the throwing violation policy for the public smoke oracle.
 *
 * @retval MEMSAFE_VIOLATION_THROW Borrow-rule violations throw
 * `memsafe::violation`.
 * @pre Define before including `<memsafe/sync.hpp>`.
 * @post Expected public exclusivity failures can be caught in-process.
 * @invariant The smoke test uses a project-defined Slice 0 violation policy,
 * not a test-local handler.
 * @throws Nothing directly; selected production code may throw later.
 * @note Ownership/thread-safety: preprocessor configuration only.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../../test_harness.hpp"

#include <memsafe/sync.hpp>
#include <memsafe/violation.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

/*
 * CPP_MEMSAFE-0430-TEST specifically requires Relacy Race Detector coverage.
 * Include the upstream Relacy core header directly from the dependency's
 * declared vendor path. This intentionally does not include any local
 * compatibility shim and intentionally has no fallback.
 *
 * The include is placed after ordinary project and standard headers because
 * upstream Relacy defines malloc/free/assert instrumentation macros for code
 * under test. This file models the private ledger with `rl::atomic` directly
 * rather than compiling production headers under those macros.
 */
#include "vendor/relacy/relacy/relacy.hpp"

namespace {

/// Maximum number of Relacy schedules allowed for each tiny two-thread case.
constexpr std::size_t k_relacy_iteration_bound = 100000U;

/// Ledger value meaning no sync borrow is live.
constexpr std::uint32_t k_no_borrow = 0U;

/// Ledger high-bit token meaning one exclusive mutable borrow is live.
constexpr std::uint32_t k_mut_token = 0x80000000U;

/// Largest shared-borrow count representable before the mutable token bit.
constexpr std::uint32_t k_max_shared = k_mut_token - 1U;

/**
 * @brief Borrow operation performed by one Relacy logical thread.
 *
 * @pre Values are used as non-type template parameters for a Relacy test case.
 * @post No state is modified by the value itself.
 * @invariant `shared` maps to `SyncOwner<T>::borrow()` and `exclusive` maps to
 * `SyncOwner<T>::borrow_mut()`.
 * @throws Nothing; enumerators own no resource.
 * @note Ownership/thread-safety: immutable scalar metadata.
 */
enum class borrow_op {
    /// Model one immutable sync borrow lifetime.
    shared,
    /// Model one exclusive mutable sync borrow lifetime.
    exclusive
};

/**
 * @brief Coverage flags accumulated across all Relacy schedules.
 *
 * @pre Mutated only by the single-threaded Relacy simulation driver.
 * @post Flags are true once at least one explored schedule reached the named
 * condition.
 * @invariant Flags are monotonic; they never affect model control flow and are
 * not part of the verified shared state.
 * @throws Nothing; this aggregate owns only booleans.
 * @note Ownership/thread-safety: Relacy runs logical threads cooperatively on
 * one host thread, so ordinary non-atomic flag writes are safe here.
 */
struct relacy_coverage final {
    /// Two immutable sync borrows were live together.
    bool two_shared_live = false;
    /// A live shared borrow blocked a mutable acquire.
    bool shared_blocks_mut = false;
    /// A live mutable borrow blocked a shared acquire.
    bool mut_blocks_shared = false;
    /// A live mutable borrow blocked another mutable acquire.
    bool mut_blocks_mut = false;
    /// A shared compare-exchange observed contention and retried.
    bool shared_cas_retry = false;
    /// Public `SyncOwner<int>::borrow_mut()` was rejected by a live SyncRef.
    bool public_shared_blocks_mut = false;
    /// Public `SyncOwner<int>::borrow()` was rejected by a live SyncMutRef.
    bool public_mut_blocks_shared = false;
};

/**
 * @brief Return the process-wide Relacy coverage accumulator.
 *
 * @return Mutable reference to the coverage accumulator.
 * @pre Called only from this single test executable.
 * @post No flag is reset by this accessor.
 * @invariant The accumulator is deliberately outside each Relacy test instance
 * because Relacy constructs fresh instances while enumerating schedules.
 * @throws Nothing; initialization is trivial.
 * @note Ownership/thread-safety: single host thread during Relacy simulation.
 */
relacy_coverage& coverage() noexcept {
    static relacy_coverage cov;
    return cov;
}

/**
 * @brief Relacy test suite for one ordered pair of sync borrow operations.
 *
 * @tparam First Operation assigned to logical thread 0.
 * @tparam Second Operation assigned to logical thread 1.
 * @pre Instantiated only for the four combinations of `borrow_op` values.
 * @post Each Relacy iteration starts from an unborrowed ledger and completed
 * iterations leave the ledger unborrowed again.
 * @invariant The ledger algorithm intentionally mirrors
 * `sync_borrow_ctrl<T>::acquire_shared`, `release_shared`, `acquire_mut`, and
 * `release_mut`: shared acquisition is a load plus CAS retry loop; mutable
 * acquisition is one CAS from unborrowed to mutable token; releases use the
 * same memory orders as production.
 * @throws The model itself does not throw. Relacy reports assertion or data-race
 * failures through its own test runner.
 * @note Ownership/thread-safety: fields are owned by one Relacy test instance
 * and are accessed only through Relacy-instrumented operations.
 */
template <borrow_op First, borrow_op Second>
class sync_owner_relacy_case final
    : public rl::test_suite<sync_owner_relacy_case<First, Second>, 2> {
public:
    /**
     * @brief Reset the borrow ledger and guarded payload before an iteration.
     *
     * @return Nothing.
     * @pre Called by Relacy before logical threads run.
     * @post `borrow_state_` is unborrowed and `payload_` holds the initial
     * value.
     * @invariant Every schedule begins from the same deterministic state.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: setup is single-threaded under Relacy.
     */
    void before() {
        borrow_state_($).store(k_no_borrow, rl::mo_relaxed);
        payload_($) = 40;
    }

    /**
     * @brief Execute one logical Relacy thread.
     *
     * @param thread_index Logical thread index supplied by Relacy; valid values
     * are 0 and 1.
     * @return Nothing.
     * @pre `thread_index < 2`.
     * @post The selected borrow operation has either completed a full
     * acquire/use/release lifetime or has rejected because the peer held an
     * incompatible borrow.
     * @invariant Both logical threads operate on the same Relacy atomic ledger
     * and Relacy guarded payload.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy interleaves this body at
     * instrumented operations.
     */
    void thread(unsigned thread_index) {
        const borrow_op op = operation_for(thread_index);
        if (op == borrow_op::shared) {
            run_shared_lifetime();
        } else {
            run_mut_lifetime();
        }
    }

    /**
     * @brief Check terminal ledger state after both logical threads complete.
     *
     * @return Nothing.
     * @pre Called by Relacy after a complete schedule.
     * @post Relacy records an assertion failure if a borrow leaked.
     * @invariant A correct borrow API balances every successful acquire with one
     * release.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy performs this check after both
     * logical threads are done.
     */
    void after() {
        const std::uint32_t state = borrow_state_($).load(rl::mo_acquire);
        RL_ASSERT(state == k_no_borrow);
    }

private:
    /**
     * @brief Return the operation assigned to one logical thread.
     *
     * @param thread_index Logical Relacy thread index.
     * @return `First` for thread 0 and `Second` for thread 1.
     * @pre `thread_index < 2`.
     * @post No state is modified.
     * @invariant The ordered pair is fixed at compile time so both
     * shared/mutable orderings are tested explicitly.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: pure metadata lookup.
     */
    static borrow_op operation_for(unsigned thread_index) noexcept {
        return thread_index == 0U ? First : Second;
    }

    /**
     * @brief Try to acquire one shared sync borrow.
     *
     * @retval true The shared borrow count was incremented.
     * @retval false The mutable token was observed or the shared counter was
     * exhausted, so no borrow was acquired.
     * @pre Called inside a Relacy logical thread.
     * @post On success, `release_shared()` must be called once.
     * @invariant Mirrors production's load plus compare-exchange retry loop;
     * the CAS failure order is acquire, and a failed CAS refreshes `observed`.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: every ledger access is a Relacy atomic
     * operation.
     */
    bool acquire_shared() {
        std::uint32_t observed = borrow_state_($).load(rl::mo_acquire);
        bool retried = false;
        for (;;) {
            if (observed == k_mut_token) {
                coverage().mut_blocks_shared = true;
                return false;
            }
            if (observed >= k_max_shared) {
                return false;
            }
            const std::uint32_t desired = observed + 1U;
            if (borrow_state_($).compare_exchange_weak(
                    observed, desired, rl::mo_acq_rel, rl::mo_acquire)) {
                if (retried) {
                    coverage().shared_cas_retry = true;
                }
                return true;
            }
            retried = true;
        }
    }

    /**
     * @brief Release one previously acquired shared sync borrow.
     *
     * @return Nothing.
     * @pre A successful `acquire_shared()` in this logical thread is live.
     * @post The shared-borrow count is decremented unless Relacy has already
     * reported a broken invariant.
     * @invariant Mirrors production's compare-exchange release loop and treats
     * impossible extra releases as no-ops.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: every ledger access is a Relacy atomic
     * operation.
     */
    void release_shared() {
        std::uint32_t observed = borrow_state_($).load(rl::mo_acquire);
        while (observed != k_no_borrow && observed != k_mut_token) {
            const std::uint32_t desired = observed - 1U;
            if (borrow_state_($).compare_exchange_weak(
                    observed, desired, rl::mo_acq_rel, rl::mo_acquire)) {
                return;
            }
        }
        RL_ASSERT(false && "shared release found no shared borrow");
    }

    /**
     * @brief Try to acquire the exclusive mutable sync borrow token.
     *
     * @retval true The mutable token was acquired.
     * @retval false Some borrow was already live, so no token was acquired.
     * @pre Called inside a Relacy logical thread.
     * @post On success, `release_mut()` must be called once.
     * @invariant Mirrors production's single `compare_exchange_strong` from
     * unborrowed to mutable token with no retry.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: the CAS is a Relacy atomic operation.
     */
    bool acquire_mut() {
        std::uint32_t expected = k_no_borrow;
        if (borrow_state_($).compare_exchange_strong(
                expected, k_mut_token, rl::mo_acq_rel, rl::mo_acquire)) {
            return true;
        }
        if (expected == k_mut_token) {
            coverage().mut_blocks_mut = true;
        } else {
            coverage().shared_blocks_mut = true;
        }
        return false;
    }

    /**
     * @brief Release the exclusive mutable sync borrow token.
     *
     * @return Nothing.
     * @pre A successful `acquire_mut()` in this logical thread is live.
     * @post The ledger returns to the unborrowed state.
     * @invariant Mirrors production's single `compare_exchange_strong` from
     * mutable token to unborrowed.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: the CAS is a Relacy atomic operation.
     */
    void release_mut() {
        std::uint32_t expected = k_mut_token;
        const bool released = borrow_state_($).compare_exchange_strong(
            expected, k_no_borrow, rl::mo_acq_rel, rl::mo_acquire);
        RL_ASSERT(released);
    }

    /**
     * @brief Execute one shared borrow lifetime against the guarded payload.
     *
     * @return Nothing.
     * @pre Called from `thread()`.
     * @post The payload was read under a shared borrow, or the borrow was
     * rejected because a mutable borrow was live.
     * @invariant Any shared/mutable overlap would be visible to Relacy as an
     * unsynchronized `rl::var` read/write race.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy controls every instrumented access.
     */
    void run_shared_lifetime() {
        if (!acquire_shared()) {
            return;
        }

        const std::uint32_t state = borrow_state_($).load(rl::mo_acquire);
        if (state == 2U) {
            coverage().two_shared_live = true;
        }

        const int seen = payload_($);
        (void)seen;

        release_shared();
    }

    /**
     * @brief Execute one mutable borrow lifetime against the guarded payload.
     *
     * @return Nothing.
     * @pre Called from `thread()`.
     * @post The payload was written under a mutable borrow, or the borrow was
     * rejected because another borrow was live.
     * @invariant Any overlap with a shared or second mutable access would be
     * visible to Relacy as an `rl::var` data race.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy controls every instrumented access.
     */
    void run_mut_lifetime() {
        if (!acquire_mut()) {
            return;
        }

        const int current = payload_($);
        payload_($) = current + 1;

        release_mut();
    }

    /// Relacy-instrumented Slice 4 atomic borrow ledger.
    rl::atomic<std::uint32_t> borrow_state_;
    /// Relacy-instrumented payload used as the data-race oracle.
    rl::var<int> payload_;
};

/**
 * @brief Relacy suite that interleaves the real public SyncOwner borrow APIs.
 *
 * @pre Constructed and driven by Relacy with two logical threads.
 * @post At least one full-search schedule holds a public `SyncRef<int>` while a
 * peer attempts `borrow_mut()`, and at least one schedule holds a public
 * `SyncMutRef<int>` while a peer attempts `borrow()`.
 * @invariant This is an API tie-back for the Relacy model above. The production
 * ledger still uses `std::atomic`, so the detailed atomic interleaving proof is
 * the `rl::atomic` state-machine model; this suite proves the public methods
 * expose the same rejection behavior while Relacy controls the borrow lifetimes.
 * @throws Expected `memsafe::violation` exceptions are caught inside the
 * logical threads. Unexpected exceptions are allowed to fail Relacy.
 * @note Ownership/thread-safety: Relacy logical threads run cooperatively on one
 * host thread; `marker_` is an `rl::atomic` used only as a scheduling point
 * while public borrow handles are live.
 */
class public_syncowner_relacy_case final
    : public rl::test_suite<public_syncowner_relacy_case, 2> {
public:
    /**
     * @brief Reset the Relacy scheduling marker before each public API run.
     *
     * @return Nothing.
     * @pre Called by Relacy before the logical threads run.
     * @post `marker_` is zero.
     * @invariant `owner_` is freshly constructed for each Relacy test instance,
     * so no public borrow state is carried between schedules.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: setup is single-threaded under Relacy.
     */
    void before() {
        marker_($).store(0, rl::mo_relaxed);
    }

    /**
     * @brief Execute one public SyncOwner borrow operation under Relacy.
     *
     * @param thread_index Logical thread index; 0 runs `borrow()`, 1 runs
     * `borrow_mut()`.
     * @return Nothing.
     * @pre `thread_index < 2`.
     * @post The chosen public borrow either completed while touching `marker_`
     * or rejected with `borrow_exclusivity` while the peer borrow was live.
     * @invariant The marker operation creates a Relacy scheduling point while
     * the public borrow handle is still in scope.
     * @throws Unexpected exceptions escape to Relacy and fail the test.
     * @note Ownership/thread-safety: public owner access is single-host-thread
     * but Relacy-controlled at marker scheduling points.
     */
    void thread(unsigned thread_index) {
        if (thread_index == 0U) {
            run_public_shared();
        } else {
            run_public_mut();
        }
    }

private:
    /**
     * @brief Hold a public immutable SyncOwner borrow across a Relacy marker.
     *
     * @return Nothing.
     * @pre Called by logical thread 0.
     * @post Either the shared borrow completed, or a live mutable borrow caused
     * a `borrow_exclusivity` violation that was recorded in coverage.
     * @invariant `marker_` is touched only while the public `SyncRef<int>` is
     * live, giving the mutable peer a schedule point inside the borrow lifetime.
     * @throws Unexpected exceptions escape to Relacy.
     * @note Ownership/thread-safety: the public handle is non-owning and scoped
     * to this logical thread body.
     */
    void run_public_shared() {
        try {
            const memsafe::SyncRef<int> read = owner_.borrow();
            CHECK(*read >= 0);
            marker_($).store(1, rl::mo_relaxed);
            (void)marker_($).load(rl::mo_relaxed);
        } catch (const memsafe::violation& caught) {
            CHECK(caught.kind() == memsafe::violation_kind::borrow_exclusivity);
            coverage().public_mut_blocks_shared = true;
        }
    }

    /**
     * @brief Hold a public mutable SyncOwner borrow across a Relacy marker.
     *
     * @return Nothing.
     * @pre Called by logical thread 1.
     * @post Either the mutable borrow completed, or a live shared borrow caused
     * a `borrow_exclusivity` violation that was recorded in coverage.
     * @invariant `marker_` is touched only while the public `SyncMutRef<int>` is
     * live, giving the shared peer a schedule point inside the borrow lifetime.
     * @throws Unexpected exceptions escape to Relacy.
     * @note Ownership/thread-safety: the public handle is non-owning and scoped
     * to this logical thread body.
     */
    void run_public_mut() {
        try {
            memsafe::SyncMutRef<int> write = owner_.borrow_mut();
            *write += 1;
            marker_($).store(2, rl::mo_relaxed);
            (void)marker_($).load(rl::mo_relaxed);
        } catch (const memsafe::violation& caught) {
            CHECK(caught.kind() == memsafe::violation_kind::borrow_exclusivity);
            coverage().public_shared_blocks_mut = true;
        }
    }

    /// Public owner whose borrow methods are interleaved by this Relacy suite.
    memsafe::SyncOwner<int> owner_{0};
    /// Relacy scheduling point touched while public borrows are live.
    rl::atomic<int> marker_;
};

/**
 * @brief Run one Relacy test suite with full schedule enumeration.
 *
 * @tparam Test Relacy test-suite type to simulate.
 * @param name Diagnostic case name printed before simulation.
 * @return Nothing.
 * @pre `Test` derives from `rl::test_suite<Test, 2>`.
 * @post Relacy has explored the full two-thread schedule tree for `Test`, or
 * has reported the first failing schedule through its own runner.
 * @invariant `sched_full` is selected explicitly so the test is exhaustive, not
 * randomized sampling. The iteration bound is deliberately high for these
 * small two-thread lifetimes and acts only as a runaway guard.
 * @throws Nothing intentionally from this wrapper; Relacy owns failure
 * reporting.
 * @note Ownership/thread-safety: drives Relacy on the current host thread.
 */
template <typename Test>
void simulate_full_relacy_case(const char* name) {
    std::fprintf(stdout, "relacy sync concurrency case: %s\n", name);

    rl::test_params params;
    params.search_type = rl::sched_full;
    params.iteration_count = k_relacy_iteration_bound;

    if constexpr (std::is_same<void, decltype(rl::simulate<Test>(params))>::value) {
        rl::simulate<Test>(params);
    } else {
        const auto result = rl::simulate<Test>(params);
        CHECK(static_cast<bool>(result));
    }
}

/**
 * @brief Exhaust all ordered two-thread borrow/borrow_mut Relacy cases.
 *
 * @return Nothing.
 * @pre Upstream Relacy headers are available from the vendored dependency path.
 * @post All four operation pairings have been simulated under full Relacy
 * schedule search.
 * @invariant Running both shared/mutable orderings prevents the test from
 * assuming thread-index symmetry.
 * @throws Nothing intentionally; Relacy handles model failures.
 * @note Ownership/thread-safety: all simulations run on the calling thread.
 */
void run_relacy_sync_borrow_interleavings() {
    simulate_full_relacy_case<
        sync_owner_relacy_case<borrow_op::shared, borrow_op::shared>>(
        "borrow/borrow");
    simulate_full_relacy_case<
        sync_owner_relacy_case<borrow_op::shared, borrow_op::exclusive>>(
        "borrow/borrow_mut");
    simulate_full_relacy_case<
        sync_owner_relacy_case<borrow_op::exclusive, borrow_op::shared>>(
        "borrow_mut/borrow");
    simulate_full_relacy_case<
        sync_owner_relacy_case<borrow_op::exclusive, borrow_op::exclusive>>(
        "borrow_mut/borrow_mut");
    simulate_full_relacy_case<public_syncowner_relacy_case>(
        "public SyncOwner borrow/borrow_mut");
}

/**
 * @brief Assert that the Relacy search reached the expected contention cases.
 *
 * @return Nothing.
 * @pre `run_relacy_sync_borrow_interleavings()` has completed.
 * @post Harness failures are recorded for missing shared/shared coexistence,
 * missing shared-vs-mutable rejection, missing mutable-vs-shared rejection,
 * missing mutable-vs-mutable rejection, or missing CAS retry coverage.
 * @invariant These flags prove the full Relacy runs did not merely instantiate
 * the test suites; they reached the meaningful contention states required by
 * the Slice 4 acceptance criteria.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: reads the single-threaded coverage aggregate.
 */
void check_relacy_coverage() noexcept {
    const relacy_coverage& cov = coverage();
    CHECK(cov.two_shared_live);
    CHECK(cov.shared_blocks_mut);
    CHECK(cov.mut_blocks_shared);
    CHECK(cov.mut_blocks_mut);
    CHECK(cov.shared_cas_retry);
    CHECK(cov.public_shared_blocks_mut);
    CHECK(cov.public_mut_blocks_shared);
}

/**
 * @brief Check public `memsafe::SyncOwner<T>` exclusivity behavior.
 *
 * @return Nothing.
 * @pre The production sync header is available, checks are enabled, and the
 * throwing violation policy is selected.
 * @post Harness failures are recorded if shared borrows cannot coexist, if
 * `borrow_mut()` is not rejected while a shared borrow is live, or if a mutable
 * borrow cannot mutate after shared borrows end.
 * @invariant This smoke check does not replace Relacy; it ties the modelled
 * state machine back to the finalized public Slice 4 API.
 * @throws Nothing intentionally; expected `memsafe::violation` exceptions are
 * caught and checked.
 * @note Ownership/thread-safety: single-threaded API smoke only.
 */
void check_public_syncowner_exclusivity() {
    memsafe::SyncOwner<int> owner(40);
    CHECK(owner.has_value());

    {
        const memsafe::SyncRef<int> first = owner.borrow();
        const memsafe::SyncRef<int> second = first;
        CHECK(*first == 40);
        CHECK(*second == 40);

        bool saw_exclusivity_violation = false;
        try {
            const memsafe::SyncMutRef<int> blocked = owner.borrow_mut();
            (void)blocked;
            CHECK(false);
        } catch (const memsafe::violation& caught) {
            saw_exclusivity_violation = true;
            CHECK(caught.kind() == memsafe::violation_kind::borrow_exclusivity);
        } catch (...) {
            CHECK(false);
        }
        CHECK(saw_exclusivity_violation);
    }

    {
        memsafe::SyncMutRef<int> writer = owner.borrow_mut();
        *writer = 73;
        CHECK(*writer == 73);
    }

    CHECK(*owner == 73);
}

} // namespace

/**
 * @brief Run the Relacy-backed SyncOwner concurrency test.
 *
 * @retval 0 Relacy exhausted every two-thread borrow/borrow_mut interleaving,
 * the required contention coverage was observed, no Relacy assertion or
 * data-race failure occurred, and the public SyncOwner smoke check passed.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built in the optional F4 concurrency lane with the
 * upstream Relacy Race Detector snapshot vendored under
 * `testing/tests/concurrency/vendor/relacy/`.
 * @post The process exit code is the CTest verdict.
 * @invariant This translation unit contributes only TEST coverage for
 * CPP_MEMSAFE-0430-TEST and does not modify production code.
 * @throws Nothing intentionally; unexpected public API exceptions fail the
 * harness.
 * @note Ownership/thread-safety: Relacy logical threads are cooperatively
 * scheduled by the checker; no host operating-system threads are started here.
 */
int main() {
    run_relacy_sync_borrow_interleavings();
    check_relacy_coverage();
    check_public_syncowner_exclusivity();

    RUN_TESTS("test_sync_concurrency");
}
