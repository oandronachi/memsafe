/**
 * @file test_arc_mutex_concurrency.cpp
 * @brief Relacy Race Detector coverage for Slice 5 `memsafe::Arc<T>` strong
 * reference accounting and `memsafe::Mutex<T>` mutual exclusion.
 *
 * @details
 * Work package: CPP_MEMSAFE-0530-TEST.
 *
 * Purpose:
 * - Exercise the Slice 5 `Arc Atomic_Refcount` protocol required by
 *   CPP_MEMSAFE-0500-FUNC under every two-thread retain/release interleaving
 *   that corresponds to public `Arc<T>` clone/drop operations.
 * - Exercise the Slice 5 `Mutex Mutex_Guarded` protocol by running two logical
 *   Relacy threads through `lock()` / guarded payload access / `unlock()`.
 * - Consume the vendored Relacy Race Detector headers from the
 *   CPP_MEMSAFE-0035-TEST vendor path. There is no fallback scheduler and no
 *   README-only success path; missing Relacy headers are a build failure.
 *
 * Key invariants:
 * - The Arc model mirrors `memsafe::detail::arc_ctrl<T>`: retain is an
 *   acquire load plus acq_rel compare-exchange loop, release is an acq_rel
 *   decrement, and the final release performs an acquire fence before the
 *   payload destruction side effect.
 * - The Arc strong count never wraps, never reaches zero while a retained
 *   clone is live, and exactly one logical thread observes the last release in
 *   the drop-to-zero case.
 * - The Mutex model uses Relacy's own `rl::mutex` and touches the guarded
 *   payload through `rl::var<int>`. If the lock/unlock protocol admitted two
 *   simultaneous critical sections, Relacy would report either the explicit
 *   live-lock assertion or a non-atomic payload data race.
 * - Public `memsafe::Arc<memsafe::Mutex<int>>` smoke coverage remains
 *   single-threaded and API-level only. The exhaustive interleaving proof is
 *   the Relacy model above, which avoids reaching into private production
 *   members.
 *
 * Ownership and thread-safety:
 * - Relacy logical threads are cooperatively scheduled by the checker; this
 *   executable starts no operating-system worker threads.
 * - All shared model state is owned by one Relacy test-suite instance and is
 *   accessed only through Relacy-instrumented atomics, variables, and mutexes.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force checked Mutex borrow bookkeeping for the public API smoke.
 *
 * @retval 1 Runtime checks are enabled for `memsafe::Mutex<T>::has_borrow()`
 * and the mutex-backed `MutRef<T>` release hook in this translation unit.
 * @pre Define before including `<memsafe/sync.hpp>`.
 * @post Public smoke checks can observe that a live `Mutex<T>::lock()` result
 * records and then clears the checked exclusive borrow token.
 * @invariant The concurrency lane must not inherit a release no-checks build
 * setting for the source-level `has_borrow()` oracle.
 * @throws Nothing directly; the macro only configures inline library code.
 * @note Ownership/thread-safety: preprocessor configuration only.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#include "../../test_harness.hpp"

#include <memsafe/sync.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>

/*
 * CPP_MEMSAFE-0530-TEST requires Relacy Race Detector coverage. Include the
 * upstream Relacy core header directly from the dependency's declared vendor
 * path and deliberately provide no compatibility shim or fallback engine.
 *
 * The include is placed after project and standard headers because upstream
 * Relacy defines malloc/free/assert instrumentation macros for code under test.
 * This file models the private Arc/Mutex synchronization protocols with
 * Relacy primitives directly rather than compiling production headers under
 * those macros.
 */
#include "vendor/relacy/relacy/relacy.hpp"

namespace {

/// Maximum number of Relacy schedules allowed for each bounded two-thread case.
constexpr std::size_t k_relacy_iteration_bound = 100000U;

/// Initial strong count for a single root Arc handle.
constexpr std::uint64_t k_arc_root_strong_count = 1U;

/// Initial strong count for two independent handles racing to drop to zero.
constexpr std::uint64_t k_arc_two_handle_strong_count = 2U;

/// Payload value used by the Arc model to prove clones see a live payload.
constexpr int k_arc_payload_value = 731;

/**
 * @brief Coverage flags accumulated across all Relacy simulations.
 *
 * @pre Mutated only by the single host thread driving Relacy simulations.
 * @post Each flag becomes true once at least one explored schedule reaches the
 * named state.
 * @invariant Coverage flags are monotonic diagnostics. They never steer the
 * modeled algorithm, so they cannot hide a bad Relacy schedule.
 * @throws Nothing; the aggregate owns only booleans.
 * @note Ownership/thread-safety: Relacy logical threads are cooperatively
 * scheduled on the current host thread, matching the pattern used by the
 * existing concurrency tests.
 */
struct relacy_coverage final {
    /// Two retained Arc clones were live at the same time as the root.
    bool arc_three_strong_seen = false;
    /// A racing Arc retain had to retry its compare-exchange.
    bool arc_retain_cas_retry_seen = false;
    /// A non-final Arc drop observed that another strong handle remained.
    bool arc_non_final_drop_seen = false;
    /// Exactly one Arc drop-to-zero schedule destroyed the modeled payload.
    bool arc_final_drop_seen = false;
    /// Both Mutex logical threads entered a serialized critical section.
    bool mutex_both_threads_entered = false;
    /// The second Mutex critical section observed the first payload update.
    bool mutex_payload_reached_two = false;
    /// Public Arc copies shared one `memsafe::Mutex<int>` payload.
    bool public_arc_mutex_shared_payload = false;
};

/**
 * @brief Return the process-wide coverage accumulator.
 *
 * @return Mutable reference to the coverage aggregate.
 * @pre Called only from this standalone test executable.
 * @post No flag is reset by this accessor.
 * @invariant The accumulator is outside Relacy suite instances because Relacy
 * constructs a fresh suite for each schedule.
 * @throws Nothing; initialization is trivial.
 * @note Ownership/thread-safety: single host thread during Relacy simulation.
 */
relacy_coverage& coverage() noexcept {
    static relacy_coverage cov;
    return cov;
}

/**
 * @brief Relacy model for two threads cloning and dropping one Arc root.
 *
 * @pre Instantiated and driven by Relacy with exactly two logical threads.
 * @post Every successful retain is released before the schedule completes, and
 * the root strong count remains one.
 * @invariant This suite mirrors public `Arc<T>` copy construction followed by
 * destruction of the copied handle. It keeps the root handle alive so clone and
 * drop interleavings race only on the atomic strong count, not on the lifetime
 * of the source handle object.
 * @throws The model itself does not throw. Relacy reports assertion, data-race,
 * or memory-model failures through its runner.
 * @note Ownership/thread-safety: fields are owned by one Relacy suite instance
 * and all shared accesses use Relacy primitives.
 */
class arc_clone_drop_relacy_case final
    : public rl::test_suite<arc_clone_drop_relacy_case, 2> {
public:
    /**
     * @brief Reset the root Arc control block model before one schedule.
     *
     * @return Nothing.
     * @pre Called by Relacy before the logical threads run.
     * @post The modeled root count is one, no destruction has occurred, and
     * the payload is initialized.
     * @invariant Every schedule begins from the same one-root-handle state.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: setup is single-threaded under Relacy.
     */
    void before() {
        strong_count_($).store(k_arc_root_strong_count, rl::mo_relaxed);
        destruction_count_($).store(0U, rl::mo_relaxed);
        payload_($) = k_arc_payload_value;
    }

    /**
     * @brief Clone the root Arc and drop the clone in one logical thread.
     *
     * @param thread_index Logical thread index supplied by Relacy; valid values
     * are 0 and 1.
     * @return Nothing.
     * @pre `thread_index < 2`.
     * @post The thread has retained one strong reference, observed the payload
     * while that reference is live, and released the retained reference.
     * @invariant Both logical threads race on the same Relacy atomic strong
     * count, matching the Slice 5 `Atomic_Refcount` ownership protocol.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy interleaves this body at
     * instrumented atomic and variable operations.
     */
    void thread(unsigned thread_index) {
        (void)thread_index;

        const bool retained = retain_strong();
        RL_ASSERT(retained);

        const std::uint64_t after_retain =
            strong_count_($).load(rl::mo_acquire);
        if (after_retain == 3U) {
            coverage().arc_three_strong_seen = true;
        }

        const int observed_payload = payload_($);
        RL_ASSERT(observed_payload == k_arc_payload_value);

        release_clone_strong();
    }

    /**
     * @brief Check that clone drops returned the root count to one.
     *
     * @return Nothing.
     * @pre Called by Relacy after both logical threads complete.
     * @post Relacy records a failure if a retain leaked, a release underflowed,
     * or a clone drop destroyed the still-rooted payload.
     * @invariant The root Arc remains live for the whole modeled schedule.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy performs this check after all
     * logical threads have finished.
     */
    void after() {
        const std::uint64_t count = strong_count_($).load(rl::mo_acquire);
        const std::uint32_t destroyed =
            destruction_count_($).load(rl::mo_acquire);
        RL_ASSERT(count == k_arc_root_strong_count);
        RL_ASSERT(destroyed == 0U);
    }

private:
    /**
     * @brief Retain one additional strong Arc reference.
     *
     * @retval true The strong count was incremented and the caller owns one
     * release obligation.
     * @retval false The count was exhausted before retain could publish a new
     * owner.
     * @pre The modeled root strong reference is live.
     * @post On success, `release_clone_strong()` must be called exactly once.
     * @invariant The loop mirrors `arc_ctrl<T>::retain_strong()`: acquire load,
     * overflow check, and acq_rel compare-exchange with acquire failure order.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: every count access is a Relacy atomic
     * operation so Relacy owns the race oracle.
     */
    bool retain_strong() {
        std::uint64_t observed = strong_count_($).load(rl::mo_acquire);
        bool retried = false;
        for (;;) {
            if (observed == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }

            const std::uint64_t desired = observed + 1U;
            if (strong_count_($).compare_exchange_weak(
                    observed, desired, rl::mo_acq_rel, rl::mo_acquire)) {
                if (retried) {
                    coverage().arc_retain_cas_retry_seen = true;
                }
                if (desired == 3U) {
                    coverage().arc_three_strong_seen = true;
                }
                return true;
            }
            retried = true;
        }
    }

    /**
     * @brief Release the retained clone reference.
     *
     * @return Nothing.
     * @pre This logical thread owns a successful retain from `retain_strong()`.
     * @post The strong count is one lower and the root handle still keeps the
     * payload alive.
     * @invariant In the clone/drop case, dropping a clone must never observe
     * the transition from one to zero because the root handle remains live.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: release uses the Relacy acq_rel atomic
     * decrement that mirrors production's `fetch_sub`.
     */
    void release_clone_strong() {
        const std::uint64_t previous =
            strong_count_($).fetch_sub(1U, rl::mo_acq_rel);
        RL_ASSERT(previous > k_arc_root_strong_count);
        coverage().arc_non_final_drop_seen = true;
    }

    /// Relacy-instrumented Arc strong-reference count.
    rl::atomic<std::uint64_t> strong_count_;
    /// Relacy-instrumented payload used to prove retained clones see live data.
    rl::var<int> payload_;
    /// Relacy-instrumented destruction side-effect counter.
    rl::atomic<std::uint32_t> destruction_count_;
};

/**
 * @brief Relacy model for two final Arc handles racing to drop to zero.
 *
 * @pre Instantiated and driven by Relacy with exactly two logical threads.
 * @post Both modeled handles have released their strong references and exactly
 * one logical thread has performed the destruction side effect.
 * @invariant This suite covers the drop-to-zero half of the Arc release
 * protocol separately from clone/drop so the final deletion race is explicit.
 * @throws The model itself does not throw. Relacy reports failures through its
 * own test runner.
 * @note Ownership/thread-safety: all state is owned by the Relacy suite and
 * accessed through Relacy primitives.
 */
class arc_drop_to_zero_relacy_case final
    : public rl::test_suite<arc_drop_to_zero_relacy_case, 2> {
public:
    /**
     * @brief Reset the two-handle Arc model before one schedule.
     *
     * @return Nothing.
     * @pre Called by Relacy before logical threads run.
     * @post The strong count is two and no destruction has been recorded.
     * @invariant Each logical thread owns exactly one release obligation.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: setup is single-threaded under Relacy.
     */
    void before() {
        strong_count_($).store(k_arc_two_handle_strong_count, rl::mo_relaxed);
        destruction_count_($).store(0U, rl::mo_relaxed);
        payload_($) = k_arc_payload_value;
    }

    /**
     * @brief Drop one pre-existing Arc handle.
     *
     * @param thread_index Logical thread index supplied by Relacy; valid values
     * are 0 and 1.
     * @return Nothing.
     * @pre `thread_index < 2` and this logical thread owns one strong release.
     * @post The thread has released its handle; exactly one thread in the full
     * schedule observes the final release.
     * @invariant Both threads use the same acq_rel decrement as production
     * `arc_ctrl<T>::release_strong()`.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy interleaves both releases.
     */
    void thread(unsigned thread_index) {
        (void)thread_index;
        release_owned_strong();
    }

    /**
     * @brief Check that the two drops destroyed the payload exactly once.
     *
     * @return Nothing.
     * @pre Called by Relacy after both logical threads complete.
     * @post Relacy records a failure if the count is not zero or the modeled
     * destructor side effect did not run exactly once.
     * @invariant Count zero is represented by no reachable control block in
     * production; in this bounded model it is represented by a zero counter
     * plus one destruction record.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: final check runs after both releases.
     */
    void after() {
        const std::uint64_t count = strong_count_($).load(rl::mo_acquire);
        const std::uint32_t destroyed =
            destruction_count_($).load(rl::mo_acquire);
        RL_ASSERT(count == 0U);
        RL_ASSERT(destroyed == 1U);
    }

private:
    /**
     * @brief Release one owned strong reference and destroy on the final drop.
     *
     * @return Nothing.
     * @pre The current logical thread owns one strong release obligation.
     * @post The count is decremented; if the previous count was one, the
     * modeled payload destruction counter is incremented exactly once.
     * @invariant Mirrors production's `fetch_sub(acq_rel)` followed by an
     * acquire fence before final deletion.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: all count and destruction bookkeeping is
     * Relacy-instrumented.
     */
    void release_owned_strong() {
        const std::uint64_t previous =
            strong_count_($).fetch_sub(1U, rl::mo_acq_rel);
        RL_ASSERT(previous > 0U);

        if (previous == 1U) {
            rl::atomic_thread_fence(rl::mo_acquire, $);
            const int observed_payload = payload_($);
            RL_ASSERT(observed_payload == k_arc_payload_value);
            const std::uint32_t previous_destroyed =
                destruction_count_($).fetch_add(1U, rl::mo_acq_rel);
            RL_ASSERT(previous_destroyed == 0U);
            coverage().arc_final_drop_seen = true;
        } else {
            coverage().arc_non_final_drop_seen = true;
        }
    }

    /// Relacy-instrumented Arc strong-reference count.
    rl::atomic<std::uint64_t> strong_count_;
    /// Relacy-instrumented payload read by the final releaser.
    rl::var<int> payload_;
    /// Relacy-instrumented destruction side-effect counter.
    rl::atomic<std::uint32_t> destruction_count_;
};

/**
 * @brief Relacy model for two Mutex lock/unlock critical sections.
 *
 * @pre Instantiated and driven by Relacy with exactly two logical threads.
 * @post Both threads have entered the critical section once, the guarded
 * payload mirror records two increments, and no live lock remains.
 * @invariant `rl::mutex` serializes all `rl::var<int>` payload access. The
 * atomic `live_locks_` mirror is an explicit assertion that at most one thread
 * is inside the critical section at any instant.
 * @throws The model itself does not throw. Relacy reports mutex misuse,
 * assertion failures, and data races through its runner.
 * @note Ownership/thread-safety: the suite owns the mutex and guarded payload.
 */
class mutex_lock_unlock_relacy_case final
    : public rl::test_suite<mutex_lock_unlock_relacy_case, 2> {
public:
    /**
     * @brief Reset the modeled Mutex payload and mirrors before one schedule.
     *
     * @return Nothing.
     * @pre Called by Relacy before logical threads run.
     * @post The payload is zero, no critical section is live, and no lock entry
     * has been counted.
     * @invariant Every schedule begins from an unlocked mutex with a stable
     * guarded payload.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: setup is single-threaded under Relacy.
     */
    void before() {
        guarded_payload_($) = 0;
        payload_mirror_($).store(0, rl::mo_relaxed);
        live_locks_($).store(0U, rl::mo_relaxed);
        completed_locks_($).store(0U, rl::mo_relaxed);
    }

    /**
     * @brief Lock the mutex, mutate the payload, and unlock it.
     *
     * @param thread_index Logical thread index supplied by Relacy; valid values
     * are 0 and 1.
     * @return Nothing.
     * @pre `thread_index < 2`.
     * @post This logical thread has performed exactly one serialized payload
     * increment.
     * @invariant The payload read/write happens only while `mutex_` is locked.
     * A broken lock protocol would be caught by `live_locks_` or Relacy's
     * `rl::var` data-race tracking.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: Relacy controls every scheduling point in
     * the lock, payload access, and unlock sequence.
     */
    void thread(unsigned thread_index) {
        (void)thread_index;

        mutex_.lock($);

        const std::uint32_t previous_live =
            live_locks_($).fetch_add(1U, rl::mo_acq_rel);
        RL_ASSERT(previous_live == 0U);

        const int current = guarded_payload_($);
        RL_ASSERT(current == 0 || current == 1);
        guarded_payload_($) = current + 1;
        payload_mirror_($).store(current + 1, rl::mo_release);
        if (current + 1 == 2) {
            coverage().mutex_payload_reached_two = true;
        }

        const std::uint32_t completed =
            completed_locks_($).fetch_add(1U, rl::mo_acq_rel) + 1U;
        if (completed == 2U) {
            coverage().mutex_both_threads_entered = true;
        }

        const std::uint32_t previous_unlock =
            live_locks_($).fetch_sub(1U, rl::mo_acq_rel);
        RL_ASSERT(previous_unlock == 1U);

        mutex_.unlock($);
    }

    /**
     * @brief Check terminal Mutex state after both logical threads finish.
     *
     * @return Nothing.
     * @pre Called by Relacy after both logical threads complete.
     * @post Relacy records a failure if a lock leaked, a thread skipped the
     * critical section, or the guarded payload mirror did not receive both
     * increments.
     * @invariant The non-atomic payload remains the race oracle inside the
     * critical section; terminal checks use atomics to avoid depending on
     * version-specific Relacy `rl::var` access rules in `after()`.
     * @throws Nothing intentionally.
     * @note Ownership/thread-safety: final check runs after both logical
     * threads are done.
     */
    void after() {
        const std::uint32_t live = live_locks_($).load(rl::mo_acquire);
        const std::uint32_t completed =
            completed_locks_($).load(rl::mo_acquire);
        const int payload = payload_mirror_($).load(rl::mo_acquire);
        RL_ASSERT(live == 0U);
        RL_ASSERT(completed == 2U);
        RL_ASSERT(payload == 2);
    }

private:
    /// Relacy mutex modeling `memsafe::Mutex<T>`'s guarded critical section.
    rl::mutex mutex_;
    /// Relacy non-atomic payload used as the mutual-exclusion race oracle.
    rl::var<int> guarded_payload_;
    /// Relacy atomic terminal mirror of the guarded payload value.
    rl::atomic<int> payload_mirror_;
    /// Relacy atomic mirror of the number of live critical sections.
    rl::atomic<std::uint32_t> live_locks_;
    /// Relacy atomic count of completed critical sections.
    rl::atomic<std::uint32_t> completed_locks_;
};

/**
 * @brief Run one Relacy suite using exhaustive full-search scheduling.
 *
 * @tparam Test Relacy test-suite type to simulate.
 * @param name Diagnostic case name printed before simulation.
 * @return Nothing.
 * @pre `Test` derives from `rl::test_suite<Test, 2>`.
 * @post Relacy has explored the full two-thread schedule tree for `Test`, or
 * the harness has recorded a failure from Relacy's verdict.
 * @invariant `sched_full` is selected explicitly so CPP_MEMSAFE-0530-TEST is
 * exhaustive rather than random sampling. The high iteration bound is a
 * runaway guard for these tiny two-thread lifetimes.
 * @throws Nothing intentionally from this wrapper; Relacy owns model failure
 * reporting.
 * @note Ownership/thread-safety: drives Relacy on the current host thread.
 */
template <typename Test>
void simulate_full_relacy_case(const char* name) {
    std::fprintf(stdout, "relacy arc/mutex concurrency case: %s\n", name);

    rl::test_params params;
    params.search_type = rl::sched_full;
    params.iteration_count = k_relacy_iteration_bound;

    if constexpr (std::is_same<void, decltype(rl::simulate<Test>(params))>::value) {
        rl::simulate<Test>(params);
        CHECK(params.test_result == rl::test_result_success);
    } else {
        const auto result = rl::simulate<Test>(params);
        CHECK(static_cast<bool>(result));
        CHECK(params.test_result == rl::test_result_success);
    }
}

/**
 * @brief Exhaust the Slice 5 Relacy Arc and Mutex concurrency suites.
 *
 * @return Nothing.
 * @pre Upstream Relacy headers are available from the vendored dependency
 * directory.
 * @post Arc clone/drop, Arc drop-to-zero, and Mutex lock/unlock suites have
 * completed full two-thread schedule search.
 * @invariant Arc ownership and Mutex guarding are modeled separately so each
 * Slice 5 synchronization protocol has a focused Relacy oracle.
 * @throws Nothing intentionally; Relacy handles model failures.
 * @note Ownership/thread-safety: all simulations run on the calling thread.
 */
void run_relacy_arc_mutex_interleavings() {
    simulate_full_relacy_case<arc_clone_drop_relacy_case>(
        "Arc clone/drop retain-release");
    simulate_full_relacy_case<arc_drop_to_zero_relacy_case>(
        "Arc drop/drop final release");
    simulate_full_relacy_case<mutex_lock_unlock_relacy_case>(
        "Mutex lock/unlock guarded increment");
}

/**
 * @brief Assert that the Relacy search reached the intended contention states.
 *
 * @return Nothing.
 * @pre `run_relacy_arc_mutex_interleavings()` has completed.
 * @post Harness failures are recorded for missing Arc concurrent-retain
 * coverage, missing final-drop coverage, missing Mutex two-entry coverage, or
 * missing final guarded payload update.
 * @invariant These flags prove the full Relacy runs did not merely instantiate
 * the suites; they reached the meaningful Arc and Mutex states named in
 * CPP_MEMSAFE-0530-TEST acceptance.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: reads the single-threaded coverage aggregate.
 */
void check_relacy_coverage() noexcept {
    const relacy_coverage& cov = coverage();
    CHECK(cov.arc_three_strong_seen);
    CHECK(cov.arc_retain_cas_retry_seen);
    CHECK(cov.arc_non_final_drop_seen);
    CHECK(cov.arc_final_drop_seen);
    CHECK(cov.mutex_both_threads_entered);
    CHECK(cov.mutex_payload_reached_two);
}

/**
 * @brief Check public `memsafe::Arc<memsafe::Mutex<int>>` API integration.
 *
 * @return Nothing.
 * @pre The finalized Slice 5 public types are available from
 * `<memsafe/sync.hpp>` and checked Mutex borrow bookkeeping is enabled.
 * @post Harness failures are recorded if Arc copies do not share one Mutex
 * payload, strong counts do not track copies, Mutex locks do not mutate the
 * shared payload, or the checked borrow token leaks after unlock.
 * @invariant This is an API tie-back only. Exhaustive refcount and
 * mutual-exclusion interleavings are covered by the Relacy suites above.
 * @throws Unexpected public API exceptions are allowed to fail the executable
 * under the runner's exit-code model.
 * @note Ownership/thread-safety: single-threaded smoke; no host worker threads
 * are created.
 */
void check_public_arc_mutex_smoke() {
    memsafe::Arc<memsafe::Mutex<int>> shared(0);
    CHECK(shared.has_value());
    CHECK(shared.strong_count() == 1U);

    {
        memsafe::Arc<memsafe::Mutex<int>> first_clone(shared);
        memsafe::Arc<memsafe::Mutex<int>> second_clone(shared);
        CHECK(shared.strong_count() == 3U);
        CHECK(first_clone.strong_count() == 3U);
        CHECK(second_clone.strong_count() == 3U);

        {
            auto locked = first_clone->lock();
            CHECK(first_clone->has_borrow());
            ++*locked;
        }
        CHECK(!shared->has_borrow());

        {
            auto locked = second_clone->lock();
            CHECK(second_clone->has_borrow());
            ++*locked;
        }
        CHECK(!shared->has_borrow());

        coverage().public_arc_mutex_shared_payload = true;
    }

    CHECK(shared.strong_count() == 1U);

    {
        auto locked = shared->lock();
        CHECK(shared->has_borrow());
        CHECK(*locked == 2);
    }
    CHECK(!shared->has_borrow());
}

/**
 * @brief Assert that the public Arc/Mutex smoke reached its oracle state.
 *
 * @return Nothing.
 * @pre `check_public_arc_mutex_smoke()` has completed.
 * @post Harness records a failure if the public smoke did not run through
 * shared Arc clones and Mutex payload mutation.
 * @invariant Kept separate from `check_relacy_coverage()` so missing public
 * API tie-back is diagnosed distinctly from missing Relacy interleavings.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: reads the single-threaded coverage aggregate.
 */
void check_public_coverage() noexcept {
    CHECK(coverage().public_arc_mutex_shared_payload);
}

} // namespace

/**
 * @brief Run the Relacy-backed Arc/Mutex concurrency test executable.
 *
 * @retval 0 Relacy exhausted every two-thread Arc clone/drop, Arc final-drop,
 * and Mutex lock/unlock interleaving; the required coverage states were
 * observed; no Relacy race/assertion failure occurred; and the public
 * Arc/Mutex smoke passed.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built in the optional F4 concurrency lane with the
 * upstream Relacy Race Detector snapshot vendored under
 * `testing/tests/concurrency/vendor/relacy/`.
 * @post The process exit code is the CTest verdict.
 * @invariant This translation unit contributes only TEST coverage for
 * CPP_MEMSAFE-0530-TEST and does not modify production code.
 * @throws Nothing intentionally; unexpected public API or infrastructure
 * exceptions fail the process under the runner's exit-code model.
 * @note Ownership/thread-safety: Relacy logical threads are cooperatively
 * scheduled by the checker; no host operating-system threads are started here.
 */
int main() {
    run_relacy_arc_mutex_interleavings();
    check_relacy_coverage();
    check_public_arc_mutex_smoke();
    check_public_coverage();

    RUN_TESTS("test_arc_mutex_concurrency");
}
