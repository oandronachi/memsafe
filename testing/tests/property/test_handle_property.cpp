/**
 * @file test_handle_property.cpp
 * @brief Non-stateful property test for Slice 2 `memsafe::Handle<T>` values.
 *
 * @details
 * Work package: CPP_MEMSAFE-0225-TEST.
 *
 * Purpose:
 * - Exercise `memsafe::Handle<T>` as the F2 non-stateful property target that
 *   was not covered by the lower-count stateful `SlotMap<T>` property test.
 * - Generate at least 10 000 random `(index, generation)` handle component
 *   pairs per normal property run through CPP_MEMSAFE-0035-TEST
 *   `property_driver.hpp`, with the driver's 10x nightly multiplier applied
 *   when a supported nightly macro is defined.
 * - Validate only value-level `Handle<T>` invariants and deliberately avoid a
 *   live `SlotMap<T>`, because this work package depends on the Slice 2 handle
 *   primitives rather than the bounded allocator property model.
 * - Report the deterministic replay seed and a minimal failing invariant
 *   ordinal plus the generated counterexample components on failure.
 *
 * Key invariants:
 * - A default-constructed `Handle<T>` is never intrinsically valid.
 * - Component construction round-trips the exact index and generation through
 *   `index()` and `generation()`.
 * - Copied handles compare equal to their source and remain independently
 *   observable after the copy.
 * - Handles with distinct `(index, generation)` component pairs never compare
 *   equal.
 * - A default handle never compares equal to a non-sentinel, allocated-style
 *   handle value.
 *
 * Ownership and thread-safety:
 * - All generated state is owned by one test process and one test thread.
 * - Handles are copied only as ordinary values; no slot storage, allocator, or
 *   synchronization object is constructed by this property test.
 */
#include "../../test_harness.hpp"

#include "../support/property_driver.hpp"

#include <memsafe/handle.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>

namespace {

/**
 * @brief Payload tag used only to instantiate `memsafe::Handle<T>`.
 *
 * @pre None; the test never creates an object of this type.
 * @post No runtime state is modified.
 * @invariant The type parameter is a phantom for `Handle<T>`, so this complete
 * payload tag lets the property instantiate the handle without introducing
 * slot-map ownership.
 * @throws Nothing; the type has no data members or custom special members.
 * @note Ownership/thread-safety: no instances are required, and the type owns
 * no shared state.
 */
struct handle_property_payload final {};

/// Handle specialization exercised by this non-stateful property target.
using handle_type = memsafe::Handle<handle_property_payload>;

/// Slot index component type stored by the handle under test.
using index_type = handle_type::index_type;

/// Generation component type stored by the handle under test.
using generation_type = handle_type::generation_type;

/// Count of ordered invariant probes used as a tiny shrink domain.
constexpr std::size_t k_invariant_probe_count = 5u;

/**
 * @brief Generated components used by one handle property sequence.
 *
 * @pre Values are produced by `draw_handle_case`.
 * @post The aggregate owns only scalar counterexample data.
 * @invariant `(distinct_index, distinct_generation)` differs from
 * `(index, generation)`, while `(allocated_index, allocated_generation)` uses
 * only non-sentinel allocated-style components.
 * @throws Nothing; the aggregate has only trivial scalar fields.
 * @note Ownership/thread-safety: one instance is stack-owned by one generated
 * property sequence.
 */
struct generated_handle_case final {
    /// Primary random slot index component.
    index_type index = 0u;
    /// Primary random generation component.
    generation_type generation = 0u;
    /// Secondary index component forced to make a distinct pair.
    index_type distinct_index = 0u;
    /// Secondary generation component forced to make a distinct pair.
    generation_type distinct_generation = 0u;
    /// Non-sentinel index used for default-vs-allocated equality checks.
    index_type allocated_index = 0u;
    /// Nonzero generation used for default-vs-allocated equality checks.
    generation_type allocated_generation = 1u;
};

/**
 * @brief Result of checking one generated handle component pair.
 *
 * @pre Default construction represents a passing sequence before any invariant
 * has been evaluated.
 * @post When `ok == false`, `failing_probe` names the first failing invariant
 * in the fixed probe order and `counterexample` contains the generated values
 * used for replay diagnostics.
 * @invariant Failing reports preserve the earliest invariant violation so the
 * shrink metadata remains deterministic.
 * @throws Nothing; the aggregate owns no dynamic storage.
 * @note Ownership/thread-safety: owned by one property sequence.
 */
struct sequence_report final {
    /// True when every checked `Handle<T>` invariant held.
    bool ok = true;
    /// One-based invariant probe that first failed, or zero on pass.
    std::size_t failing_probe = 0u;
    /// Static diagnostic string naming the violated invariant.
    const char* reason = "no failure";
    /// Generated values that exposed the failure.
    generated_handle_case counterexample{};
};

/**
 * @brief Convert the next PRNG output to a handle index component.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @return A deterministic `index_type` value drawn from the engine stream.
 * @pre `rng` is alive and owned by the active property sequence.
 * @post `rng` has advanced by one draw.
 * @invariant Direct truncation keeps replay based only on the standardized
 * `std::mt19937_64` output stream, not on implementation-defined
 * distribution state.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the caller-owned engine for the
 * duration of the call.
 */
index_type draw_index(std::mt19937_64& rng) noexcept {
    return static_cast<index_type>(rng());
}

/**
 * @brief Convert the next PRNG output to a handle generation component.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @return A deterministic `generation_type` value drawn from the engine stream.
 * @pre `rng` is alive and owned by the active property sequence.
 * @post `rng` has advanced by one draw.
 * @invariant Direct truncation exercises the complete 32-bit component domain
 * expected by the Slice 2 handle representation.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the caller-owned engine for the
 * duration of the call.
 */
generation_type draw_generation(std::mt19937_64& rng) noexcept {
    return static_cast<generation_type>(rng());
}

/**
 * @brief Normalize a random index into the allocated-style component domain.
 *
 * @param index Random index value.
 * @return `index` unless it is the reserved sentinel; zero otherwise.
 * @pre None.
 * @post No state is modified.
 * @invariant The returned value is never `handle_type::invalid_index`, making
 * it suitable for testing a handle that looks like one minted for a real slot.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure scalar transformation.
 */
index_type allocated_index_from(index_type index) noexcept {
    return index == handle_type::invalid_index ? index_type{0u} : index;
}

/**
 * @brief Normalize a random generation into the allocated-style domain.
 *
 * @param generation Random generation value.
 * @return `generation` unless it is the reserved zero sentinel; one otherwise.
 * @pre None.
 * @post No state is modified.
 * @invariant The returned value is never `handle_type::invalid_generation`, so
 * the generated handle is intrinsically non-default.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure scalar transformation.
 */
generation_type allocated_generation_from(generation_type generation) noexcept {
    return generation == handle_type::invalid_generation
               ? generation_type{1u}
               : generation;
}

/**
 * @brief Generate one complete non-stateful handle property case.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @return Generated primary, distinct, and allocated-style component pairs.
 * @pre `rng` is alive and owned by the active property sequence.
 * @post `rng` has advanced by four draws.
 * @invariant The primary pair is unconstrained and may include default
 * sentinels. The allocated-style pair is non-sentinel. The distinct pair is
 * forced to differ from the primary pair even when the random draws collide.
 * @throws Nothing.
 * @note Ownership/thread-safety: all generated state is returned by value.
 *
 * Example:
 * @code
 * std::mt19937_64 rng(7u);
 * generated_handle_case c = draw_handle_case(rng);
 * memsafe::Handle<handle_property_payload> h(c.index, c.generation);
 * @endcode
 */
generated_handle_case draw_handle_case(std::mt19937_64& rng) noexcept {
    generated_handle_case generated;
    generated.index = draw_index(rng);
    generated.generation = draw_generation(rng);
    generated.distinct_index = draw_index(rng);
    generated.distinct_generation = draw_generation(rng);
    generated.allocated_index = allocated_index_from(generated.index);
    generated.allocated_generation =
        allocated_generation_from(generated.generation);

    if (generated.distinct_index == generated.index &&
        generated.distinct_generation == generated.generation) {
        /*
         * F2 requires distinct component pairs to compare unequal. Collisions
         * are rare but deterministic replay must not depend on probability, so
         * perturb one component in the unsigned domain and keep the pair
         * structurally distinct without consulting any SlotMap state.
         */
        generated.distinct_generation =
            static_cast<generation_type>(generated.distinct_generation + 1u);
    }

    return generated;
}

/**
 * @brief Mark a sequence report as failed if no earlier probe failed.
 *
 * @param report Report to update.
 * @param probe One-based invariant probe index.
 * @param reason Static diagnostic string naming the violated invariant.
 * @param counterexample Generated components associated with the failure.
 * @return No value.
 * @pre `reason != nullptr` and `probe > 0`.
 * @post The first failure is preserved and later failures do not overwrite it.
 * @invariant Earliest-failure reporting gives `property_driver.hpp` a stable
 * minimal shrink target for this non-stateful value test.
 * @throws Nothing.
 * @note Ownership/thread-safety: mutates caller-owned local sequence state.
 */
void fail_sequence(sequence_report& report,
                   std::size_t probe,
                   const char* reason,
                   const generated_handle_case& counterexample) noexcept {
    if (report.ok) {
        report.ok = false;
        report.failing_probe = probe;
        report.reason = reason;
        report.counterexample = counterexample;
    }
}

/**
 * @brief Verify a copied handle remains an independently observable value.
 *
 * @param original Source handle.
 * @param expected_index Index component expected from the copy.
 * @param expected_generation Generation component expected from the copy.
 * @retval true The copy compares equal to the source and preserves both
 * observer values.
 * @retval false The copy changed equality or either observed component.
 * @pre `original` is alive.
 * @post No state is modified.
 * @invariant Copying `Handle<T>` is structural and never links to slot-map
 * state, so the copy can be inspected independently of any allocator context.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads only local scalar handle values.
 */
bool copy_is_independently_usable(handle_type original,
                                  index_type expected_index,
                                  generation_type expected_generation) noexcept {
    const handle_type copy = original;
    return copy == original && copy.index() == expected_index &&
           copy.generation() == expected_generation &&
           copy.is_valid() == original.is_valid();
}

/**
 * @brief Check every non-stateful `Handle<T>` invariant for one generated case.
 *
 * @param generated Randomized handle component case.
 * @return Report describing success or the first failing invariant probe.
 * @pre `generated` was produced by `draw_handle_case`.
 * @post No global state is modified and no `SlotMap<T>` has been constructed.
 * @invariant The fixed probe order is the shrink order: default validity,
 * component round-trip, copy usability, distinct-pair comparison, and
 * default-vs-allocated comparison.
 * @throws Nothing.
 * @note Ownership/thread-safety: all handles are automatic values used on one
 * test thread.
 */
sequence_report check_handle_case(
    const generated_handle_case& generated) noexcept {
    sequence_report report;
    report.counterexample = generated;

    const handle_type default_handle;
    if (default_handle.is_valid()) {
        fail_sequence(report,
                      1u,
                      "default-constructed Handle was valid",
                      generated);
    }

    const handle_type constructed(generated.index, generated.generation);
    if (constructed.index() != generated.index ||
        constructed.generation() != generated.generation) {
        fail_sequence(report,
                      2u,
                      "Handle observers did not round-trip components",
                      generated);
    }

    if (!copy_is_independently_usable(constructed,
                                      generated.index,
                                      generated.generation)) {
        fail_sequence(report,
                      3u,
                      "copied Handle was not equal and independently usable",
                      generated);
    }

    const handle_type distinct(generated.distinct_index,
                               generated.distinct_generation);
    if (distinct == constructed || !(distinct != constructed)) {
        fail_sequence(report,
                      4u,
                      "distinct Handle component pairs compared equal",
                      generated);
    }

    const handle_type allocated_style(generated.allocated_index,
                                      generated.allocated_generation);
    if (!allocated_style.is_valid() || default_handle == allocated_style) {
        fail_sequence(report,
                      5u,
                      "default Handle equaled an allocated-style Handle",
                      generated);
    }

    return report;
}

/**
 * @brief Convert a scalar handle component to a decimal diagnostic string.
 *
 * @tparam Component Unsigned scalar component type.
 * @param value Component value to stringify.
 * @return Decimal representation of `value`.
 * @pre `Component` is convertible to `unsigned long long`.
 * @post The returned string owns its characters.
 * @invariant Diagnostics avoid implementation-defined signed formatting by
 * promoting every component to `unsigned long long`.
 * @throws `std::bad_alloc` if string allocation fails.
 * @note Ownership/thread-safety: all state is local to the returned string.
 */
template <typename Component>
std::string component_to_string(Component value) {
    return std::to_string(static_cast<unsigned long long>(value));
}

/**
 * @brief Build the shrink note recorded for a failing handle property case.
 *
 * @param report Failed sequence report.
 * @return Human-readable note containing the first failing invariant and
 * generated counterexample components.
 * @pre `report.ok == false`.
 * @post The returned string owns its diagnostic text.
 * @invariant The note contains enough data to reconstruct the minimal
 * non-stateful counterexample without a slot map: primary pair, distinct pair,
 * and allocated-style pair.
 * @throws `std::bad_alloc` if string allocation fails.
 * @note Ownership/thread-safety: all state is local to the returned string.
 */
std::string describe_failure(const sequence_report& report) {
    const generated_handle_case& c = report.counterexample;
    std::string note = report.reason;
    note += "; invariant_probe=";
    note += std::to_string(report.failing_probe);
    note += "; index=";
    note += component_to_string(c.index);
    note += "; generation=";
    note += component_to_string(c.generation);
    note += "; distinct_index=";
    note += component_to_string(c.distinct_index);
    note += "; distinct_generation=";
    note += component_to_string(c.distinct_generation);
    note += "; allocated_index=";
    note += component_to_string(c.allocated_index);
    note += "; allocated_generation=";
    note += component_to_string(c.allocated_generation);
    return note;
}

/**
 * @brief Execute one deterministic generated handle value sequence.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @return Report containing pass/fail status and counterexample metadata.
 * @pre `rng` is the per-sequence engine supplied by
 * `memsafe::test_support::property::run_sequences`, or another engine seeded
 * with a reported replay seed.
 * @post Exactly one random primary `(index, generation)` pair has been
 * generated and checked without constructing a live `SlotMap<T>`.
 * @invariant A property sequence is a single non-stateful handle value, which
 * makes the F2 sequence count equal to the number of random primary handle
 * values generated per run.
 * @throws Nothing.
 * @note Ownership/thread-safety: all state is local to the sequence.
 */
sequence_report run_handle_sequence(std::mt19937_64& rng) noexcept {
    return check_handle_case(draw_handle_case(rng));
}

/**
 * @brief Print property-driver failure metadata in replay form.
 *
 * @param result Failed property result returned by `run_sequences`.
 * @return No value.
 * @pre `result.passed() == false`.
 * @post A single diagnostic line containing seed and minimal counterexample
 * metadata has been written to standard error.
 * @invariant The emitted seed is the per-sequence replay seed stored in
 * `result.failure.seed`; the shrunk prefix is the earliest failing invariant
 * probe recorded in `result.failure.minimal_steps`.
 * @throws Nothing.
 * @note Ownership/thread-safety: writes to process-local `stderr`; no shared
 * library state is modified.
 */
void print_property_failure(
    const memsafe::test_support::property::run_result& result) noexcept {
    std::fprintf(stderr,
                 "property failure: name=%s seed=%llu sequence=%zu "
                 "original_steps=%zu shrunk_prefix=%zu detail=%s message=%s\n",
                 result.config.name.c_str(),
                 static_cast<unsigned long long>(result.failure.seed),
                 result.failure.failing_sequence,
                 result.failure.original_steps,
                 result.failure.minimal_steps,
                 result.failure.note.c_str(),
                 result.message.c_str());
}

/**
 * @brief Check a completed handle property result against F2 thresholds.
 *
 * @param result Result returned by `property::run_sequences`.
 * @return No value.
 * @pre `result` was produced by this translation unit's non-stateful property
 * configuration.
 * @post Harness failures are recorded for property failures or insufficient
 * sequence budgets.
 * @invariant A passing run executes the property driver's active non-stateful
 * threshold, which is 10 000 normally and at least 100 000 when a supported
 * nightly multiplier macro is defined.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads immutable result data and records
 * process-local harness failures.
 */
void check_property_result(
    const memsafe::test_support::property::run_result& result) noexcept {
    namespace prop = memsafe::test_support::property;

    if (!result.passed()) {
        print_property_failure(result);
    }

    CHECK(result.config.sequence_count >= prop::non_stateful_base_threshold());
    CHECK(prop::sequence_threshold(prop::workload::non_stateful) >= 10000u);
    CHECK(result.passed());

    if (result.passed()) {
        CHECK(result.executed_sequences == result.config.sequence_count);
        CHECK(result.executed_sequences >= prop::non_stateful_base_threshold());
    }

#if defined(MEMSAFE_PROPERTY_NIGHTLY) || defined(MEMSAFE_NIGHTLY_PROPERTY_TESTS) || \
    defined(MEMSAFE_ENABLE_NIGHTLY_PROPERTY_TESTS) || defined(MEMSAFE_NIGHTLY) ||  \
    defined(MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER)
    CHECK(prop::active_multiplier() >= 10u);
    CHECK(result.config.sequence_count >=
          prop::non_stateful_base_threshold() * 10u);
#endif
}

} // namespace

/**
 * @brief Run the `memsafe::Handle<T>` non-stateful property test.
 *
 * @retval 0 All generated handle values satisfied the non-stateful invariants.
 * @retval 1 One or more harness checks failed, with replay seed and minimal
 * counterexample metadata reported for property failures.
 * @pre The executable is run by the F4 property lane, which discovers
 * `testing/tests/property/*.cpp` and defines any lane-specific nightly
 * property macros before including `property_driver.hpp`.
 * @post The process exit code is the test verdict consumed by CTest.
 * @invariant One named non-stateful property target executes for
 * `Handle<T>` and receives the property driver's full non-stateful threshold.
 * @throws The test does not intentionally throw. Unexpected allocation
 * failures while formatting a shrink note are caught by `property_driver.hpp`
 * and surfaced as property failures with seed metadata.
 * @note Ownership/thread-safety: all test state is automatic storage or
 * property-driver local state; no worker threads are started and no
 * `SlotMap<T>` is constructed.
 */
int main() {
    namespace prop = memsafe::test_support::property;

    prop::run_config config =
        prop::make_config("handle.components.value-invariants",
                          prop::workload::non_stateful,
                          0x0225u);

    const prop::run_result result = prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const sequence_report report = run_handle_sequence(rng);
            if (report.ok) {
                return true;
            }

            const std::string note = describe_failure(report);
            prop::record_shrink(shrink,
                                k_invariant_probe_count,
                                report.failing_probe,
                                shrink.seed,
                                sequence,
                                note.c_str());
            return false;
        });

    check_property_result(result);

    RUN_TESTS("test_handle_property");
}
