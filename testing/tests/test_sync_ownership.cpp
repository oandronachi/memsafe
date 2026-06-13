/**
 * @file test_sync_ownership.cpp
 * @brief Runtime unit tests for Slice 4 `SyncOwner`, `SyncRef`, and
 * `SyncMutRef`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0410-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0400-FUNC synchronization ownership
 *   primitives through the public `memsafe::SyncOwner<T>`,
 *   `memsafe::SyncRef<T>`, and `memsafe::SyncMutRef<T>` API.
 * - Verify single-thread functional parity with `memsafe::Owner<T>` for
 *   construction, destruction, immutable borrows, mutable borrows, direct
 *   payload observation, and borrow-exclusivity violations.
 * - Verify F2 Scope and Concurrency case 2 with two host threads: a shared
 *   sync borrow blocks an exclusive mutable sync borrow while live, and an
 *   exclusive mutable sync borrow blocks a shared sync borrow while live.
 *
 * Key invariants:
 * - This translation unit forces runtime checks on and selects the THROW
 *   violation policy so borrow-accounting failures are observable in-process.
 * - No `CHECK` call runs from worker threads. Worker threads write atomic
 *   result cells, and the main thread performs all harness assertions after
 *   joins because the F4 harness deliberately owns only a simple process-wide
 *   failure counter.
 * - The cross-thread tests keep the owning `SyncOwner<int>` alive until every
 *   borrow handle has been destroyed and every worker thread has joined.
 *
 * Ownership and thread-safety:
 * - `SyncOwner<T>` remains uniquely owned in every scenario. Borrow handles are
 *   non-owning and never outlive their owner.
 * - The thread tests use condition variables only to create deterministic
 *   lifetime edges; the borrow acceptance and rejection decisions are made by
 *   the production atomic borrow ledger.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force Slice 4 runtime borrow counters for this sync ownership test.
 *
 * @retval 1 Enables checked branches in `memsafe::SyncOwner<T>` and its borrow
 * handles for every infra lane that compiles this file.
 * @pre This macro must be defined before including any memsafe feature header.
 * @post Shared and mutable sync-borrow conflicts report through the configured
 * violation policy instead of being compiled out by a release lane.
 * @invariant CPP_MEMSAFE-0410-TEST needs a checked oracle for atomic
 * borrow-accounting behavior; the generic release no-checks lane validates a
 * different build configuration and must not change this source-level oracle.
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
 * @brief Select exception throwing for sync ownership violation checks.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs Slice 4 borrow-rule violations to
 * throw `memsafe::violation`.
 * @pre This macro must be defined before including `<memsafe/sync.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post Expected shared-vs-mutable conflicts can be inspected with ordinary
 * C++ exception handling inside this test process.
 * @invariant The test uses the canonical Slice 0 policy token rather than a
 * custom handler, keeping the oracle aligned with the F4 harness.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: this preprocessor selection owns no runtime
 * state and does not install a handler.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../test_harness.hpp"

#include <memsafe/owner.hpp>
#include <memsafe/sync.hpp>
#include <memsafe/violation.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace {

/**
 * @brief Mutable ledger used to verify owner destruction parity.
 *
 * @pre The ledger must outlive every `tracked_payload` that stores its address.
 * @post Construction and destruction of tracked payloads update the counters
 * directly.
 * @invariant `destroyed` never exceeds `constructed` in the successful test
 * path, and `last_destroyed_payload` records the payload value seen by the
 * most recent destructor.
 * @throws This aggregate owns no resources and does not throw.
 * @note Ownership/thread-safety: the ledger is stack-owned by the main test
 * thread and is touched only by single-threaded construction/destruction
 * parity checks.
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
 * `int` has no observable user-defined destructor, so the parity test uses
 * this instrumented payload to prove that `Owner<T>` and `SyncOwner<T>` both
 * destroy their heap objects exactly once through the same RAII shape.
 *
 * Example:
 * @code
 * destructor_counters counters;
 * {
 *     memsafe::SyncOwner<tracked_payload> owner(counters, 7);
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
 * @note Ownership/thread-safety: instances are uniquely owned by either
 * `memsafe::Owner<tracked_payload>` or
 * `memsafe::SyncOwner<tracked_payload>` and are used on one thread here.
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
     * uniquely owned by one owner wrapper.
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
     * owning wrapper's lifetime.
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
 * @brief Verify that a live immutable borrow blocks mutable borrowing.
 *
 * @tparam OwnerLike `memsafe::Owner<T>` or `memsafe::SyncOwner<T>`-like type
 * exposing `borrow_mut()`.
 * @param owner Owner whose payload currently has at least one live immutable
 * borrow.
 * @return Nothing.
 * @pre At least one immutable borrow from `owner` is live and runtime checks
 * are enabled.
 * @post The harness records that `borrow_mut()` threw `memsafe::violation`
 * with `violation_kind::borrow_exclusivity`.
 * @invariant A failed mutable borrow attempt must not mutate the payload or
 * leave a mutable token behind.
 * @throws Nothing intentionally; the expected `memsafe::violation` is caught
 * and converted into CHECK results.
 * @note Ownership/thread-safety: used on one thread for parity coverage; the
 * dedicated thread tests below cover cross-thread sync accounting.
 */
template <typename OwnerLike>
void verify_live_shared_blocks_mutable(OwnerLike& owner) {
    bool saw_violation = false;

    try {
        auto unexpected_mutable_borrow = owner.borrow_mut();
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
 * @brief Verify that a live mutable borrow blocks immutable borrowing.
 *
 * @tparam OwnerLike `memsafe::Owner<T>` or `memsafe::SyncOwner<T>`-like type
 * exposing `borrow()`.
 * @param owner Owner whose payload currently has a live mutable borrow.
 * @return Nothing.
 * @pre A mutable borrow from `owner` is live and runtime checks are enabled.
 * @post The harness records that `borrow()` threw `memsafe::violation` with
 * `violation_kind::borrow_exclusivity`.
 * @invariant A failed shared-borrow attempt must not increment the shared
 * counter or observe the payload.
 * @throws Nothing intentionally; the expected `memsafe::violation` is caught
 * and converted into CHECK results.
 * @note Ownership/thread-safety: used on one thread for parity coverage; the
 * dedicated thread tests below cover cross-thread sync accounting.
 */
template <typename OwnerLike>
void verify_live_mutable_blocks_shared(OwnerLike& owner) {
    bool saw_violation = false;

    try {
        auto unexpected_shared_borrow = owner.borrow();
        (void)unexpected_shared_borrow;
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
 * @brief Exercise one owner-like wrapper through the string borrow flow.
 *
 * @tparam OwnerLike `memsafe::Owner<std::string>` or
 * `memsafe::SyncOwner<std::string>`.
 * @param owner Owner initialized with the value `"daily"`.
 * @return Nothing.
 * @pre `owner.has_value()` is true, no borrow is live, and `*owner == "daily"`.
 * @post The owner payload has been changed to `"dailygrind"` by one mutable
 * borrow, and no borrow remains live.
 * @invariant Two immutable borrows may coexist, any mutable/shared overlap
 * reports `borrow_exclusivity`, and the owner can be borrowed mutably after
 * shared borrows end.
 * @throws Nothing intentionally; expected violations are caught by helper
 * functions and unexpected exceptions fail the process under the runner.
 * @note Ownership/thread-safety: this helper is single-threaded. For
 * `SyncOwner<T>`, that intentionally proves parity with `Owner<T>` before the
 * two-thread cases exercise atomic accounting.
 */
template <typename OwnerLike>
void exercise_owner_like_string_flow(OwnerLike& owner) {
    CHECK(owner.has_value());
    CHECK(*owner == "daily");

    {
        auto first_read = owner.borrow();
        {
            auto copied_read = first_read;
            CHECK(*first_read == "daily");
            CHECK(*copied_read == "daily");
        }

        /*
         * F1 Slice 4 says SyncOwner keeps Owner's exclusivity semantics while
         * replacing the single-thread counter with one atomic ledger. Running
         * this helper for both wrappers makes the expected parity explicit.
         */
        verify_live_shared_blocks_mutable(owner);
        CHECK(*first_read == "daily");
    }

    {
        auto mutable_read = owner.borrow_mut();
        *mutable_read = "dailygrind";
        CHECK(*mutable_read == "dailygrind");
        verify_live_mutable_blocks_shared(owner);
    }

    {
        auto final_read = owner.borrow();
        CHECK(*final_read == "dailygrind");
    }

    CHECK(*owner == "dailygrind");
}

/**
 * @brief Verify SyncOwner construction/destruction parity with Owner.
 *
 * @return Nothing.
 * @pre The test executable is compiled with the project include directory and
 * the local THROW policy selected.
 * @post The harness records failed checks for any construction, observation,
 * or destruction-count mismatch.
 * @invariant Both owner wrappers construct one heap payload and destroy it
 * exactly once when their scopes end.
 * @throws Nothing intentionally; unexpected exceptions surface as process
 * failures under the runner's exit-code model.
 * @note Ownership/thread-safety: all objects are stack-owned and single
 * threaded in this parity check.
 */
void verify_construction_and_destruction_parity() {
    destructor_counters owner_counters;
    destructor_counters sync_counters;

    {
        memsafe::Owner<tracked_payload> owner_payload(owner_counters, 61);
        memsafe::SyncOwner<tracked_payload> sync_payload(sync_counters, 61);

        CHECK(owner_payload.has_value());
        CHECK(sync_payload.has_value());
        CHECK(owner_payload->payload() == 61);
        CHECK(sync_payload->payload() == owner_payload->payload());
        CHECK(owner_counters.constructed == 1);
        CHECK(sync_counters.constructed == 1);
        CHECK(owner_counters.destroyed == 0);
        CHECK(sync_counters.destroyed == 0);
    }

    CHECK(owner_counters.constructed == 1);
    CHECK(sync_counters.constructed == 1);
    CHECK(owner_counters.destroyed == 1);
    CHECK(sync_counters.destroyed == 1);
    CHECK(owner_counters.last_destroyed_payload == 61);
    CHECK(sync_counters.last_destroyed_payload == owner_counters.last_destroyed_payload);
}

/**
 * @brief Verify single-thread SyncOwner parity with Owner.
 *
 * @return Nothing.
 * @pre Checked THROW policy is active for both owner wrappers.
 * @post The harness records failed checks for any parity mismatch in direct
 * access, shared borrowing, mutable borrowing, or exclusivity violations.
 * @invariant The same sequence of legal and intentionally conflicting borrows
 * produces the same final payload for `Owner<std::string>` and
 * `SyncOwner<std::string>`.
 * @throws Nothing intentionally; expected violations are caught by helper
 * functions.
 * @note Ownership/thread-safety: deliberately single-threaded to isolate API
 * parity from cross-thread scheduling.
 */
void verify_single_thread_functional_parity() {
    memsafe::Owner<std::string> owner(std::string("daily"));
    memsafe::SyncOwner<std::string> sync_owner(std::string("daily"));

    exercise_owner_like_string_flow(owner);
    exercise_owner_like_string_flow(sync_owner);

    CHECK(*owner == *sync_owner);
}

/**
 * @brief Results captured by the shared-live then mutable-borrow thread test.
 *
 * @pre The result object is zero-initialized before the worker threads start.
 * @post Worker threads update the atomic cells; the main thread loads them
 * after joining both workers.
 * @invariant Every field is atomic so result recording cannot race with another
 * worker or with the main thread's post-join assertions.
 * @throws Atomic construction and destruction do not throw.
 * @note Ownership/thread-safety: owns only scalar test observations, not
 * library payloads or borrow handles.
 */
struct shared_then_mut_result final {
    /// Value observed through the live shared borrow in the owner thread.
    std::atomic<int> shared_value{0};
    /// True if the first mutable borrow attempt threw the expected exception.
    std::atomic<bool> mutable_attempt_threw{false};
    /// True if the first mutable borrow attempt reported borrow exclusivity.
    std::atomic<bool> mutable_violation_kind{false};
    /// True if the first mutable borrow attempt incorrectly returned a handle.
    std::atomic<bool> mutable_attempt_returned{false};
    /// True if a mutable borrow succeeded after the shared borrow ended.
    std::atomic<bool> mutated_after_release{false};
    /// Value observed through the successful mutable borrow after release.
    std::atomic<int> mutable_value{0};
    /// Final value observed by the owner thread before owner destruction.
    std::atomic<int> final_value{0};
    /// True if the owner still reported a payload after the two-thread flow.
    std::atomic<bool> owner_stayed_live{false};
    /// True if any worker caught an unexpected exception or missing owner.
    std::atomic<bool> unexpected_exception{false};
};

/**
 * @brief Shared synchronization state for the shared-live then mutable test.
 *
 * @pre Created before both worker threads start and destroyed after both join.
 * @post Condition-variable flags describe the lifetime edges between the live
 * shared borrow, the failed mutable attempt, and the later successful mutable
 * borrow.
 * @invariant `owner` is published only while the owner thread keeps the
 * `SyncOwner<int>` alive and clears no state until the mutable worker is done.
 * @throws Mutex and condition-variable construction may throw according to the
 * C++ standard library implementation.
 * @note Ownership/thread-safety: owns synchronization primitives only; the
 * `owner` pointer is non-owning and guarded by `mutex`.
 */
struct shared_then_mut_state final {
    /// Mutex guarding all condition-variable predicates and owner publication.
    std::mutex mutex;
    /// Condition variable used to create deterministic cross-thread lifetimes.
    std::condition_variable cv;
    /// Non-owning pointer to the owner stack-owned by the owner thread.
    memsafe::SyncOwner<int>* owner = nullptr;
    /// True while the owner thread has published a live shared borrow.
    bool shared_live = false;
    /// True when the mutable worker has completed the expected failed attempt.
    bool release_shared = false;
    /// True after the owner thread has destroyed the shared borrow.
    bool shared_released = false;
    /// True after the mutable worker has completed its post-release mutation.
    bool mutation_done = false;
};

/**
 * @brief Unblock the shared-live then mutable test after an unexpected failure.
 *
 * @param state Shared synchronization state to mark complete.
 * @param result Atomic result cells that receive the unexpected-failure flag.
 * @return Nothing.
 * @pre Called by either worker when continuing normally would risk leaving the
 * peer blocked on a condition variable.
 * @post All wait predicates are true and `unexpected_exception` is recorded.
 * @invariant Failure signalling does not pretend the borrow-accounting checks
 * passed; it only lets the main thread join and report harness failures.
 * @throws Nothing intentionally; standard mutex operations are expected to
 * succeed in this test process.
 * @note Ownership/thread-safety: mutates only test synchronization state and
 * atomic result cells.
 */
void unblock_shared_then_mut_after_failure(shared_then_mut_state& state,
                                           shared_then_mut_result& result) {
    result.unexpected_exception.store(true);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.shared_live = true;
        state.release_shared = true;
        state.shared_released = true;
        state.mutation_done = true;
    }
    state.cv.notify_all();
}

/**
 * @brief Own a SyncOwner on one thread while holding a shared borrow.
 *
 * @param state Shared synchronization state for the test.
 * @param result Atomic result cells written by the worker.
 * @return Nothing.
 * @pre `state` and `result` outlive the thread running this function.
 * @post The owner has stayed alive until the mutable worker either completed
 * the post-release mutation or signalled failure.
 * @invariant This function matches F2 case 2's shape: one thread creates a
 * `SyncOwner<int>`, borrows it immutably, and keeps the owner alive while the
 * second thread attempts an exclusive mutable borrow.
 * @throws Nothing intentionally; unexpected exceptions are caught and reported
 * through `result`.
 * @note Ownership/thread-safety: the published owner pointer is non-owning and
 * valid only while this function is blocked on the test condition variable.
 */
void shared_owner_thread(shared_then_mut_state& state,
                         shared_then_mut_result& result) {
    try {
        memsafe::SyncOwner<int> owner(41);

        {
            auto read = owner.borrow();
            result.shared_value.store(*read);

            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.owner = &owner;
                state.shared_live = true;
                state.cv.notify_all();
                state.cv.wait(lock, [&state] { return state.release_shared; });
            }
        }

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.shared_released = true;
            state.cv.notify_all();
            state.cv.wait(lock, [&state] { return state.mutation_done; });
        }

        result.final_value.store(*owner);
        result.owner_stayed_live.store(owner.has_value());
    } catch (...) {
        unblock_shared_then_mut_after_failure(state, result);
    }
}

/**
 * @brief Attempt mutable borrowing while another thread holds a shared borrow.
 *
 * @param state Shared synchronization state for the test.
 * @param result Atomic result cells written by the worker.
 * @return Nothing.
 * @pre The owner thread will publish `state.owner` before setting
 * `shared_live`.
 * @post The first mutable attempt has been recorded, the shared borrow has been
 * released, and a post-release mutable borrow has either succeeded or an
 * unexpected failure has been recorded.
 * @invariant The first `borrow_mut()` call must fail while the shared borrow is
 * live; the second must succeed after the shared borrow's destructor releases
 * the atomic shared count.
 * @throws Nothing intentionally; expected and unexpected exceptions are caught
 * and recorded through `result`.
 * @note Ownership/thread-safety: this worker never stores a borrow handle past
 * the owner thread's lifetime gate.
 */
void mutable_worker_after_shared(shared_then_mut_state& state,
                                 shared_then_mut_result& result) {
    memsafe::SyncOwner<int>* owner = nullptr;

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&state] { return state.shared_live; });
        owner = state.owner;
    }

    if (owner == nullptr) {
        unblock_shared_then_mut_after_failure(state, result);
        return;
    }

    try {
        auto unexpected_mutable_borrow = owner->borrow_mut();
        (void)unexpected_mutable_borrow;
        result.mutable_attempt_returned.store(true);
    } catch (const memsafe::violation& caught) {
        result.mutable_attempt_threw.store(true);
        result.mutable_violation_kind.store(
            caught.kind() == memsafe::violation_kind::borrow_exclusivity);
    } catch (...) {
        result.unexpected_exception.store(true);
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.release_shared = true;
        state.cv.notify_all();
        state.cv.wait(lock, [&state] { return state.shared_released; });
    }

    if (!result.unexpected_exception.load()) {
        try {
            {
                auto write = owner->borrow_mut();
                *write = 73;
                result.mutable_value.store(*write);
                result.mutated_after_release.store(*write == 73);
            }
        } catch (...) {
            result.unexpected_exception.store(true);
        }
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.mutation_done = true;
    }
    state.cv.notify_all();
}

/**
 * @brief Results captured by the mutable-live then shared-borrow thread test.
 *
 * @pre The result object is zero-initialized before the worker threads start.
 * @post Worker threads update the atomic cells; the main thread loads them
 * after joining both workers.
 * @invariant Every field is atomic so result recording cannot race with another
 * worker or with the main thread's post-join assertions.
 * @throws Atomic construction and destruction do not throw.
 * @note Ownership/thread-safety: owns only scalar test observations, not
 * library payloads or borrow handles.
 */
struct mut_then_shared_result final {
    /// Value written through the live mutable borrow.
    std::atomic<int> mutable_value{0};
    /// True if the first shared borrow attempt threw the expected exception.
    std::atomic<bool> shared_attempt_threw{false};
    /// True if the first shared borrow attempt reported borrow exclusivity.
    std::atomic<bool> shared_violation_kind{false};
    /// True if the first shared borrow attempt incorrectly returned a handle.
    std::atomic<bool> shared_attempt_returned{false};
    /// True if a shared borrow succeeded after the mutable borrow ended.
    std::atomic<bool> shared_after_release{false};
    /// Value observed through the successful shared borrow after release.
    std::atomic<int> shared_value_after_release{0};
    /// True if any worker caught an unexpected exception.
    std::atomic<bool> unexpected_exception{false};
};

/**
 * @brief Shared synchronization state for the mutable-live then shared test.
 *
 * @pre Created before both worker threads start and destroyed after both join.
 * @post Condition-variable flags describe the lifetime edges between the live
 * mutable borrow, the failed shared attempt, and the later successful shared
 * borrow.
 * @invariant The `SyncOwner<int>` itself is owned by the caller and outlives
 * both worker threads.
 * @throws Mutex and condition-variable construction may throw according to the
 * C++ standard library implementation.
 * @note Ownership/thread-safety: owns synchronization primitives only.
 */
struct mut_then_shared_state final {
    /// Mutex guarding all condition-variable predicates.
    std::mutex mutex;
    /// Condition variable used to create deterministic cross-thread lifetimes.
    std::condition_variable cv;
    /// True while the writer thread has a live mutable borrow.
    bool mutable_live = false;
    /// True when the reader has completed the expected failed shared attempt.
    bool release_mutable = false;
    /// True after the writer has destroyed the mutable borrow.
    bool mutable_released = false;
};

/**
 * @brief Unblock the mutable-live then shared test after an unexpected failure.
 *
 * @param state Shared synchronization state to mark complete.
 * @param result Atomic result cells that receive the unexpected-failure flag.
 * @return Nothing.
 * @pre Called by either worker when continuing normally would risk leaving the
 * peer blocked on a condition variable.
 * @post All wait predicates are true and `unexpected_exception` is recorded.
 * @invariant Failure signalling does not pretend the borrow-accounting checks
 * passed; it only lets the main thread join and report harness failures.
 * @throws Nothing intentionally; standard mutex operations are expected to
 * succeed in this test process.
 * @note Ownership/thread-safety: mutates only test synchronization state and
 * atomic result cells.
 */
void unblock_mut_then_shared_after_failure(mut_then_shared_state& state,
                                           mut_then_shared_result& result) {
    result.unexpected_exception.store(true);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.mutable_live = true;
        state.release_mutable = true;
        state.mutable_released = true;
    }
    state.cv.notify_all();
}

/**
 * @brief Hold a mutable sync borrow while another thread tries a shared borrow.
 *
 * @param owner Sync owner shared by reference for the duration of the test.
 * @param state Shared synchronization state for the test.
 * @param result Atomic result cells written by the worker.
 * @return Nothing.
 * @pre `owner` outlives this worker thread and currently has no live borrow.
 * @post The mutable borrow has been destroyed after the reader completed its
 * expected failed shared-borrow attempt.
 * @invariant The mutable borrow's exclusive token remains live from
 * `mutable_live` until `release_mutable` is observed.
 * @throws Nothing intentionally; unexpected exceptions are caught and reported
 * through `result`.
 * @note Ownership/thread-safety: the owner is not moved or destroyed while this
 * worker holds its non-owning mutable borrow.
 */
void mutable_owner_thread(memsafe::SyncOwner<int>& owner,
                          mut_then_shared_state& state,
                          mut_then_shared_result& result) {
    try {
        {
            auto write = owner.borrow_mut();
            *write = 19;
            result.mutable_value.store(*write);

            std::unique_lock<std::mutex> lock(state.mutex);
            state.mutable_live = true;
            state.cv.notify_all();
            state.cv.wait(lock, [&state] { return state.release_mutable; });
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.mutable_released = true;
        }
        state.cv.notify_all();
    } catch (...) {
        unblock_mut_then_shared_after_failure(state, result);
    }
}

/**
 * @brief Attempt shared borrowing while another thread holds a mutable borrow.
 *
 * @param owner Sync owner shared by reference for the duration of the test.
 * @param state Shared synchronization state for the test.
 * @param result Atomic result cells written by the worker.
 * @return Nothing.
 * @pre The writer thread will set `mutable_live` after acquiring an exclusive
 * `SyncMutRef<int>`.
 * @post The failed shared attempt has been recorded, the mutable borrow has
 * been released, and a post-release shared borrow has either succeeded or an
 * unexpected failure has been recorded.
 * @invariant The first `borrow()` call must fail while the mutable token is
 * live; the second must succeed after the mutable borrow's destructor clears
 * the token.
 * @throws Nothing intentionally; expected and unexpected exceptions are caught
 * and recorded through `result`.
 * @note Ownership/thread-safety: this worker never stores a shared handle past
 * the caller-owned owner's lifetime.
 */
void shared_worker_after_mutable(memsafe::SyncOwner<int>& owner,
                                 mut_then_shared_state& state,
                                 mut_then_shared_result& result) {
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&state] { return state.mutable_live; });
    }

    try {
        auto unexpected_shared_borrow = owner.borrow();
        (void)unexpected_shared_borrow;
        result.shared_attempt_returned.store(true);
    } catch (const memsafe::violation& caught) {
        result.shared_attempt_threw.store(true);
        result.shared_violation_kind.store(
            caught.kind() == memsafe::violation_kind::borrow_exclusivity);
    } catch (...) {
        result.unexpected_exception.store(true);
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.release_mutable = true;
        state.cv.notify_all();
        state.cv.wait(lock, [&state] { return state.mutable_released; });
    }

    if (!result.unexpected_exception.load()) {
        try {
            auto read = owner.borrow();
            result.shared_value_after_release.store(*read);
            result.shared_after_release.store(*read == 19);
        } catch (...) {
            result.unexpected_exception.store(true);
        }
    }
}

/**
 * @brief Verify a live shared borrow blocks cross-thread mutable borrowing.
 *
 * @return Nothing.
 * @pre The standard library can create and join two `std::thread` objects.
 * @post The harness records failed checks for any missing violation,
 * unexpected exception, failed post-release mutation, or owner lifetime issue.
 * @invariant The owner thread creates `SyncOwner<int>` and holds a
 * `SyncRef<int>` while the mutable worker attempts `borrow_mut()`, matching
 * F2 Scope and Concurrency case 2 without exposing a data race on the payload.
 * @throws `std::system_error` may propagate if thread creation or joining
 * fails; such a failure correctly fails the test process.
 * @note Ownership/thread-safety: every borrow handle is destroyed before the
 * creating owner thread lets its stack-owned `SyncOwner<int>` go out of scope.
 */
void verify_two_thread_shared_blocks_mutable() {
    shared_then_mut_state state;
    shared_then_mut_result result;

    std::thread owner_thread(shared_owner_thread,
                             std::ref(state),
                             std::ref(result));
    std::thread mutable_thread(mutable_worker_after_shared,
                               std::ref(state),
                               std::ref(result));

    mutable_thread.join();
    owner_thread.join();

    CHECK(!result.unexpected_exception.load());
    CHECK(result.shared_value.load() == 41);
    CHECK(result.mutable_attempt_threw.load());
    CHECK(result.mutable_violation_kind.load());
    CHECK(!result.mutable_attempt_returned.load());
    CHECK(result.mutated_after_release.load());
    CHECK(result.mutable_value.load() == 73);
    CHECK(result.final_value.load() == 73);
    CHECK(result.owner_stayed_live.load());
}

/**
 * @brief Verify a live mutable borrow blocks cross-thread shared borrowing.
 *
 * @return Nothing.
 * @pre The standard library can create and join two `std::thread` objects.
 * @post The harness records failed checks for any missing violation,
 * unexpected exception, failed post-release shared borrow, or payload mismatch.
 * @invariant The writer thread holds a `SyncMutRef<int>` while the reader
 * attempts `borrow()`, proving the atomic exclusive token blocks shared
 * acquisition from another thread.
 * @throws `std::system_error` may propagate if thread creation or joining
 * fails; such a failure correctly fails the test process.
 * @note Ownership/thread-safety: the caller-owned `SyncOwner<int>` outlives
 * both workers and is not moved or destroyed while borrowed.
 */
void verify_two_thread_mutable_blocks_shared() {
    memsafe::SyncOwner<int> owner(11);
    mut_then_shared_state state;
    mut_then_shared_result result;

    std::thread writer_thread(mutable_owner_thread,
                              std::ref(owner),
                              std::ref(state),
                              std::ref(result));
    std::thread reader_thread(shared_worker_after_mutable,
                              std::ref(owner),
                              std::ref(state),
                              std::ref(result));

    reader_thread.join();
    writer_thread.join();

    CHECK(!result.unexpected_exception.load());
    CHECK(result.mutable_value.load() == 19);
    CHECK(result.shared_attempt_threw.load());
    CHECK(result.shared_violation_kind.load());
    CHECK(!result.shared_attempt_returned.load());
    CHECK(result.shared_after_release.load());
    CHECK(result.shared_value_after_release.load() == 19);
    CHECK(*owner == 19);
}

} // namespace

/**
 * @brief Run the Slice 4 sync ownership unit tests.
 *
 * @return Zero when all sync ownership checks pass; nonzero when the harness
 * recorded any failed CHECK.
 * @pre The executable is built as a standalone F4 runtime test with
 * `testing/test_harness.hpp`, the project include directory, and the standard
 * threading library available.
 * @post The test harness prints a summary and returns its process verdict.
 * @invariant Coverage is limited to CPP_MEMSAFE-0410-TEST acceptance:
 * single-thread parity with `Owner<T>` and two-thread shared/exclusive
 * sync-borrow accounting.
 * @throws Nothing intentionally; unexpected exceptions are allowed to
 * terminate the process under the runner's exit-code model.
 * @note Ownership/thread-safety: each thread scenario joins all worker threads
 * before the owning `SyncOwner<int>` is destroyed.
 */
int main() {
    verify_construction_and_destruction_parity();
    verify_single_thread_functional_parity();
    verify_two_thread_shared_blocks_mutable();
    verify_two_thread_mutable_blocks_shared();

    RUN_TESTS("test_sync_ownership");
}
