/**
 * @file test_mutex_property.cpp
 * @brief Stateful property test for Slice 5 `memsafe::Mutex<T>` guards.
 *
 * @details
 * Work package: CPP_MEMSAFE-0535-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0500-FUNC `memsafe::Mutex<T>` artifact
 *   through generated single-threaded lock, mutate, and unlock command
 *   sequences.
 * - Use the CPP_MEMSAFE-0035-TEST `property_driver.hpp` stateful threshold so
 *   the property lane executes at least 1 000 sequences in normal CI and
 *   receives the documented 10x nightly multiplier automatically.
 * - Report the deterministic replay seed and the smallest command prefix known
 *   to reproduce any Mutex guard, value, or borrow-ledger invariant failure.
 *
 * Key invariants:
 * - A generated `MutRef<T>` is obtained only when the model says the mutex is
 *   unlocked. Generated lock commands while already locked are rejected by the
 *   sequential model instead of calling the blocking `lock()` API recursively.
 * - Every applied mutation occurs through a live `MutRef<int>` returned by
 *   `Mutex<int>::lock()`.
 * - The guarded integer value equals the model value after every applied
 *   mutation and after final cleanup.
 * - The checked borrow ledger reports a live borrow while a guard is held and
 *   returns to zero after each guard is dropped.
 *
 * Ownership and thread-safety:
 * - All generated state is owned by one test process and one test thread.
 * - Cross-thread lock blocking and interleavings remain the responsibility of
 *   the Slice 5 Relacy concurrency package; this file is the F2 sequential
 *   property model.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force checked Mutex borrow bookkeeping for this property oracle.
 *
 * @retval 1 Enables `Mutex<T>::has_borrow()` so the property can observe that
 * a mutex-backed `MutRef<T>` records one live borrow and clears it on drop.
 * @pre Define before including any memsafe feature header.
 * @post The generated lock/unlock model can assert borrow-ledger transitions
 * even if an outer build lane requested release no-checks.
 * @invariant CPP_MEMSAFE-0535-TEST requires the borrow counter to return to
 * zero after each guard is dropped, so this translation unit cannot inherit a
 * check-free release configuration.
 * @throws Nothing directly; this macro selects compile-time library code.
 * @note Ownership/thread-safety: the macro owns no runtime storage and affects
 * only this test translation unit.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception reporting for unexpected Mutex ledger violations.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs memsafe violation reports to throw
 * `memsafe::violation`.
 * @pre Define before including `<memsafe/sync.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post Unexpected library-managed borrow-ledger violations are catchable by
 * the property replay and can be reported with seed and prefix metadata.
 * @invariant Normal generated commands should not trigger memsafe violations;
 * any thrown violation is treated as a property failure.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: compile-time selection only. No handler slot
 * is installed by this test.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../../test_harness.hpp"

#include "../support/property_driver.hpp"

#include <memsafe/sync.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <string>

namespace {

/// Maximum generated command count for one stateful Mutex property sequence.
constexpr std::size_t k_max_commands_per_sequence = 64u;

/// Number of command variants in the generated Mutex state machine.
constexpr std::size_t k_mutex_command_count = 3u;

/// Number of signed mutation deltas in the range [-32, 32].
constexpr std::size_t k_delta_domain_size = 65u;

/// Bias used to center generated mutation deltas around zero.
constexpr int k_delta_bias = 32;

/// Sentinel prefix limit that means "execute the full generated sequence".
constexpr std::size_t k_full_sequence_prefix =
    (std::numeric_limits<std::size_t>::max)();

/**
 * @brief Return a deterministic bounded value from a Mersenne Twister engine.
 *
 * @param rng Random engine owned by the active property sequence.
 * @param exclusive_upper Exclusive upper bound for the returned value.
 * @return A value in the half-open range `[0, exclusive_upper)`.
 * @pre `exclusive_upper > 0`.
 * @post `rng` has advanced by one draw and no global state is modified.
 * @invariant Direct modulo reduction keeps replay based only on the
 * standardized `std::mt19937_64` output stream, not implementation-specific
 * distribution state.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the caller-owned engine for the
 * duration of the call. The small modulo bias is acceptable because the
 * property needs broad command coverage rather than statistical sampling
 * guarantees.
 */
std::size_t bounded_random(std::mt19937_64& rng,
                           std::size_t exclusive_upper) noexcept {
    const std::uint64_t raw = rng();
    const std::uint64_t bound = static_cast<std::uint64_t>(exclusive_upper);
    return static_cast<std::size_t>(raw % bound);
}

/**
 * @brief Draw one bounded signed mutation delta.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return Integer delta in the inclusive range `[-32, 32]`.
 * @pre `rng` is live and seeded for this property sequence.
 * @post `rng` has advanced by one draw.
 * @invariant The small range prevents signed overflow while still exercising
 * positive, negative, and zero mutations.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the caller-owned engine only for the
 * duration of the call.
 */
int draw_delta(std::mt19937_64& rng) noexcept {
    return static_cast<int>(bounded_random(rng, k_delta_domain_size)) -
           k_delta_bias;
}

/**
 * @brief Command kinds generated for the Mutex sequential state machine.
 *
 * @pre Values are produced only by `draw_mutex_command`.
 * @post Dispatching a command either mutates the guarded value, changes the
 * model lock state, or checks a documented no-op when the command precondition
 * is absent.
 * @invariant The command set is limited to lock, mutate, and unlock, which is
 * the CPP_MEMSAFE-0535-TEST Mutex acceptance surface.
 * @throws Nothing; enum values own no resources.
 * @note Ownership/thread-safety: values are local to one property sequence.
 */
enum class mutex_command {
    /// Obtain a `MutRef<int>` when the generated model is unlocked.
    lock,
    /// Mutate the guarded integer through a live `MutRef<int>`.
    mutate,
    /// Drop the live `MutRef<int>` and release the mutex when locked.
    unlock
};

/**
 * @brief Result of executing one generated Mutex command sequence.
 *
 * @pre Default construction represents a passing sequence before any command
 * has run.
 * @post Fields describe the first detected invariant failure, if any.
 * @invariant `ok == false` implies `reason` names the violated invariant and
 * `failing_step` identifies the one-based command prefix that exposed it.
 * @throws Nothing; the aggregate owns no dynamic storage.
 * @note Ownership/thread-safety: owned by one property sequence.
 */
struct sequence_report final {
    /// True when every checked Mutex invariant held.
    bool ok = true;
    /// Number of commands generated before prefix truncation.
    std::size_t generated_steps = 0u;
    /// Number of generated commands actually executed for this replay.
    std::size_t executed_steps = 0u;
    /// One-based command index that first exposed a failure, or zero on pass.
    std::size_t failing_step = 0u;
    /// Static diagnostic string naming the violated invariant.
    const char* reason = "no failure";
    /// Model value expected in the guarded integer at the point of failure.
    int expected_value = 0;
    /// Count of generated mutations actually applied through a live guard.
    std::size_t applied_mutations = 0u;
    /// Count of generated lock commands rejected while already locked.
    std::size_t rejected_lock_attempts = 0u;
};

/**
 * @brief Mutable model state for one Mutex property sequence replay.
 *
 * @pre Construct a fresh state for every replay seed and prefix.
 * @post `mutex` owns the guarded integer and `guard` owns any live lock.
 * @invariant `guard.has_value()` is the model's locked state. The guard field
 * is declared after the mutex so destruction releases the guard before the
 * mutex object is destroyed.
 * @throws `memsafe::Mutex<int>` construction may propagate `int` construction
 * exceptions, which do not occur for this zero initialization.
 * @note Ownership/thread-safety: all fields are local to one property replay
 * and are not shared across threads.
 */
struct mutex_sequence_state final {
    /**
     * @brief Construct a Mutex replay state with guarded value zero.
     *
     * @return Constructors do not return a value.
     * @pre None.
     * @post `mutex` guards integer zero and no `MutRef<int>` is live.
     * @invariant `expected_value` mirrors the guarded integer from
     * construction through every generated mutation.
     * @throws Any exception propagated by `memsafe::Mutex<int>` construction.
     * @note Ownership/thread-safety: constructed and used on one thread.
     */
    mutex_sequence_state() : mutex(0) {}

    /// Mutex under test, guarding the modeled integer.
    memsafe::Mutex<int> mutex;
    /// Optional live guard; engaged means the generated model is locked.
    std::optional<memsafe::MutRef<int>> guard;
    /// Integer value expected after every applied mutation.
    int expected_value = 0;
    /// Count of mutations applied through the live guard.
    std::size_t applied_mutations = 0u;
    /// Count of generated lock commands rejected while already locked.
    std::size_t rejected_lock_attempts = 0u;
};

/**
 * @brief Copy model counters from sequence state into a report.
 *
 * @param report Report receiving the current model snapshot.
 * @param state Current Mutex sequence model.
 * @return No value.
 * @pre `report` and `state` belong to the same replay.
 * @post Diagnostic model fields in `report` match `state`.
 * @invariant Failure notes can describe the expected value and mutation count
 * without retaining references to the state object.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads local sequence state only.
 */
void capture_model(sequence_report& report,
                   const mutex_sequence_state& state) noexcept {
    report.expected_value = state.expected_value;
    report.applied_mutations = state.applied_mutations;
    report.rejected_lock_attempts = state.rejected_lock_attempts;
}

/**
 * @brief Mark a sequence report as failed if no earlier failure exists.
 *
 * @param report Report to update.
 * @param state Current Mutex model snapshot for diagnostics.
 * @param step One-based command index associated with the failure.
 * @param reason Static diagnostic string naming the violated invariant.
 * @return No value.
 * @pre `reason != nullptr`.
 * @post The first failure is preserved and later failures do not overwrite it.
 * @invariant Earliest-failure reporting gives the shrink replay a stable
 * target.
 * @throws Nothing.
 * @note Ownership/thread-safety: mutates caller-owned local sequence state.
 */
void fail_sequence(sequence_report& report,
                   const mutex_sequence_state& state,
                   std::size_t step,
                   const char* reason) noexcept {
    if (report.ok) {
        report.ok = false;
        report.failing_step = step;
        report.reason = reason;
        capture_model(report, state);
    }
}

/**
 * @brief Draw one Mutex command from the deterministic command stream.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return Generated command kind.
 * @pre `rng` is live and seeded for this property sequence.
 * @post `rng` has advanced by one draw.
 * @invariant The three command variants are selected from one fixed domain so
 * replay does not depend on library distribution internals.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows the caller-owned PRNG only for the
 * duration of the call.
 */
mutex_command draw_mutex_command(std::mt19937_64& rng) noexcept {
    switch (bounded_random(rng, k_mutex_command_count)) {
    case 0u:
        return mutex_command::lock;
    case 1u:
        return mutex_command::mutate;
    default:
        return mutex_command::unlock;
    }
}

/**
 * @brief Validate invariants while the generated Mutex model is locked.
 *
 * @param state Current Mutex sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being checked.
 * @return True when the locked-state invariants hold.
 * @pre `state.guard.has_value()` is expected to be true.
 * @post `report` is failed if the guard, borrow ledger, or value disagrees
 * with the model.
 * @invariant While a guard is live, `Mutex<int>::has_borrow()` must be true and
 * dereferencing the guard must read the model value.
 * @throws Nothing for passing states; live `MutRef<int>` dereference should not
 * report null access.
 * @note Ownership/thread-safety: observes one live guard on one thread.
 */
bool check_locked_state(mutex_sequence_state& state,
                        sequence_report& report,
                        std::size_t step) noexcept {
    if (!state.guard.has_value()) {
        fail_sequence(report, state, step, "locked model had no live MutRef");
        return false;
    }
    if (!state.mutex.has_borrow()) {
        fail_sequence(report,
                      state,
                      step,
                      "Mutex borrow ledger did not record live MutRef");
        return false;
    }
    if (**state.guard != state.expected_value) {
        fail_sequence(report,
                      state,
                      step,
                      "guarded value did not reflect applied mutations");
        return false;
    }
    return report.ok;
}

/**
 * @brief Validate invariants while the generated Mutex model is unlocked.
 *
 * @param state Current Mutex sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being checked.
 * @return True when the unlocked-state invariants hold.
 * @pre `state.guard.has_value()` is expected to be false.
 * @post The function has briefly locked and unlocked the mutex to read the
 * guarded value when the borrow ledger was clear.
 * @invariant An unlocked mutex must report no live borrow before the probe,
 * must allow exactly one probe `MutRef<int>`, and must return to no borrow
 * after that probe guard is dropped.
 * @throws Nothing intentionally; unexpected lock exceptions are converted to
 * sequence failures so shrink metadata remains available.
 * @note Ownership/thread-safety: the probe lock is single-threaded and is
 * created only when the model is unlocked.
 */
bool check_unlocked_state(mutex_sequence_state& state,
                          sequence_report& report,
                          std::size_t step) noexcept {
    if (state.guard.has_value()) {
        fail_sequence(report, state, step, "unlocked model had a live MutRef");
        return false;
    }
    if (state.mutex.has_borrow()) {
        fail_sequence(report,
                      state,
                      step,
                      "Mutex borrow ledger was nonzero after guard drop");
        return false;
    }

    int observed_value = 0;
    bool probe_recorded_borrow = false;
    try {
        {
            auto probe = state.mutex.lock();
            probe_recorded_borrow = state.mutex.has_borrow();
            observed_value = *probe;
        }
    } catch (...) {
        fail_sequence(report,
                      state,
                      step,
                      "Mutex::lock threw while model was unlocked");
        return false;
    }

    if (!probe_recorded_borrow) {
        fail_sequence(report,
                      state,
                      step,
                      "probe MutRef did not record a borrow");
    }
    if (observed_value != state.expected_value) {
        fail_sequence(report,
                      state,
                      step,
                      "unlocked probe observed the wrong guarded value");
    }
    if (state.mutex.has_borrow()) {
        fail_sequence(report,
                      state,
                      step,
                      "probe MutRef did not clear the borrow ledger on drop");
    }
    return report.ok;
}

/**
 * @brief Execute a generated Mutex lock command.
 *
 * @param state Current Mutex sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre `state` is the active replay model.
 * @post If the model was unlocked, `guard` owns a new `MutRef<int>`. If the
 * model was already locked, no recursive blocking lock call is made.
 * @invariant The sequential model treats lock-while-locked as an unobtainable
 * guard attempt, matching the acceptance requirement that `MutRef<T>` is
 * obtainable only when the mutex is unlocked.
 * @throws Nothing intentionally; unexpected lock exceptions become sequence
 * failures with shrink metadata.
 * @note Ownership/thread-safety: obtains at most one guard on one thread.
 */
void execute_lock(mutex_sequence_state& state,
                  sequence_report& report,
                  std::size_t step) noexcept {
    if (state.guard.has_value()) {
        ++state.rejected_lock_attempts;
        check_locked_state(state, report, step);
        return;
    }

    try {
        state.guard.emplace(state.mutex.lock());
    } catch (...) {
        fail_sequence(report,
                      state,
                      step,
                      "Mutex::lock failed when model was unlocked");
        return;
    }

    check_locked_state(state, report, step);
}

/**
 * @brief Execute a generated Mutex mutate command.
 *
 * @param state Current Mutex sequence model.
 * @param rng Random engine used to draw the mutation delta.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre `state` is the active replay model.
 * @post A mutation is applied only when a live guard exists; otherwise the
 * command is a model no-op and the unlocked invariants are checked.
 * @invariant Every applied mutation is performed through `MutRef<int>` and is
 * mirrored in `state.expected_value`.
 * @throws Nothing for passing states.
 * @note Ownership/thread-safety: mutates the guarded integer on one thread.
 */
void execute_mutate(mutex_sequence_state& state,
                    std::mt19937_64& rng,
                    sequence_report& report,
                    std::size_t step) noexcept {
    if (!state.guard.has_value()) {
        check_unlocked_state(state, report, step);
        return;
    }

    const int delta = draw_delta(rng);
    **state.guard += delta;
    state.expected_value += delta;
    ++state.applied_mutations;
    check_locked_state(state, report, step);
}

/**
 * @brief Execute a generated Mutex unlock command.
 *
 * @param state Current Mutex sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre `state` is the active replay model.
 * @post A live guard has been dropped when one existed; otherwise the unlocked
 * invariants have been checked as a no-op.
 * @invariant Dropping the guard must clear `Mutex<int>::has_borrow()` before
 * any later lock obtains a fresh guard.
 * @throws Nothing; `MutRef<T>` destruction is `noexcept`.
 * @note Ownership/thread-safety: releases at most one guard on one thread.
 */
void execute_unlock(mutex_sequence_state& state,
                    sequence_report& report,
                    std::size_t step) noexcept {
    if (!state.guard.has_value()) {
        check_unlocked_state(state, report, step);
        return;
    }

    if (**state.guard != state.expected_value) {
        fail_sequence(report,
                      state,
                      step,
                      "guarded value changed before unlock");
    }
    state.guard.reset();
    check_unlocked_state(state, report, step);
}

/**
 * @brief Dispatch one generated Mutex command.
 *
 * @param command Command kind to execute.
 * @param state Current Mutex sequence model.
 * @param rng Random engine owned by the active property sequence.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre `state` is the active replay model.
 * @post The command's state transition and invariant checks have run.
 * @invariant Dispatch covers exactly the lock/mutate/unlock command domain.
 * @throws Nothing intentionally; command helpers convert unexpected lock
 * exceptions to property failures.
 * @note Ownership/thread-safety: all state belongs to the current replay.
 */
void execute_mutex_command(mutex_command command,
                           mutex_sequence_state& state,
                           std::mt19937_64& rng,
                           sequence_report& report,
                           std::size_t step) noexcept {
    switch (command) {
    case mutex_command::lock:
        execute_lock(state, report, step);
        break;
    case mutex_command::mutate:
        execute_mutate(state, rng, report, step);
        break;
    case mutex_command::unlock:
        execute_unlock(state, report, step);
        break;
    }
}

/**
 * @brief Drop any live guard and validate the final Mutex value.
 *
 * @param state Current Mutex sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step Prefix length associated with cleanup diagnostics.
 * @return No value.
 * @pre `state` may be locked or unlocked.
 * @post The model is unlocked and the guarded value has been read through a
 * fresh probe guard when no earlier invariant failed.
 * @invariant Cleanup makes every replay end with borrow count zero, even when
 * the random command stream did not emit an unlock after the last lock.
 * @throws Nothing; guard destruction is `noexcept` and lock exceptions are
 * converted to sequence failures.
 * @note Ownership/thread-safety: all cleanup occurs on one thread.
 */
void cleanup_mutex_sequence(mutex_sequence_state& state,
                            sequence_report& report,
                            std::size_t step) noexcept {
    if (state.guard.has_value()) {
        state.guard.reset();
    }
    check_unlocked_state(state, report, step);
}

/**
 * @brief Execute one deterministic generated Mutex command sequence.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @param prefix_limit Maximum number of generated commands to execute, or
 * `k_full_sequence_prefix` to execute the full generated sequence.
 * @return Sequence report containing pass/fail status and replay metadata.
 * @pre `rng` is the per-sequence engine supplied by
 * `memsafe::test_support::property::run_sequences`, or another engine seeded
 * with the reported failing sequence seed.
 * @post Any live guard has been dropped before the function returns, so the
 * borrow-ledger zero invariant has been checked at replay cleanup.
 * @invariant The generated operation stream contains only lock, mutate, and
 * unlock commands; mutation is applied only through a live `MutRef<int>`.
 * @throws Construction of the local mutex may throw only if `int`
 * construction throws, which does not occur.
 * @note Ownership/thread-safety: the sequence is single-threaded by design.
 */
sequence_report run_mutex_sequence(std::mt19937_64& rng,
                                   std::size_t prefix_limit) {
    sequence_report report;
    report.generated_steps =
        1u + bounded_random(rng, k_max_commands_per_sequence);
    report.executed_steps = report.generated_steps < prefix_limit
                                ? report.generated_steps
                                : prefix_limit;

    mutex_sequence_state state;
    check_unlocked_state(state, report, 0u);

    for (std::size_t step = 1u; report.ok && step <= report.executed_steps;
         ++step) {
        const mutex_command command = draw_mutex_command(rng);
        execute_mutex_command(command, state, rng, report, step);
    }

    if (report.ok) {
        cleanup_mutex_sequence(state, report, report.executed_steps);
    }
    if (report.ok) {
        capture_model(report, state);
    }

    return report;
}

/**
 * @brief Replay one generated Mutex sequence from a deterministic sequence seed.
 *
 * @param seed Seed reported by `property_driver.hpp` for a specific sequence.
 * @param prefix_limit Command prefix to execute before cleanup.
 * @return Sequence report for the replayed prefix.
 * @pre `seed` is the per-sequence seed, not the property run base seed.
 * @post No global state is modified.
 * @invariant Replays use the same command generator as the property body.
 * @throws Same construction exceptions as `run_mutex_sequence`.
 * @note Ownership/thread-safety: all replay state is local to the call.
 */
sequence_report replay_mutex_sequence(std::uint64_t seed,
                                      std::size_t prefix_limit) {
    std::mt19937_64 rng(seed);
    return run_mutex_sequence(rng, prefix_limit);
}

/**
 * @brief Find the smallest command prefix that still reproduces a failure.
 *
 * @param seed Per-sequence seed reported by the property driver.
 * @param original_steps Number of commands generated for the failing sequence.
 * @return Smallest prefix length in `[0, original_steps]` that fails, or
 * `original_steps` if no smaller failing prefix is found.
 * @pre The full generated sequence identified by `seed` has failed.
 * @post No global state is modified.
 * @invariant Prefixes are tested from shortest to longest to provide stable
 * minimal diagnostics without requiring an external shrink tree.
 * @throws Same construction exceptions as `run_mutex_sequence`.
 * @note Ownership/thread-safety: all replay attempts are independent.
 */
std::size_t find_minimal_failing_prefix(std::uint64_t seed,
                                        std::size_t original_steps) {
    for (std::size_t prefix = 0u; prefix <= original_steps; ++prefix) {
        if (!replay_mutex_sequence(seed, prefix).ok) {
            return prefix;
        }
    }
    return original_steps;
}

/**
 * @brief Build a shrink note for a failing Mutex sequence.
 *
 * @param report Failed sequence report.
 * @return Human-readable note containing the invariant and model counters.
 * @pre `report.ok == false`.
 * @post The returned string owns its diagnostic text.
 * @invariant The note is copied into `property_driver.hpp` shrink metadata so
 * failure output contains seed, sequence index, and minimal prefix.
 * @throws `std::bad_alloc` if string allocation fails.
 * @note Ownership/thread-safety: all state is local to the returned string.
 */
std::string describe_failure(const sequence_report& report) {
    std::string note = report.reason;
    note += "; failing_step=";
    note += std::to_string(report.failing_step);
    note += "; expected_value=";
    note += std::to_string(report.expected_value);
    note += "; applied_mutations=";
    note += std::to_string(report.applied_mutations);
    note += "; rejected_lock_attempts=";
    note += std::to_string(report.rejected_lock_attempts);
    return note;
}

/**
 * @brief Print property-driver failure metadata in replay form.
 *
 * @param result Failed property result returned by `run_sequences`.
 * @return No value.
 * @pre `result.passed() == false`.
 * @post A single diagnostic line containing seed and minimal prefix metadata
 * has been written to standard error.
 * @invariant The emitted seed is the per-sequence replay seed stored in
 * `result.failure.seed`; the shrunk prefix is
 * `result.failure.minimal_steps`.
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
 * @brief Check a completed Mutex property result against F2 thresholds.
 *
 * @param result Result returned by `property::run_sequences`.
 * @return No value.
 * @pre `result` was produced by this translation unit's stateful Mutex
 * property configuration.
 * @post Harness failures are recorded for property failures or insufficient
 * sequence budgets.
 * @invariant A passing run executes the property driver's active stateful
 * threshold, which is 1 000 normally and at least 10 000 under nightly macros.
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

    CHECK(result.config.sequence_count >= prop::stateful_base_threshold());
    CHECK(prop::sequence_threshold(prop::workload::stateful) >= 1000u);
    CHECK(result.passed());

    if (result.passed()) {
        CHECK(result.executed_sequences == result.config.sequence_count);
        CHECK(result.executed_sequences >= prop::stateful_base_threshold());
    }

#if defined(MEMSAFE_PROPERTY_NIGHTLY) || defined(MEMSAFE_NIGHTLY_PROPERTY_TESTS) || \
    defined(MEMSAFE_ENABLE_NIGHTLY_PROPERTY_TESTS) || defined(MEMSAFE_NIGHTLY) ||  \
    defined(MEMSAFE_PROPERTY_NIGHTLY_MULTIPLIER)
    CHECK(prop::active_multiplier() >= 10u);
    CHECK(result.config.sequence_count >=
          prop::stateful_base_threshold() * 10u);
#endif
}

} // namespace

/**
 * @brief Run the `memsafe::Mutex<T>` stateful property test.
 *
 * @retval 0 All generated Mutex command sequences satisfied the guard, value,
 * and borrow-ledger invariants.
 * @retval 1 One or more harness checks failed, with replay seed and minimal
 * prefix reported for property failures.
 * @pre The executable is run by the F4 property lane, which discovers
 * `testing/tests/property/*.cpp` and defines any lane-specific nightly
 * property macros before including `property_driver.hpp`.
 * @post The process exit code is the test verdict consumed by CTest.
 * @invariant One named stateful property target executes for `Mutex<T>` and
 * receives the property driver's full stateful threshold.
 * @throws The test does not intentionally throw. Unexpected allocation
 * failures while formatting a shrink note are caught by `property_driver.hpp`
 * and surfaced as property failures with seed metadata.
 * @note Ownership/thread-safety: all test state is automatic storage or
 * property-driver local state; no worker threads are started.
 */
int main() {
    namespace prop = memsafe::test_support::property;

    prop::run_config config =
        prop::make_config("mutex.lock-mutate-unlock.guard",
                          prop::workload::stateful,
                          0x0535b1u);

    const prop::run_result result = prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const sequence_report report =
                run_mutex_sequence(rng, k_full_sequence_prefix);
            if (report.ok) {
                return true;
            }

            const std::size_t minimal_prefix =
                find_minimal_failing_prefix(shrink.seed,
                                            report.generated_steps);
            const std::string note = describe_failure(report);
            prop::record_shrink(shrink,
                                report.generated_steps,
                                minimal_prefix,
                                shrink.seed,
                                sequence,
                                note.c_str());
            return false;
        });

    check_property_result(result);

    RUN_TESTS("test_mutex_property");
}
