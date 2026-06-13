/**
 * @file test_mutex.cpp
 * @brief Runtime unit coverage for Slice 5 `memsafe::Mutex<T>`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0520-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0500-FUNC `memsafe::Mutex<T>` API through
 *   construction, `lock()`, mutable access through the returned `MutRef<T>`,
 *   and the debug-check `has_borrow()` observer.
 * - Verify F1 Slice 5 and the PRD concurrency-type requirement that
 *   `Mutex<T>` wraps a payload and returns a `MutRef<T>` from `lock()`.
 * - Verify F2 Scope and Concurrency case 3: two threads increment a
 *   `Mutex<int>` counter through `lock()`, and the final value reflects every
 *   attempted increment with no lost updates.
 *
 * Key invariants:
 * - The test owns exactly one `memsafe::Mutex<int>` for the full lifetime of
 *   both worker threads.
 * - Every counter mutation occurs only while a `MutRef<int>` returned by
 *   `Mutex<int>::lock()` is alive.
 * - Worker threads never call the F4 `CHECK` macro. They write atomic result
 *   cells, and the main thread performs all harness assertions after joining.
 *
 * Ownership and thread-safety:
 * - The mutex object has automatic storage in the main thread and is never
 *   moved, copied, or destroyed until both workers have joined.
 * - The standard-library condition variable is test-only scheduling support;
 *   mutual exclusion of the payload is provided solely by `memsafe::Mutex<int>`.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force runtime borrow-ledger checks for the Slice 5 Mutex unit oracle.
 *
 * @retval 1 Enables checked `Mutex<T>` borrow bookkeeping and the
 * `has_borrow()` observer in every build lane that compiles this file.
 * @pre This macro must be defined before including any memsafe feature header.
 * @post The test can assert that each live lock records one debug mutable
 * borrow and that the token clears after the guard is destroyed.
 * @invariant CPP_MEMSAFE-0520-TEST validates checked Mutex lock semantics; a
 * release no-check lane must not remove the source-level oracle from this test.
 * @throws Nothing directly; the macro only selects compile-time library code.
 * @note Ownership/thread-safety: this macro owns no storage and affects only
 * this translation unit.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#include "../test_harness.hpp"

#include <memsafe/sync.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace {

/// Number of worker threads required by F2 Scope and Concurrency case 3.
constexpr int k_worker_count = 2;

/// Per-thread increment count; high enough to expose lost updates in practice.
constexpr int k_iterations_per_worker = 10000;

/**
 * @brief Increment a shared `memsafe::Mutex<int>` after a coordinated start.
 *
 * @param counter Shared mutex-protected integer counter under test.
 * @param start_mutex Mutex guarding the start predicate used by this test.
 * @param start_cv Condition variable that releases both workers together.
 * @param start_requested Predicate set by the main thread to begin increments.
 * @param ready_workers Atomic count of workers that reached the start gate.
 * @param completed_workers Atomic count of workers that finished all loops.
 * @param missing_borrow_observed Atomic flag set if a live lock fails to report
 * a debug-check borrow token.
 * @param unexpected_exception Atomic flag set if any increment path throws.
 * @return Nothing.
 * @pre `counter`, synchronization primitives, predicate storage, and atomic
 * result cells outlive the worker thread executing this function.
 * @post On success, this worker has added exactly `k_iterations_per_worker` to
 * `counter` and incremented `completed_workers`.
 * @invariant Each payload write occurs between `counter.lock()` and destruction
 * of the returned `MutRef<int>`, satisfying the F1 Slice 5 Mutex contract.
 * @throws Nothing intentionally; all exceptions are caught and reported through
 * `unexpected_exception` so the main thread can turn them into harness checks.
 * @note Ownership/thread-safety: the function takes only non-owning references.
 * It never calls `CHECK` because the F4 harness failure counter is not
 * synchronized for concurrent writes.
 *
 * Example:
 * @code
 * memsafe::Mutex<int> counter(0);
 * // Spawn two std::thread objects running this function, then join them.
 * @endcode
 */
void increment_mutex_worker(memsafe::Mutex<int>& counter,
                            std::mutex& start_mutex,
                            std::condition_variable& start_cv,
                            const bool& start_requested,
                            std::atomic<int>& ready_workers,
                            std::atomic<int>& completed_workers,
                            std::atomic<bool>& missing_borrow_observed,
                            std::atomic<bool>& unexpected_exception) noexcept {
    try {
        ready_workers.fetch_add(1, std::memory_order_acq_rel);

        {
            std::unique_lock<std::mutex> start_lock(start_mutex);
            start_cv.wait(start_lock, [&start_requested]() noexcept {
                return start_requested;
            });
        }

        for (int iteration = 0; iteration < k_iterations_per_worker;
             ++iteration) {
            auto locked = counter.lock();

            /*
             * CPP_MEMSAFE-0500-FUNC requires `lock()` to yield an exclusive
             * `MutRef<T>`. In checked lanes, `has_borrow()` is the public
             * observer that the returned guard recorded that exclusive token.
             */
            if (!counter.has_borrow()) {
                missing_borrow_observed.store(true, std::memory_order_release);
            }

            ++*locked;
        }

        completed_workers.fetch_add(1, std::memory_order_acq_rel);
    } catch (...) {
        unexpected_exception.store(true, std::memory_order_release);
    }
}

/**
 * @brief Verify that two workers serialize all `Mutex<int>` increments.
 *
 * @return Nothing.
 * @pre The standard library can create and join two `std::thread` objects.
 * @post The F4 harness records failed checks for missing worker completion,
 * unexpected exceptions, missing checked borrow tokens, leaked borrow tokens,
 * or an incorrect final counter value.
 * @invariant The only mutable access to the counter payload is through
 * `Mutex<int>::lock()`, directly covering F2 Scope and Concurrency case 3.
 * @throws `std::system_error` may propagate if thread construction or joining
 * fails; such infrastructure failure correctly fails the test process.
 * @note Ownership/thread-safety: the `memsafe::Mutex<int>` remains alive until
 * both workers have joined, and all harness checks run on the main thread.
 */
void verify_two_thread_mutex_increment_serializes() {
    memsafe::Mutex<int> counter(0);
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start_requested = false;
    std::atomic<int> ready_workers{0};
    std::atomic<int> completed_workers{0};
    std::atomic<bool> missing_borrow_observed{false};
    std::atomic<bool> unexpected_exception{false};

    std::thread first_worker(increment_mutex_worker,
                             std::ref(counter),
                             std::ref(start_mutex),
                             std::ref(start_cv),
                             std::cref(start_requested),
                             std::ref(ready_workers),
                             std::ref(completed_workers),
                             std::ref(missing_borrow_observed),
                             std::ref(unexpected_exception));
    std::thread second_worker(increment_mutex_worker,
                              std::ref(counter),
                              std::ref(start_mutex),
                              std::ref(start_cv),
                              std::cref(start_requested),
                              std::ref(ready_workers),
                              std::ref(completed_workers),
                              std::ref(missing_borrow_observed),
                              std::ref(unexpected_exception));

    while (ready_workers.load(std::memory_order_acquire) < k_worker_count) {
        std::this_thread::yield();
    }

    {
        std::lock_guard<std::mutex> start_lock(start_mutex);
        start_requested = true;
    }
    start_cv.notify_all();

    first_worker.join();
    second_worker.join();

    int final_value = 0;
    bool final_lock_recorded_borrow = false;
    {
        auto locked = counter.lock();
        final_lock_recorded_borrow = counter.has_borrow();
        final_value = *locked;
    }

    CHECK(!unexpected_exception.load(std::memory_order_acquire));
    CHECK(completed_workers.load(std::memory_order_acquire) == k_worker_count);
    CHECK(!missing_borrow_observed.load(std::memory_order_acquire));
    CHECK(final_lock_recorded_borrow);
    CHECK(!counter.has_borrow());
    CHECK(final_value == k_worker_count * k_iterations_per_worker);
}

} // namespace

/**
 * @brief Run the Slice 5 Mutex unit test executable.
 *
 * @return Zero when all Mutex checks pass; nonzero when the F4 harness recorded
 * any failed `CHECK`.
 * @pre The executable is built as a standalone runtime test with
 * `testing/test_harness.hpp`, the project include directory, and standard
 * threading support available.
 * @post The F4 harness prints the test summary and returns the process verdict.
 * @invariant Coverage is limited to CPP_MEMSAFE-0520-TEST: a two-thread
 * `Mutex<int>` increment scenario plus direct public lock/borrow observation.
 * @throws Nothing intentionally; unexpected infrastructure exceptions may
 * terminate the process under the runner's exit-code model.
 * @note Ownership/thread-safety: `main()` performs all harness checks after the
 * two worker threads have joined.
 */
int main() {
    verify_two_thread_mutex_increment_serializes();

    RUN_TESTS("test_mutex");
}
