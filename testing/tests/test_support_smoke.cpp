/**
 * @file test_support_smoke.cpp
 * @brief Standalone smoke test for CPP_MEMSAFE-0035-TEST support headers.
 *
 * @details
 * Work package: CPP_MEMSAFE-0035-TEST.
 *
 * Purpose:
 * - Verify that the shared support headers build together in the F4 runtime
 *   test layout using only `test_harness.hpp` and the support headers.
 * - Exercise parent/child death-test respawn, deterministic property metadata,
 *   normal-main fuzz replay, and chrono ratio/geometric-mean helpers.
 *
 * Key invariants:
 * - The death-test child aborts or exits before normal smoke assertions run.
 * - The smoke test keeps generated property and benchmark counts small while
 *   separately asserting that production thresholds meet the F2 floors.
 * - Exit code 0 is the only pass signal consumed by the generic F4 CTest lane.
 *
 * Ownership and thread-safety:
 * - The test owns all local state and starts no worker threads.
 */
#include "../test_harness.hpp"

#include "support/chrono_bench.hpp"
#include "support/death_test.hpp"
#include "support/fuzz_replay.hpp"
#include "support/property_driver.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

/**
 * @brief Fuzz entry point used by the smoke test.
 *
 * @param data Byte buffer supplied by the replay adapter. The pointer may be
 * null when `size == 0`.
 * @param size Number of bytes available at `data`.
 * @return Zero when the byte sequence is accepted.
 * @pre `data` must be valid for `size` bytes when `size > 0`.
 * @post No global state is modified.
 * @invariant The entry point accepts every byte sequence so the smoke test
 * validates adapter plumbing rather than target-specific semantics.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the byte buffer only for the duration
 * of the call.
 */
int smoke_fuzz_entry(const std::uint8_t* data, std::size_t size) noexcept {
    if (size > 0u && data == nullptr) {
        return 1;
    }
    return 0;
}

} // namespace

/**
 * @brief Run the shared test-support smoke checks.
 *
 * @param argc Argument count supplied by the F4 test runner.
 * @param argv Argument vector supplied by the F4 test runner.
 * @return Zero when all checks pass; nonzero when any support helper violates
 * its smoke-test contract.
 * @pre The executable must be launched in a normal environment where `argv[0]`
 * can be used to re-spawn the current test for the death-test check.
 * @post The parent process prints the `test_harness.hpp` summary and exits with
 * the summary status. The death-test child does not return from the helper.
 * @invariant The test includes only the project test harness and the support
 * headers produced by CPP_MEMSAFE-0035-TEST.
 * @throws Nothing intentionally; unexpected exceptions are allowed to terminate
 * the test as a failure under the F4 exit-code model.
 * @note Ownership/thread-safety: all state is local to the process; no threads
 * are started.
 */
int main(int argc, char** argv) {
    namespace bench = memsafe::test_support::chrono_bench;
    namespace death = memsafe::test_support::death;
    namespace fuzz = memsafe::test_support::fuzz;
    namespace prop = memsafe::test_support::property;

    const death::death_result death_result =
        death::expect_abnormal_child(argc, argv, "smoke-abort", [] {
#if defined(_WIN32)
            /*
             * Windows debug CRT abort handling can be interactive on some
             * developer machines. The death helper treats nonzero child exit
             * as abnormal on Windows, matching how sanitizer and abort paths
             * are observed through `_spawnv(_P_WAIT, ...)`.
             */
            std::_Exit(3);
#else
            std::abort();
#endif
        });
    CHECK(death_result.passed());

    CHECK(prop::sequence_threshold(prop::workload::non_stateful) >= 10000u);
    CHECK(prop::sequence_threshold(prop::workload::stateful) >= 1000u);
    CHECK(prop::stable_seed("deterministic", 7u) ==
          prop::stable_seed("deterministic", 7u));
    CHECK(prop::stable_seed("deterministic", 7u) !=
          prop::stable_seed("deterministic", 8u));

    prop::run_config config =
        prop::make_config("support-smoke", prop::workload::stateful, 11u);
    config.sequence_count = 8u;
    const prop::run_result property_result = prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const std::uint64_t value = rng();
            if (sequence == 1000000u && value == 0u) {
                prop::record_shrink(shrink, 4u, 1u, shrink.seed, sequence,
                                    "unreachable smoke shrink");
                return false;
            }
            return true;
        });
    CHECK(property_result.passed());
    CHECK(property_result.executed_sequences == config.sequence_count);

    const std::vector<fuzz::byte_sequence> corpus = {
        fuzz::bytes_case("empty", {}),
        fuzz::ascii_case("letters", "abc")
    };
    CHECK(fuzz::replay(corpus, smoke_fuzz_entry) == 0);

    bench::bench_config bench_config;
    bench_config.warmup_repetitions = 1u;
    bench_config.measured_repetitions = 3u;
    bench_config.operations_per_repetition = 64u;
    int sink = 0;
    const bench::bench_result measured = bench::measure(
        "increment-smoke", bench_config, [&] {
            ++sink;
            bench::do_not_optimize(sink);
        });
    CHECK(measured.samples_ns_per_op.size() == 3u);
    CHECK(measured.median_ns_per_operation >= 0.0);

    const bench::comparison_result comparison = bench::compare(
        "same-operation", bench_config,
        [&] {
            ++sink;
            bench::do_not_optimize(sink);
        },
        [&] {
            ++sink;
            bench::do_not_optimize(sink);
        });
    CHECK(comparison.ratio > 0.0);

    bench::comparison_result fixed_ratio;
    fixed_ratio.name = "fixed-ratio";
    fixed_ratio.ratio = 1.04;
    fixed_ratio.max_ratio = 1.05;
    const bench::suite_result suite = bench::summarize({fixed_ratio}, 1.05);
    CHECK(suite.passed());
    CHECK(suite.geometric_mean_ratio <= 1.05);

    RUN_TESTS("test_support_smoke");
}
