/**
 * @file bench_primitives.cpp
 * @brief Chrono performance gate for the memsafe primitive wrappers.
 *
 * @details
 * Work package: CPP_MEMSAFE-0910-TEST.
 *
 * Purpose:
 * - Benchmark the release/no-checks cost of the public primitive families
 *   named by the package: `Owner`/`MutRef`, `SlotMap`/`Handle`, `Arc`, and
 *   `Mutex`.
 * - Compare those primitives against the accepted raw, `std::unique_ptr`,
 *   `std::shared_ptr`, and `std::mutex` baselines without introducing an
 *   external google/benchmark link dependency.
 * - Fail the process when any required slowdown ratio exceeds 1.05 or when
 *   the geometric mean of all required ratios exceeds 1.05.
 *
 * Key invariants:
 * - The benchmark is authoritative only for `MEMSAFE_RELEASE_CHECKS=0`, the
 *   F1/F2 release lane where runtime checks are required to compile away.
 * - Each measured operation isolates the primitive access, copy/drop, lock, or
 *   lookup path under review. The only payload work inside the timed region is
 *   one unsigned mutation and one optimizer barrier needed to keep the access
 *   observable.
 * - The default ratio budget is exactly 1.05, matching the F1 NFR Performance
 *   and F2 performance-benchmark threshold.
 * - The test remains a self-contained F4 executable: CTest observes only the
 *   process exit code, and the implementation uses `chrono_bench.hpp` plus
 *   `std::chrono` rather than any linked benchmark framework.
 *
 * Ownership and thread-safety:
 * - Every benchmark fixture is owned by `main()` and measured on one thread.
 * - The `Mutex` comparison intentionally exercises uncontended locking because
 *   the package measures primitive overhead, not scheduler behavior.
 */
#include <memsafe/memsafe.hpp>

#include "../support/chrono_bench.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

#if MEMSAFE_RELEASE_CHECKS == 0

namespace bench = memsafe::test_support::chrono_bench;

/**
 * @brief Maximum accepted slowdown ratio for every primitive comparison.
 *
 * @return Constant expression equal to the F2 5 percent threshold.
 * @pre None.
 * @post No state is modified.
 * @invariant The value must remain synchronized with `bench_config::max_ratio`
 * so per-comparison and suite-level gates use the same budget.
 * @throws Nothing; this is a compile-time constant.
 * @note Ownership/thread-safety: the constant owns no storage with dynamic
 * lifetime and introduces no synchronization.
 */
constexpr double k_max_ratio = 1.05;

/**
 * @brief Create the shared benchmark configuration for the performance lane.
 *
 * @return `chrono_bench` configuration using the package's 1.05 budget.
 * @pre The executable is running in the release/no-checks perf lane.
 * @post No global state is modified.
 * @invariant Warm-up samples are separated from measured samples, and the
 * operation count is high enough that primitive-only measurements do not become
 * zero-duration timer artifacts on ordinary CI hosts.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns a value object with no shared state.
 *
 * Example:
 * @code
 * const auto config = make_bench_config();
 * @endcode
 */
bench::bench_config make_bench_config() noexcept {
    bench::bench_config config;
    config.warmup_repetitions = 5U;
    config.measured_repetitions = 13U;
    config.operations_per_repetition = 500000U;
    config.max_ratio = k_max_ratio;
    return config;
}

/**
 * @brief Mutate one payload just enough to keep a primitive access observable.
 *
 * @param value Payload value reached through the primitive or baseline under
 * test.
 * @param salt Monotonic per-side salt that prevents a constant update stream.
 * @return Nothing.
 * @pre `value` is a live object accessed according to the benchmark fixture's
 * ownership or locking rules.
 * @post `value` has been updated and made visible to the optimizer barrier.
 * @invariant The mutation is intentionally one unsigned addition. It is not a
 * benchmark payload; it is the minimum side effect used to prevent the compiler
 * from deleting the access being measured.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows `value` for the duration of the call
 * and performs no synchronization.
 *
 * Example:
 * @code
 * std::uint64_t payload = 1;
 * std::uint64_t salt = 0;
 * increment_observed_payload(payload, salt);
 * @endcode
 */
void increment_observed_payload(std::uint64_t& value,
                                std::uint64_t& salt) noexcept {
    value += ++salt;
    bench::do_not_optimize(value);
}

/*
 * SPEC AMBIGUITY (resolved by reviewer consensus) -- DEC-0002:
 * F1/F2 require a <=5 percent release/no-checks slowdown gate but do not spell
 * out whether the timed operation may include a stabilizing synthetic payload.
 * The prior version included a 16-round hash-like payload in both baseline and
 * candidate timings, which made the ratio primarily measure common arithmetic
 * instead of primitive access/copy/lock/lookup overhead. DEC-0002 applies the
 * agreed reading: each measured operation below isolates the primitive cost and
 * includes only the minimal payload mutation plus optimizer barrier needed for
 * correctness before the 1.05 ratio gate is applied.
 */

/**
 * @brief Benchmark `Owner<T>::borrow_mut()` against a `unique_ptr` raw view.
 *
 * @param config Shared chrono benchmark configuration.
 * @return Candidate-vs-baseline comparison for the Owner primitive family.
 * @pre Runtime checks are disabled by `MEMSAFE_RELEASE_CHECKS=0`.
 * @post The owned payloads have been mutated many times; ownership remains
 * local to the benchmark fixture.
 * @invariant The baseline uses `unique_ptr::get()` to model the raw mutable
 * view commonly used with `std::unique_ptr`, while the candidate uses the
 * public `Owner<T>::borrow_mut()` path required by Slice 1.
 * @throws Allocation for the initial fixtures can throw `std::bad_alloc`;
 * benchmark helper allocation can also throw.
 * @note Ownership/thread-safety: all state is owned by this function and used
 * on the calling thread only.
 *
 * Example:
 * @code
 * auto result = benchmark_owner(make_bench_config());
 * @endcode
 */
bench::comparison_result benchmark_owner(const bench::bench_config& config) {
    std::unique_ptr<std::uint64_t> baseline =
        std::make_unique<std::uint64_t>(1U);
    memsafe::Owner<std::uint64_t> candidate(1U);
    std::uint64_t baseline_salt = 0U;
    std::uint64_t candidate_salt = 0U;

    return bench::compare(
        "owner.borrow_mut_vs_unique_ptr_get", config,
        [&] {
            std::uint64_t* const raw = baseline.get();
            increment_observed_payload(*raw, baseline_salt);
        },
        [&] {
            auto borrow = candidate.borrow_mut();
            increment_observed_payload(*borrow, candidate_salt);
        });
}

/**
 * @brief Benchmark generation-handle lookup against a raw pointer baseline.
 *
 * @param config Shared chrono benchmark configuration.
 * @return Candidate-vs-baseline comparison for `SlotMap<T>` and `Handle<T>`.
 * @pre Runtime checks are disabled by `MEMSAFE_RELEASE_CHECKS=0`.
 * @post The raw and slot-map payloads have been mutated; the slot-map handle
 * remains live until the fixture is destroyed.
 * @invariant The candidate uses `SlotMap<T>::deref(handle)` so the measured
 * path includes the release-lane generation lookup required for handle safety.
 * The baseline is a direct raw pointer to the same payload shape.
 * @throws Slot allocation or benchmark helper allocation can throw.
 * @note Ownership/thread-safety: the fixture is single-threaded; no concurrent
 * access to the raw value or slot-map payload occurs.
 *
 * Example:
 * @code
 * auto result = benchmark_slotmap(make_bench_config());
 * @endcode
 */
bench::comparison_result benchmark_slotmap(const bench::bench_config& config) {
    std::uint64_t baseline_value = 1U;
    std::uint64_t* const baseline = &baseline_value;
    memsafe::SlotMap<std::uint64_t, 1U> candidate_map;
    const auto handle = candidate_map.allocate(1U);
    std::uint64_t baseline_salt = 0U;
    std::uint64_t candidate_salt = 0U;

    return bench::compare(
        "slotmap.handle_deref_vs_raw_pointer", config,
        [&] {
            increment_observed_payload(*baseline, baseline_salt);
        },
        [&] {
            increment_observed_payload(candidate_map.deref(handle),
                                       candidate_salt);
        });
}

/**
 * @brief Benchmark `Arc<T>` copy/drop against `std::shared_ptr<T>` copy/drop.
 *
 * @param config Shared chrono benchmark configuration.
 * @return Candidate-vs-baseline comparison for the shared-ownership primitive.
 * @pre Runtime checks are disabled by `MEMSAFE_RELEASE_CHECKS=0`.
 * @post Both shared payloads have been mutated through short-lived copies, and
 * the root owners remain live until the function returns.
 * @invariant Each operation retains one additional shared owner, touches the
 * payload through that retained owner, and releases it at scope exit. No
 * synthetic payload dominates the copy/drop path being measured.
 * @throws Allocation for initial fixtures or benchmark result storage can
 * throw. `Arc<T>` retain may report a violation only on refcount exhaustion,
 * which this bounded benchmark does not approach.
 * @note Ownership/thread-safety: refcount operations are atomic, but this
 * benchmark uses one thread and therefore does not measure contention.
 *
 * Example:
 * @code
 * auto result = benchmark_arc(make_bench_config());
 * @endcode
 */
bench::comparison_result benchmark_arc(const bench::bench_config& config) {
    std::shared_ptr<std::uint64_t> baseline =
        std::make_shared<std::uint64_t>(1U);
    memsafe::Arc<std::uint64_t> candidate(1U);
    std::uint64_t baseline_salt = 0U;
    std::uint64_t candidate_salt = 0U;

    return bench::compare(
        "arc.copy_drop_vs_shared_ptr_copy_drop", config,
        [&] {
            std::shared_ptr<std::uint64_t> retained = baseline;
            increment_observed_payload(*retained, baseline_salt);
            bench::do_not_optimize(retained);
        },
        [&] {
            memsafe::Arc<std::uint64_t> retained(candidate);
            increment_observed_payload(*retained, candidate_salt);
            bench::do_not_optimize(retained);
        });
}

/**
 * @brief Benchmark uncontended `Mutex<T>::lock()` against `std::mutex`.
 *
 * @param config Shared chrono benchmark configuration.
 * @return Candidate-vs-baseline comparison for the mutex wrapper.
 * @pre Runtime checks are disabled by `MEMSAFE_RELEASE_CHECKS=0`.
 * @post Both guarded payloads have been mutated under their respective locks.
 * @invariant The baseline and candidate both acquire an uncontended mutex,
 * minimally mutate the payload, and release the mutex through RAII before
 * returning from one benchmark operation.
 * @throws `std::mutex::lock()` or `memsafe::Mutex<T>::lock()` may throw
 * `std::system_error`; benchmark helper allocation can throw.
 * @note Ownership/thread-safety: the measurement is single-threaded by design
 * so the ratio captures wrapper overhead rather than scheduler contention.
 *
 * Example:
 * @code
 * auto result = benchmark_mutex(make_bench_config());
 * @endcode
 */
bench::comparison_result benchmark_mutex(const bench::bench_config& config) {
    std::mutex baseline_mutex;
    std::uint64_t baseline_value = 1U;
    memsafe::Mutex<std::uint64_t> candidate(1U);
    std::uint64_t baseline_salt = 0U;
    std::uint64_t candidate_salt = 0U;

    return bench::compare(
        "mutex.lock_vs_std_mutex_lock_guard", config,
        [&] {
            std::lock_guard<std::mutex> lock(baseline_mutex);
            increment_observed_payload(baseline_value, baseline_salt);
        },
        [&] {
            auto lock = candidate.lock();
            increment_observed_payload(*lock, candidate_salt);
        });
}

/**
 * @brief Return whether a comparison has a finite positive in-budget ratio.
 *
 * @param comparison Comparison result produced by `chrono_bench::compare`.
 * @retval true The ratio is finite, positive, and no greater than its budget.
 * @retval false The ratio is non-finite, non-positive, or over budget.
 * @pre `comparison` is a completed benchmark comparison.
 * @post No state is modified.
 * @invariant This predicate is stricter than `comparison_result::passed()` so
 * a NaN ratio cannot accidentally satisfy the `ratio <= max_ratio` check.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows immutable comparison state only.
 */
bool comparison_within_budget(
    const bench::comparison_result& comparison) noexcept {
    return std::isfinite(comparison.ratio) && comparison.ratio > 0.0 &&
           comparison.ratio <= comparison.max_ratio;
}

/**
 * @brief Print one comparison row for human-readable CI diagnostics.
 *
 * @param comparison Comparison result to print.
 * @return Nothing.
 * @pre `comparison` has been produced by `chrono_bench::compare`.
 * @post A single line has been written to standard output.
 * @invariant Reported ratios are the exact values used by the failure gate.
 * @throws I/O stream operations may set stream error state; exceptions are not
 * enabled by this test.
 * @note Ownership/thread-safety: writes to process-wide `std::cout` from the
 * calling thread only.
 */
void print_comparison(const bench::comparison_result& comparison) {
    std::cout << std::left << std::setw(42) << comparison.name << " baseline="
              << std::right << std::setw(9) << std::fixed
              << std::setprecision(3)
              << comparison.baseline.median_ns_per_operation << " ns"
              << " candidate=" << std::setw(9)
              << comparison.candidate.median_ns_per_operation << " ns"
              << " ratio=" << std::setw(7) << std::setprecision(4)
              << comparison.ratio
              << (comparison_within_budget(comparison) ? " PASS" : " FAIL")
              << '\n';
}

/**
 * @brief Print all benchmark rows plus the geometric-mean summary.
 *
 * @param suite Suite result produced by `chrono_bench::summarize`.
 * @return Nothing.
 * @pre `suite.comparisons` contains the required primitive comparisons.
 * @post A diagnostic table has been written to standard output.
 * @invariant The geometric mean row uses the same ratio budget as the
 * per-comparison rows.
 * @throws I/O stream operations may set stream error state; exceptions are not
 * enabled by this test.
 * @note Ownership/thread-safety: writes to process-wide `std::cout` from the
 * calling thread only.
 */
void print_suite(const bench::suite_result& suite) {
    std::cout << "CPP_MEMSAFE-0910-TEST primitive performance gate\n";
    std::cout << "budget: ratio <= " << std::fixed << std::setprecision(2)
              << suite.max_ratio << " in MEMSAFE_RELEASE_CHECKS=0\n";
    for (std::vector<bench::comparison_result>::const_iterator it =
             suite.comparisons.begin();
         it != suite.comparisons.end(); ++it) {
        print_comparison(*it);
    }
    std::cout << "geometric_mean_ratio=" << std::fixed << std::setprecision(4)
              << suite.geometric_mean_ratio
              << ((std::isfinite(suite.geometric_mean_ratio) &&
                   suite.geometric_mean_ratio > 0.0 &&
                   suite.geometric_mean_ratio <= suite.max_ratio)
                      ? " PASS"
                      : " FAIL")
              << '\n';
}

/**
 * @brief Return whether the suite satisfies every required budget.
 *
 * @param suite Suite result produced by `chrono_bench::summarize`.
 * @retval true Every comparison ratio and the geometric mean are finite,
 * positive, and no greater than 1.05.
 * @retval false At least one required ratio or the geometric mean is invalid
 * or over budget.
 * @pre `suite` contains all required primitive comparisons.
 * @post No state is modified.
 * @invariant A passing geometric mean cannot hide a single over-budget
 * primitive comparison, matching the CPP_MEMSAFE-0910-TEST acceptance clause.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads immutable suite state only.
 */
bool suite_within_budget(const bench::suite_result& suite) noexcept {
    if (!std::isfinite(suite.geometric_mean_ratio) ||
        suite.geometric_mean_ratio <= 0.0 ||
        suite.geometric_mean_ratio > suite.max_ratio) {
        return false;
    }

    for (std::vector<bench::comparison_result>::const_iterator it =
             suite.comparisons.begin();
         it != suite.comparisons.end(); ++it) {
        if (!comparison_within_budget(*it)) {
            return false;
        }
    }
    return true;
}

#endif /* MEMSAFE_RELEASE_CHECKS == 0 */

} // namespace

/**
 * @brief Run the release-lane primitive performance benchmark suite.
 *
 * @return Zero when every required ratio and the geometric mean are within the
 * 1.05 budget, or when a non-release lane compiles the file and therefore skips
 * this release-only gate; nonzero when any required release-lane budget fails.
 * @pre The authoritative performance verdict must be taken from a Release
 * build with `MEMSAFE_RELEASE_CHECKS=0`, as required by F1/F2.
 * @post The process has printed benchmark diagnostics and exits with the
 * verdict consumed by the F4 CTest lane.
 * @invariant This executable never links google/benchmark; the package
 * acceptance requires the F4 drop-in chrono form.
 * @throws Unexpected exceptions are caught and converted to exit code 2 so
 * benchmark infrastructure failures cannot be mistaken for a passing ratio.
 * @note Ownership/thread-safety: all benchmark fixtures are local to the main
 * thread and are destroyed before exit.
 */
int main() {
#if MEMSAFE_RELEASE_CHECKS != 0
    /*
     * The PRD and test-plan clauses cited by this WP define the <=5 percent
     * budget only for the release/no-checks mode. Returning success here keeps
     * accidental debug inclusion from producing an irrelevant performance
     * failure while the perf lane still runs the real gate.
     */
    std::cout << "SKIP: CPP_MEMSAFE-0910-TEST requires "
                 "MEMSAFE_RELEASE_CHECKS=0 for an authoritative verdict\n";
    return 0;
#else
    try {
        const bench::bench_config config = make_bench_config();
        std::vector<bench::comparison_result> comparisons;
        comparisons.reserve(4U);
        comparisons.push_back(benchmark_owner(config));
        comparisons.push_back(benchmark_slotmap(config));
        comparisons.push_back(benchmark_arc(config));
        comparisons.push_back(benchmark_mutex(config));

        const bench::suite_result suite =
            bench::summarize(comparisons, k_max_ratio);
        print_suite(suite);
        return suite_within_budget(suite) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: CPP_MEMSAFE-0910-TEST benchmark exception: "
                  << ex.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "ERROR: CPP_MEMSAFE-0910-TEST benchmark unknown exception\n";
        return 2;
    }
#endif
}
