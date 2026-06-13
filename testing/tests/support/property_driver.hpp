/**
 * @file property_driver.hpp
 * @brief Deterministic, dependency-free property-test driver utilities.
 *
 * @details
 * Work package: CPP_MEMSAFE-0035-TEST.
 *
 * Purpose:
 * - Provide F2 property-test sequence thresholds without requiring RapidCheck
 *   or any external link step in the F4 harness.
 * - Derive deterministic seeds from stable test names so failing sequences can
 *   be replayed from normal standalone executables.
 * - Carry minimal shrinking metadata: failing sequence index, seed, original
 *   operation count, and smallest known prefix.
 *
 * Key invariants:
 * - Non-stateful workloads default to at least 10 000 generated sequences.
 * - Stateful workloads default to at least 1 000 generated sequences.
 * - Defining a nightly property macro multiplies both thresholds by at least
 *   10, matching F2's nightly multiplier requirement.
 * - The driver is header-only and owns no global mutable state.
 *
 * Ownership and thread-safety:
 * - Each run owns its local PRNG and metadata objects.
 * - Distinct `run_sequences` calls may execute concurrently when their property
 *   bodies do not share mutable state.
 */
#ifndef MEMSAFE_TEST_SUPPORT_PROPERTY_DRIVER_HPP
#define MEMSAFE_TEST_SUPPORT_PROPERTY_DRIVER_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <random>
#include <string>
#include <utility>

/**
 * @def MEMSAFE_PROPERTY_NIGHTLY
 * @brief Request the F2 10x nightly property-test multiplier.
 *
 * @retval defined Both stateful and non-stateful property thresholds are
 * multiplied by 10.
 * @pre Define the macro in the property lane before including this header when
 * a nightly run is desired.
 * @post `memsafe::test_support::property::active_multiplier()` returns at
 * least 10.
 * @invariant The macro is presence-based; its replacement value is ignored.
 * @throws Nothing; the macro is consumed by preprocessing only.
 * @note Ownership/thread-safety: the macro owns no runtime state.
 *
 * Example:
 * @code
 * #define MEMSAFE_PROPERTY_NIGHTLY 1
 * #include "support/property_driver.hpp"
 * @endcode
 */

/**
 * @def MEMSAFE_NIGHTLY_PROPERTY_TESTS
 * @brief Alternate lane macro accepted as a nightly property-test selector.
 *
 * @retval defined Both stateful and non-stateful property thresholds are
 * multiplied by 10.
 * @pre Define before including this header.
 * @post `active_multiplier()` returns at least 10.
 * @invariant This alias is equivalent to `MEMSAFE_PROPERTY_NIGHTLY`.
 * @throws Nothing; preprocessing only.
 * @note Ownership/thread-safety: the macro owns no runtime state.
 */

/**
 * @def MEMSAFE_ENABLE_NIGHTLY_PROPERTY_TESTS
 * @brief Verbose alternate lane macro accepted as a nightly property selector.
 *
 * @retval defined Both F2 property thresholds use the 10x nightly multiplier.
 * @pre Define before including this header.
 * @post `active_multiplier()` returns at least 10.
 * @invariant This alias is equivalent to `MEMSAFE_PROPERTY_NIGHTLY`.
 * @throws Nothing; preprocessing only.
 * @note Ownership/thread-safety: the macro owns no runtime state.
 */

/**
 * @def MEMSAFE_NIGHTLY
 * @brief Broad nightly-build macro accepted by the property driver.
 *
 * @retval defined Both F2 property thresholds use the 10x nightly multiplier.
 * @pre Define before including this header only for lanes that should expand
 * test budgets.
 * @post `active_multiplier()` returns at least 10.
 * @invariant This broad selector never lowers an explicitly configured numeric
 * multiplier.
 * @throws Nothing; preprocessing only.
 * @note Ownership/thread-safety: the macro owns no runtime state.
 */

/**
 * @def MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER
 * @brief Numeric nightly property multiplier override.
 *
 * @return Integer preprocessor expression used as the multiplier when it is
 * greater than 10; values below 10 are clamped to 10.
 * @pre Define before including this header. The replacement must be usable as a
 * positive integer expression.
 * @post `active_multiplier()` returns `max(10, value)`.
 * @invariant A numeric override cannot reduce the F2-required nightly budget.
 * @throws Nothing directly; non-integer replacement text is a compile-time
 * error in `active_multiplier()`.
 * @note Ownership/thread-safety: the macro owns no runtime state.
 *
 * Example:
 * @code
 * #define MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER 20
 * #include "support/property_driver.hpp"
 * @endcode
 */

namespace memsafe {
namespace test_support {
namespace property {

/**
 * @brief Identify which F2 property threshold applies to a test target.
 *
 * @pre Use `non_stateful` for value-like ownership or handle types and
 * `stateful` for command-sequence models such as slot maps, scopes, mutexes,
 * and reference-counted objects.
 * @post No state is modified.
 * @invariant The enum maps directly to the F2 10 000 and 1 000 sequence-count
 * floors.
 * @throws Nothing; enum operations are trivial.
 * @note Thread-safety: enum values are immutable.
 */
enum class workload {
    /// F2 non-stateful threshold: at least 10 000 sequences per normal run.
    non_stateful,
    /// F2 stateful threshold: at least 1 000 sequences per normal run.
    stateful
};

/**
 * @brief Minimal shrinking data carried from a failing property sequence.
 *
 * @details
 * The driver does not implement RapidCheck's full shrink tree. Instead, it
 * records enough metadata for F4-compatible replay tests to report the seed and
 * the smallest prefix known to reproduce the failure. Property bodies can call
 * `record_shrink` when they discover a smaller prefix.
 *
 * @pre Initialize with defaults or let `run_sequences` populate the seed and
 * failing index before calling the property body.
 * @post The object owns its note string and no external resources.
 * @invariant When `has_failure` is false, the prefix fields are advisory only.
 * When true, `minimal_steps <= original_steps` unless a caller deliberately
 * records invalid metadata for its own diagnostics.
 * @throws Copying or assigning can throw `std::bad_alloc` because `note` owns
 * memory.
 * @note Thread-safety: independent metadata objects may be used concurrently.
 *
 * Example:
 * @code
 * memsafe::test_support::property::shrinking_metadata shrink;
 * memsafe::test_support::property::record_shrink(
 *     shrink, 12, 4, 1234u, 7u, "shortest stale-handle prefix");
 * @endcode
 */
struct shrinking_metadata {
    /// True when a failing sequence has been observed.
    bool has_failure = false;
    /// Stable seed used to initialize the failing sequence PRNG.
    std::uint64_t seed = 0;
    /// Zero-based sequence index within the property run.
    std::size_t failing_sequence = 0;
    /// Original operation count or prefix length that failed.
    std::size_t original_steps = 0;
    /// Smallest known operation prefix that still fails.
    std::size_t minimal_steps = 0;
    /// Human-readable shrink note supplied by the property body.
    std::string note;
};

/**
 * @brief Configuration for a deterministic property run.
 *
 * @pre `sequence_count` should be at least `sequence_threshold(kind)` for
 * production property tests; smoke tests may lower it explicitly to keep the
 * baseline fast.
 * @post The configuration owns its name string and no external resources.
 * @invariant `kind` selects the threshold family and `seed` is stable for a
 * given test name and salt.
 * @throws Copying or assigning can throw `std::bad_alloc` because `name` owns
 * memory.
 * @note Thread-safety: immutable configurations can be shared across threads.
 */
struct run_config {
    /// Stable display name for the property target.
    std::string name;
    /// F2 threshold category.
    workload kind = workload::non_stateful;
    /// Number of generated sequences to execute.
    std::size_t sequence_count = 0;
    /// Base seed mixed with each sequence index.
    std::uint64_t seed = 0;
};

/**
 * @brief Result from `run_sequences`.
 *
 * @pre Obtain from `run_sequences` or construct directly in helper tests.
 * @post The result owns its diagnostic strings and no external resources.
 * @invariant `passed()` is true exactly when `failure.has_failure` is false.
 * @throws Copying or assigning can throw `std::bad_alloc`.
 * @note Thread-safety: independent result objects may be read concurrently.
 */
struct run_result {
    /// Configuration used for the run.
    run_config config;
    /// Count of sequences actually executed.
    std::size_t executed_sequences = 0;
    /// Failure and shrink metadata, if a sequence failed.
    shrinking_metadata failure;
    /// Human-readable failure detail.
    std::string message;

    /**
     * @brief Report whether every generated sequence satisfied the property.
     *
     * @return `true` when no failure metadata was recorded; `false` otherwise.
     * @pre The result must have been produced by a completed or failed property
     * run.
     * @post The result is not modified.
     * @invariant Passing results have `failure.has_failure == false`.
     * @throws Nothing.
     * @note Ownership/thread-safety: reads immutable fields only.
     */
    bool passed() const noexcept {
        return !failure.has_failure;
    }
};

namespace detail {

/**
 * @brief Apply one FNV-1a byte-mixing step.
 *
 * @param hash Current 64-bit hash state.
 * @param value Byte value to fold into the hash.
 * @return Updated hash state.
 * @pre `hash` may be any 64-bit value.
 * @post No global state is read or modified.
 * @invariant The constants are fixed so `stable_seed` remains deterministic
 * across platforms and future F4 runners.
 * @throws Nothing.
 * @note Ownership/thread-safety: operates only on value parameters.
 */
inline std::uint64_t fnv1a_mix(std::uint64_t hash, unsigned char value) noexcept {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ull;
    return hash;
}

/**
 * @brief Diffuse a seed with an additional 64-bit value.
 *
 * @param seed Base deterministic seed.
 * @param value Salt or sequence index to mix into the base seed.
 * @return Nonzero mixed seed suitable for `std::mt19937_64`.
 * @pre None.
 * @post No global state is read or modified.
 * @invariant A zero intermediate is remapped to a fixed nonzero constant so
 * generated property sequences never rely on a zero seed sentinel.
 * @throws Nothing.
 * @note Ownership/thread-safety: operates only on value parameters.
 */
inline std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t value) noexcept {
    std::uint64_t x = seed + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    x ^= x >> 30u;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27u;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31u;
    return x == 0u ? 0x6a09e667f3bcc909ull : x;
}

} // namespace detail

/**
 * @brief Return the normal-run non-stateful F2 sequence threshold.
 *
 * @return `10000`.
 * @pre None.
 * @post No state is modified.
 * @invariant The value is the F2 floor before nightly multiplication.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns a constant and owns no state.
 */
inline std::size_t non_stateful_base_threshold() noexcept {
    return 10000u;
}

/**
 * @brief Return the normal-run stateful F2 sequence threshold.
 *
 * @return `1000`.
 * @pre None.
 * @post No state is modified.
 * @invariant The value is the F2 floor before nightly multiplication.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns a constant and owns no state.
 */
inline std::size_t stateful_base_threshold() noexcept {
    return 1000u;
}

/**
 * @brief Return the active property-test multiplier for this translation unit.
 *
 * @return `1` for normal lanes; at least `10` when a supported nightly macro is
 * defined.
 * @pre Define any multiplier macro before including this header.
 * @post No runtime state is modified.
 * @invariant Nightly selection cannot reduce the threshold below the F2 10x
 * requirement.
 * @throws Nothing unless `MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER` expands to an
 * invalid integer expression, which is a compile-time error.
 * @note Ownership/thread-safety: compile-time configuration only.
 */
inline std::size_t active_multiplier() noexcept {
#if defined(MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER)
    const std::size_t configured =
        static_cast<std::size_t>(MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER);
    return configured < 10u ? 10u : configured;
#elif defined(MEMSAFE_PROPERTY_NIGHTLY) || defined(MEMSAFE_NIGHTLY_PROPERTY_TESTS) || \
      defined(MEMSAFE_ENABLE_NIGHTLY_PROPERTY_TESTS) || defined(MEMSAFE_NIGHTLY)
    return 10u;
#else
    return 1u;
#endif
}

/**
 * @brief Return the active sequence threshold for a workload category.
 *
 * @param kind F2 workload category.
 * @return `10000 * active_multiplier()` for `non_stateful`, or
 * `1000 * active_multiplier()` for `stateful`.
 * @pre None.
 * @post No state is modified.
 * @invariant Returned thresholds are never below F2's normal-run floors.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns a computed constant and owns no state.
 */
inline std::size_t sequence_threshold(workload kind) noexcept {
    const std::size_t base = kind == workload::stateful
                                 ? stateful_base_threshold()
                                 : non_stateful_base_threshold();
    return base * active_multiplier();
}

/**
 * @brief Derive a deterministic 64-bit seed from a test name and salt.
 *
 * @param name Stable property name. Null is treated as the empty string.
 * @param salt Optional caller-supplied salt for splitting related properties.
 * @return Nonzero deterministic seed value.
 * @pre Use stable names rather than source-line numbers when replay stability
 * matters.
 * @post No global state is read or modified.
 * @invariant Equal `(name, salt)` inputs produce equal seeds across platforms.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows `name` for the duration of the call.
 *
 * Example:
 * @code
 * const auto seed =
 *     memsafe::test_support::property::stable_seed("owner.borrow", 1u);
 * @endcode
 */
inline std::uint64_t stable_seed(const char* name, std::uint64_t salt = 0u) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const char* cursor = name == nullptr ? "" : name;
    while (*cursor != '\0') {
        hash = detail::fnv1a_mix(hash, static_cast<unsigned char>(*cursor));
        ++cursor;
    }
    hash = detail::mix_seed(hash, salt);
    return hash == 0u ? 0xcbf29ce484222325ull : hash;
}

/**
 * @brief Build a default run configuration for a property target.
 *
 * @param name Stable property name used for display and seed derivation.
 * @param kind F2 workload category.
 * @param salt Optional seed salt for related properties.
 * @return Configuration with the active F2 threshold and deterministic seed.
 * @pre Define nightly multiplier macros before including this header if the
 * returned sequence count should reflect nightly budgets.
 * @post The returned configuration owns a copy of `name`.
 * @invariant `sequence_count == sequence_threshold(kind)`.
 * @throws `std::bad_alloc` if copying `name` fails.
 * @note Thread-safety: the function uses only local state.
 */
inline run_config make_config(const char* name,
                              workload kind,
                              std::uint64_t salt = 0u) {
    run_config config;
    config.name = name == nullptr ? "" : name;
    config.kind = kind;
    config.sequence_count = sequence_threshold(kind);
    config.seed = stable_seed(name, salt);
    return config;
}

/**
 * @brief Record the smallest known failing prefix for a property sequence.
 *
 * @param metadata Metadata object to update.
 * @param original_steps Original operation count that failed.
 * @param minimal_steps Smallest known failing prefix length.
 * @param seed Seed for the failing sequence.
 * @param failing_sequence Zero-based failing sequence index.
 * @param note Human-readable shrink note. Null is treated as empty.
 * @pre Call from a property body after discovering a reproducible failure.
 * @post `metadata.has_failure` is true and all supplied fields are stored.
 * @invariant The helper does not enforce `minimal_steps <= original_steps` so
 * tests can report intentionally unusual shrink diagnostics.
 * @throws `std::bad_alloc` if copying `note` fails.
 * @note Ownership/thread-safety: `metadata` is caller-owned and must not be
 * mutated concurrently.
 */
inline void record_shrink(shrinking_metadata& metadata,
                          std::size_t original_steps,
                          std::size_t minimal_steps,
                          std::uint64_t seed,
                          std::size_t failing_sequence,
                          const char* note) {
    metadata.has_failure = true;
    metadata.original_steps = original_steps;
    metadata.minimal_steps = minimal_steps;
    metadata.seed = seed;
    metadata.failing_sequence = failing_sequence;
    metadata.note = note == nullptr ? "" : note;
}

/**
 * @brief Execute deterministic generated sequences against a property body.
 *
 * @tparam PropertyBody Callable compatible with
 * `bool(std::size_t, std::mt19937_64&, shrinking_metadata&)`.
 * @param config Run configuration, including sequence count and base seed.
 * @param body Property body. Return `true` for pass and `false` for failure.
 * @return Result containing execution count and shrink metadata on failure.
 *
 * @pre `config.sequence_count` must be greater than zero for production
 * property runs. `body` must not retain references to the per-sequence PRNG or
 * metadata after it returns.
 * @post Each executed sequence receives a deterministic PRNG seeded from
 * `config.seed` and its sequence index. Execution stops at the first false
 * result or escaping exception.
 * @invariant The same config and body behavior produce the same failing
 * sequence index and seed across platforms.
 * @throws `std::bad_alloc` while constructing diagnostic strings. Exceptions
 * thrown by `body` are caught and converted into a failed `run_result`.
 * @note Ownership/thread-safety: the driver owns local PRNG instances and
 * passes caller-owned behavior by reference only for the duration of the call.
 *
 * Example:
 * @code
 * auto config = memsafe::test_support::property::make_config(
 *     "handle.roundtrip",
 *     memsafe::test_support::property::workload::non_stateful);
 * auto result = memsafe::test_support::property::run_sequences(
 *     config,
 *     [](std::size_t, std::mt19937_64& rng, auto&) {
 *         return rng() != 0u;
 *     });
 * CHECK(result.passed());
 * @endcode
 */
template <typename PropertyBody>
run_result run_sequences(const run_config& config, PropertyBody&& body) {
    run_result result;
    result.config = config;

    for (std::size_t sequence = 0; sequence < config.sequence_count; ++sequence) {
        const std::uint64_t sequence_seed = detail::mix_seed(config.seed, sequence);
        std::mt19937_64 rng(sequence_seed);
        shrinking_metadata metadata;
        metadata.seed = sequence_seed;
        metadata.failing_sequence = sequence;

        bool passed = false;
        try {
            passed = static_cast<bool>(body(sequence, rng, metadata));
        } catch (const std::exception& ex) {
            record_shrink(metadata, sequence + 1u, sequence + 1u,
                          sequence_seed, sequence, ex.what());
            result.failure = metadata;
            result.executed_sequences = sequence + 1u;
            result.message = "property body threw std::exception";
            return result;
        } catch (...) {
            record_shrink(metadata, sequence + 1u, sequence + 1u,
                          sequence_seed, sequence, "property body threw");
            result.failure = metadata;
            result.executed_sequences = sequence + 1u;
            result.message = "property body threw non-standard exception";
            return result;
        }

        if (!passed) {
            if (!metadata.has_failure) {
                record_shrink(metadata, sequence + 1u, sequence + 1u,
                              sequence_seed, sequence, "property returned false");
            }
            result.failure = metadata;
            result.executed_sequences = sequence + 1u;
            result.message = "property returned false";
            return result;
        }
    }

    result.executed_sequences = config.sequence_count;
    return result;
}

} // namespace property
} // namespace test_support
} // namespace memsafe

#endif /* MEMSAFE_TEST_SUPPORT_PROPERTY_DRIVER_HPP */
