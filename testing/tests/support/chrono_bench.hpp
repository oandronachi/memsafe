/**
 * @file chrono_bench.hpp
 * @brief Header-only chrono microbenchmark helpers for F4 performance tests.
 *
 * @details
 * Work package: CPP_MEMSAFE-0035-TEST.
 *
 * Purpose:
 * - Measure per-operation timings with `std::chrono::steady_clock`.
 * - Compare candidate operations to baseline operations and compute slowdown
 *   ratios.
 * - Compute geometric means across ratios so F2's <=5 percent performance
 *   checks can be expressed without linking google/benchmark.
 *
 * Key invariants:
 * - Warm-up repetitions are separated from measured repetitions.
 * - Per-operation nanoseconds are derived from measured elapsed time divided by
 *   the configured operation count.
 * - The default maximum ratio is 1.05, matching the F2 performance gate.
 * - This helper is for reviewable microchecks in F4; true google/benchmark
 *   integration remains optional future infrastructure.
 *
 * Ownership and thread-safety:
 * - Result objects own their vectors and names.
 * - Independent benchmarks can run concurrently when their operations do not
 *   share mutable state.
 */
#ifndef MEMSAFE_TEST_SUPPORT_CHRONO_BENCH_HPP
#define MEMSAFE_TEST_SUPPORT_CHRONO_BENCH_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace memsafe {
namespace test_support {
namespace chrono_bench {

/**
 * @brief Configuration for chrono-based microbenchmarks.
 *
 * @pre Production performance tests should keep enough repetitions and
 * operations per repetition to make a <=5 percent review meaningful. Smoke
 * tests may lower the values to keep baseline validation fast.
 * @post The configuration owns no external resources.
 * @invariant `max_ratio` defaults to 1.05, the F2 slowdown limit.
 * @throws Nothing for ordinary construction and assignment.
 * @note Thread-safety: immutable configurations can be shared across threads.
 */
struct bench_config {
    /// Number of unmeasured warm-up repetitions before sampling.
    std::size_t warmup_repetitions = 3;
    /// Number of measured repetitions used to select the median sample.
    std::size_t measured_repetitions = 9;
    /// Number of operations executed inside each repetition.
    std::size_t operations_per_repetition = 10000;
    /// Maximum accepted slowdown ratio; F2 uses 1.05.
    double max_ratio = 1.05;
};

/**
 * @brief Timing result for one operation under one benchmark configuration.
 *
 * @pre Obtain from `measure` or construct directly for helper tests.
 * @post The result owns its name and sample vector.
 * @invariant `median_ns_per_operation` is the median of `samples_ns_per_op`
 * when produced by `measure`.
 * @throws Copying or assigning can throw `std::bad_alloc`.
 * @note Thread-safety: immutable results can be read concurrently.
 */
struct bench_result {
    /// Stable benchmark name.
    std::string name;
    /// Configuration used for the measurement.
    bench_config config;
    /// Per-repetition nanoseconds per operation.
    std::vector<double> samples_ns_per_op;
    /// Median nanoseconds per operation across measured repetitions.
    double median_ns_per_operation = 0.0;
};

/**
 * @brief Candidate-vs-baseline comparison result.
 *
 * @pre Obtain from `compare` or construct directly for tests of summary logic.
 * @post The result owns both benchmark result objects and its display name.
 * @invariant `ratio == candidate.median_ns_per_operation /
 * baseline.median_ns_per_operation` when produced by `compare`.
 * @throws Copying or assigning can throw `std::bad_alloc`.
 * @note Thread-safety: immutable results can be read concurrently.
 */
struct comparison_result {
    /// Stable comparison name.
    std::string name;
    /// Baseline measurement, such as a raw pointer or STL operation.
    bench_result baseline;
    /// Candidate measurement, such as the memsafe operation under review.
    bench_result candidate;
    /// Candidate divided by baseline. Values above 1.0 are slower.
    double ratio = 1.0;
    /// Maximum accepted ratio for this comparison.
    double max_ratio = 1.05;

    /**
     * @brief Report whether this comparison satisfies its slowdown budget.
     *
     * @return `true` when `ratio <= max_ratio`; `false` otherwise.
     * @pre `ratio` and `max_ratio` should be finite positive values.
     * @post The comparison is not modified.
     * @invariant The F2 default budget is represented by `max_ratio == 1.05`.
     * @throws Nothing.
     * @note Ownership/thread-safety: reads immutable scalar fields only.
     */
    bool passed() const noexcept {
        return ratio <= max_ratio;
    }
};

/**
 * @brief Aggregate benchmark-suite result with geometric mean slowdown.
 *
 * @pre Obtain from `summarize` or construct directly for helper tests.
 * @post The result owns its comparison vector.
 * @invariant `geometric_mean_ratio` is computed from comparison ratios when
 * produced by `summarize`.
 * @throws Copying or assigning can throw `std::bad_alloc`.
 * @note Thread-safety: immutable results can be read concurrently.
 */
struct suite_result {
    /// Per-operation comparisons included in the suite.
    std::vector<comparison_result> comparisons;
    /// Geometric mean of all comparison ratios.
    double geometric_mean_ratio = 1.0;
    /// Maximum accepted ratio for the geometric mean.
    double max_ratio = 1.05;

    /**
     * @brief Report whether every comparison and the geometric mean pass.
     *
     * @return `true` when all comparisons pass and
     * `geometric_mean_ratio <= max_ratio`; `false` otherwise.
     * @pre Comparison ratios should be finite positive values.
     * @post The suite result is not modified.
     * @invariant A single over-budget comparison fails the suite even if the
     * geometric mean passes.
     * @throws Nothing.
     * @note Ownership/thread-safety: reads immutable fields only.
     */
    bool passed() const noexcept {
        if (geometric_mean_ratio > max_ratio) {
            return false;
        }
        for (std::vector<comparison_result>::const_iterator it = comparisons.begin();
             it != comparisons.end(); ++it) {
            if (!it->passed()) {
                return false;
            }
        }
        return true;
    }
};

/**
 * @brief Inhibit optimizing away a value used by a microbenchmark.
 *
 * @tparam T Observed value type.
 * @param value Value whose address is made visible to the compiler barrier.
 * @pre `value` must remain alive for the duration of the call.
 * @post The value is not modified.
 * @invariant The helper creates a compiler barrier only; it is not a hardware
 * memory fence for inter-thread communication.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows `value` and owns no state.
 *
 * Example:
 * @code
 * int x = 42;
 * memsafe::test_support::chrono_bench::do_not_optimize(x);
 * @endcode
 */
template <typename T>
inline void do_not_optimize(const T& value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    __asm__ __volatile__("" : : "g"(&value) : "memory");
#elif defined(_MSC_VER)
    (void)&value;
    _ReadWriteBarrier();
#else
    (void)&value;
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Create a compiler memory barrier between benchmark operations.
 *
 * @return Nothing.
 * @pre None.
 * @post The compiler is prevented from freely moving memory operations across
 * the barrier.
 * @invariant This is not a hardware synchronization primitive.
 * @throws Nothing.
 * @note Ownership/thread-safety: owns no state and communicates no data.
 */
inline void clobber_memory() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

namespace detail {

/**
 * @brief Clamp a repetition or operation count to at least one.
 *
 * @param value Caller-supplied count.
 * @return `value` when nonzero; otherwise `1`.
 * @pre None.
 * @post No state is modified.
 * @invariant Smoke tests can set low counts without risking division by zero,
 * while production benchmarks still express their configured counts directly.
 * @throws Nothing.
 * @note Ownership/thread-safety: operates only on a value parameter.
 */
inline std::size_t nonzero(std::size_t value) noexcept {
    return value == 0u ? 1u : value;
}

/**
 * @brief Return the upper median sample from a copied sample vector.
 *
 * @param values Per-operation timing samples to sort by value.
 * @return Median sample, or `0.0` when `values` is empty.
 * @pre Values should be finite nonnegative timing samples for meaningful
 * benchmark reports.
 * @post The caller's original vector is unchanged because samples are accepted
 * by value.
 * @invariant Median selection dampens outliers while keeping the helper simple
 * enough for F4 reviewable <=5 percent checks.
 * @throws `std::bad_alloc` may occur before entry while copying `values`;
 * sorting ordinary `double` values does not allocate.
 * @note Ownership/thread-safety: owns the copied vector and no shared state.
 */
inline double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2u];
}

} // namespace detail

/**
 * @brief Measure one operation with chrono warm-up and repeated samples.
 *
 * @tparam Operation Callable compatible with `void()`.
 * @param name Stable benchmark name.
 * @param config Warm-up, repetition, and operation-count controls.
 * @param operation Operation to execute repeatedly.
 * @return Timing result with per-operation samples and median nanoseconds.
 *
 * @pre `operation` should perform one logical operation and should use
 * `do_not_optimize` or externally visible state when necessary to avoid being
 * optimized away.
 * @post The operation has been executed for all warm-up and measured
 * repetitions. Zero repetition or operation counts are treated as one to avoid
 * division by zero in smoke tests.
 * @invariant Samples are measured with `std::chrono::steady_clock`.
 * @throws Exceptions thrown by `operation` propagate to the caller.
 * Allocation for sample storage or the name can throw `std::bad_alloc`.
 * @note Ownership/thread-safety: owns local timing data and invokes
 * `operation` on the calling thread.
 *
 * Example:
 * @code
 * auto result = memsafe::test_support::chrono_bench::measure(
 *     "increment", {}, [&] { ++counter; });
 * @endcode
 */
template <typename Operation>
bench_result measure(const char* name,
                     const bench_config& config,
                     Operation&& operation) {
    const std::size_t warmups = detail::nonzero(config.warmup_repetitions);
    const std::size_t repetitions = detail::nonzero(config.measured_repetitions);
    const std::size_t operations = detail::nonzero(config.operations_per_repetition);

    for (std::size_t repetition = 0; repetition < warmups; ++repetition) {
        for (std::size_t op = 0; op < operations; ++op) {
            operation();
        }
        clobber_memory();
    }

    bench_result result;
    result.name = name == nullptr ? "" : name;
    result.config = config;
    result.samples_ns_per_op.reserve(repetitions);

    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
        const std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        for (std::size_t op = 0; op < operations; ++op) {
            operation();
        }
        const std::chrono::steady_clock::time_point end =
            std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::nano> elapsed = end - start;
        result.samples_ns_per_op.push_back(elapsed.count() /
                                           static_cast<double>(operations));
        clobber_memory();
    }

    result.median_ns_per_operation = detail::median(result.samples_ns_per_op);
    return result;
}

/**
 * @brief Compare a candidate operation against a baseline operation.
 *
 * @tparam BaselineOperation Callable compatible with `void()`.
 * @tparam CandidateOperation Callable compatible with `void()`.
 * @param name Stable comparison name.
 * @param config Shared measurement and ratio-budget configuration.
 * @param baseline Baseline operation to measure first.
 * @param candidate Candidate operation to measure second.
 * @return Comparison containing both measurements and candidate/baseline ratio.
 *
 * @pre Baseline and candidate operations should have comparable logical work
 * units and should use identical `operations_per_repetition`.
 * @post Both operations have been warmed up and measured according to
 * `config`.
 * @invariant A zero or negative baseline timing yields infinity rather than a
 * false passing ratio.
 * @throws Exceptions thrown by either operation propagate. Allocation can throw
 * `std::bad_alloc`.
 * @note Ownership/thread-safety: invokes both operations on the calling thread.
 */
template <typename BaselineOperation, typename CandidateOperation>
comparison_result compare(const char* name,
                          const bench_config& config,
                          BaselineOperation&& baseline,
                          CandidateOperation&& candidate) {
    comparison_result comparison;
    comparison.name = name == nullptr ? "" : name;
    comparison.baseline = measure("baseline", config,
                                  std::forward<BaselineOperation>(baseline));
    comparison.candidate = measure("candidate", config,
                                   std::forward<CandidateOperation>(candidate));
    comparison.max_ratio = config.max_ratio;
    if (comparison.baseline.median_ns_per_operation <= 0.0) {
        comparison.ratio = std::numeric_limits<double>::infinity();
    } else {
        comparison.ratio = comparison.candidate.median_ns_per_operation /
                           comparison.baseline.median_ns_per_operation;
    }
    return comparison;
}

/**
 * @brief Compute the geometric mean of positive slowdown ratios.
 *
 * @param ratios Ratios to aggregate.
 * @return Geometric mean, `1.0` for an empty vector, or infinity if any ratio
 * is nonpositive.
 * @pre Ratios should be finite and positive for meaningful benchmark reports.
 * @post No input values are modified.
 * @invariant Log-space accumulation is used to avoid unnecessary overflow for
 * ordinary microbenchmark ratios.
 * @throws Nothing for normal floating-point operations.
 * @note Ownership/thread-safety: borrows the vector for the duration of the
 * call and owns no state.
 *
 * Example:
 * @code
 * const double gm =
 *     memsafe::test_support::chrono_bench::geometric_mean({1.01, 1.04});
 * @endcode
 */
inline double geometric_mean(const std::vector<double>& ratios) noexcept {
    if (ratios.empty()) {
        return 1.0;
    }
    double log_sum = 0.0;
    for (std::vector<double>::const_iterator it = ratios.begin();
         it != ratios.end(); ++it) {
        if (*it <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        log_sum += std::log(*it);
    }
    return std::exp(log_sum / static_cast<double>(ratios.size()));
}

/**
 * @brief Summarize per-operation comparisons into a suite-level verdict.
 *
 * @param comparisons Comparison results to include.
 * @param max_ratio Maximum accepted geometric mean ratio.
 * @return Suite result with copied comparisons and geometric mean ratio.
 * @pre Each comparison ratio should be finite and positive.
 * @post The returned suite owns a copy of `comparisons`.
 * @invariant The suite passes only when every comparison and the geometric mean
 * are within budget.
 * @throws `std::bad_alloc` if copying comparisons or collecting ratios fails.
 * @note Thread-safety: uses only local state.
 */
inline suite_result summarize(const std::vector<comparison_result>& comparisons,
                              double max_ratio = 1.05) {
    std::vector<double> ratios;
    ratios.reserve(comparisons.size());
    for (std::vector<comparison_result>::const_iterator it = comparisons.begin();
         it != comparisons.end(); ++it) {
        ratios.push_back(it->ratio);
    }

    suite_result suite;
    suite.comparisons = comparisons;
    suite.geometric_mean_ratio = geometric_mean(ratios);
    suite.max_ratio = max_ratio;
    return suite;
}

} // namespace chrono_bench
} // namespace test_support
} // namespace memsafe

#endif /* MEMSAFE_TEST_SUPPORT_CHRONO_BENCH_HPP */
