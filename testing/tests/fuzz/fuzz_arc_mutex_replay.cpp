/**
 * @file fuzz_arc_mutex_replay.cpp
 * @brief Byte-driven fuzz replay harness for `memsafe::Arc<memsafe::Mutex<T>>`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0540-TEST.
 *
 * Purpose:
 * - Drive the finalized CPP_MEMSAFE-0500-FUNC `memsafe::Arc<T>` and
 *   `memsafe::Mutex<T>` APIs from arbitrary input bytes through clone, drop,
 *   lock, and unlock commands.
 * - Expose a canonical libFuzzer-shaped
 *   `LLVMFuzzerTestOneInput(const uint8_t*, size_t)` entry point while using
 *   the CPP_MEMSAFE-0035-TEST `fuzz_replay.hpp` adapter to run as a normal
 *   deterministic executable in the fuzz lane.
 * - Replay built-in byte seeds and optional command-line corpus files under
 *   the same oracle that a future true libFuzzer target would use. As with
 *   CPP_MEMSAFE-0230-TEST, timed libFuzzer execution (`>=5 min/change` and
 *   `24 CPU-h nightly` per F2) is optional future infrastructure, not a
 *   required drop-in artifact for this F4 replay target.
 * - Remain the canonical fuzz-lane translation unit. Sanitizer-lane
 *   reachability is provided by
 *   `testing/tests/test_arc_mutex_fuzz_replay.cpp`, a TEST-scope wrapper that
 *   includes this file from the top-level `testing/tests/*.cpp` discovery set.
 *   This intentionally avoids a `testing/infra_lanes.json` change while still
 *   compiling and running the same replay oracle under ASan/UBSan.
 *
 * State machine:
 * - Each input byte selects one of four commands by `byte % 4` and carries a
 *   small operand by `byte / 4`.
 * - `clone` copies a live `Arc<Mutex<payload>>` into an empty model slot.
 * - `drop` resets one live `Arc` slot; when this is the final owner and no
 *   mutex guard is live, the payload must be destroyed exactly once.
 * - `lock` obtains a `Mutex<T>::lock()` `MutRef<T>` when the model is
 *   unlocked, or mutates through the already-live guard instead of attempting a
 *   recursive `std::mutex` lock.
 * - `unlock` drops the live guard and verifies the checked borrow ledger is
 *   clear. Unlock without a live guard is a no-op that still validates the
 *   unlocked state through a probe lock.
 *
 * Key invariants:
 * - The harness is total for every byte sequence. It never dereferences an
 *   empty Arc, never attempts to recursively lock a non-recursive mutex, and
 *   never drops the final Arc owner while a mutex-backed `MutRef<T>` is live.
 * - `Arc<T>::strong_count()` equals the number of live model slots after every
 *   clone, drop, lock, unlock, and cleanup transition.
 * - All live Arc handles in an episode share the same `Mutex<T>` payload
 *   address.
 * - While a guard is live, `Mutex<T>::has_borrow()` is true and the guarded
 *   payload value matches the byte-stream model. After the guard is dropped,
 *   `has_borrow()` is false and a fresh probe lock observes the modeled value.
 * - Every episode constructs one tracked payload and destroys it exactly once
 *   when the final Arc owner is dropped. The replay must be clean under
 *   sanitizer lanes: any ASan or UBSan report is a defect in the code under
 *   test, not an accepted negative test outcome.
 *
 * Ownership and thread-safety:
 * - Each fuzz input owns a fresh model, Arc slots, Mutex payload, optional
 *   mutex guard, and destruction ledgers. No state persists across inputs.
 * - This harness is intentionally single-threaded. Cross-thread Arc/Mutex
 *   interleavings are covered by CPP_MEMSAFE-0530-TEST; this package covers
 *   the F2 byte-driven state-machine fuzz surface.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force checked Mutex borrow bookkeeping for the fuzz replay oracle.
 *
 * @retval 1 Enables `Mutex<T>::has_borrow()` and Arc empty-owner checks in
 * every lane that builds this translation unit.
 * @pre Define before including any memsafe feature header.
 * @post The harness can assert lock/unlock borrow-ledger transitions even when
 * an outer release lane supplied `MEMSAFE_RELEASE_CHECKS=0`.
 * @invariant CPP_MEMSAFE-0540-TEST validates the checked Arc/Mutex state
 * machine, so the replay oracle must not inherit a check-free configuration.
 * @throws Nothing directly; the macro only selects compile-time library code.
 * @note Ownership/thread-safety: preprocessor configuration only.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception reporting for unexpected Arc/Mutex violations.
 *
 * @retval MEMSAFE_VIOLATION_THROW Routes library-managed invariant violations
 * to `memsafe::violation` exceptions.
 * @pre Define before including `<memsafe/sync.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post Unexpected empty-Arc, null-MutRef, or mutex-ledger violations become
 * ordinary failing replay statuses instead of aborting the process.
 * @invariant Normal fuzz commands are modeled so they should not trigger
 * memsafe violations; any thrown violation is treated as an oracle failure.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: compile-time selection only. No handler slot
 * is installed by this harness.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../support/fuzz_replay.hpp"

#include <memsafe/sync.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <vector>

namespace {

/// Fixed number of Arc handle slots retained by one fuzz input.
constexpr std::size_t k_arc_slot_count = 16u;

/// Upper bound on commands executed for one input byte stream.
constexpr std::size_t k_max_steps = 4096u;

/// Maximum number of Arc/Mutex payload episodes possible in one input.
constexpr std::size_t k_max_episodes = k_max_steps + 1u;

/// Number of commands in the byte-driven Arc/Mutex state machine.
constexpr std::uint8_t k_command_count = 4u;

/// Modulus for deterministic initial payload values in fuzz episodes.
constexpr int k_initial_value_modulus = 1000003;

/// Number of small mutation deltas in the range [-8, 8].
constexpr std::uint8_t k_delta_domain_size = 17u;

/// Bias used to center command-operand deltas around zero.
constexpr int k_delta_bias = 8;

/**
 * @brief Command kinds consumed by the Arc/Mutex fuzz state machine.
 *
 * @pre Values are produced only by `decode_command` or `command_byte`.
 * @post Dispatching a value applies one modeled public API transition.
 * @invariant The domain is exactly clone, drop, lock, and unlock, matching
 * CPP_MEMSAFE-0540-TEST acceptance.
 * @throws Nothing; enum values own no resources.
 * @note Ownership/thread-safety: command values are local to one replay.
 */
enum class arc_mutex_command : std::uint8_t {
    /// Copy a live Arc handle into an empty model slot.
    clone = 0u,
    /// Reset a live Arc handle, possibly releasing the final owner.
    drop = 1u,
    /// Acquire a Mutex guard or mutate through the existing guard.
    lock = 2u,
    /// Drop the current Mutex guard when one is live.
    unlock = 3u
};

/**
 * @brief Encode a command and operand into one corpus byte.
 *
 * @param command Command selector to encode.
 * @param operand Small operand used by the command.
 * @return Byte whose modulo and quotient decode to `command` and `operand`.
 * @pre `operand <= 63`; built-in seeds satisfy this bound.
 * @post No global state is modified.
 * @invariant The encoding mirrors `decode_command`: `byte % 4` is the command
 * and `byte / 4` is the operand.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure arithmetic helper.
 */
constexpr std::uint8_t command_byte(arc_mutex_command command,
                                    std::uint8_t operand) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(command) + k_command_count * operand);
}

/**
 * @brief Decode the command selector from one input byte.
 *
 * @param byte Byte consumed from the corpus stream.
 * @return Command selected by `byte % k_command_count`.
 * @pre None; every byte value is accepted.
 * @post No state is modified.
 * @invariant The modulo result always maps to a declared `arc_mutex_command`
 * enumerator.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure arithmetic helper.
 */
constexpr arc_mutex_command decode_command(std::uint8_t byte) noexcept {
    return static_cast<arc_mutex_command>(byte % k_command_count);
}

/**
 * @brief Decode the operand component from one input byte.
 *
 * @param byte Byte consumed from the corpus stream.
 * @return Operand in the range `[0, 63]`.
 * @pre None; every byte value is accepted.
 * @post No state is modified.
 * @invariant The operand is used only after reducing it modulo the relevant
 * live-slot, empty-slot, or delta domain.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure arithmetic helper.
 */
constexpr std::uint8_t decode_operand(std::uint8_t byte) noexcept {
    return static_cast<std::uint8_t>(byte / k_command_count);
}

/**
 * @brief Convert a byte operand into a small signed payload delta.
 *
 * @param operand Operand decoded from the command byte.
 * @return Mutation delta in the inclusive range `[-8, 8]`.
 * @pre None; every operand value is accepted.
 * @post No state is modified.
 * @invariant The bounded delta prevents signed overflow across
 * `k_max_steps` while still exercising positive, negative, and zero mutation.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure arithmetic helper.
 */
constexpr int decode_delta(std::uint8_t operand) noexcept {
    return static_cast<int>(operand % k_delta_domain_size) - k_delta_bias;
}

/**
 * @brief Mutable ledger for one Arc-managed Mutex payload episode.
 *
 * @pre The ledger must outlive the tracked payload that points at it.
 * @post Payload construction and destruction update the counters directly.
 * @invariant A passing episode has exactly one construction and exactly one
 * destruction. `double_destroy` remains false.
 * @throws Nothing; the aggregate owns only scalar fields.
 * @note Ownership/thread-safety: stack-owned by one replay and used on one
 * thread.
 */
struct destruction_ledger final {
    /// Number of tracked payload constructors that completed.
    std::size_t constructed = 0u;
    /// Number of tracked payload destructors observed.
    std::size_t destroyed = 0u;
    /// Final payload value observed by the destructor.
    int last_destroyed_value = 0;
    /// True if a destructor ran beyond the construction count.
    bool double_destroy = false;
};

/**
 * @brief Mutex-guarded payload with observable lifetime and value mutations.
 *
 * @details
 * `memsafe::Arc<memsafe::Mutex<T>>` should share exactly one mutex-protected
 * payload across Arc clones. This non-copyable payload records lifetime events
 * and carries an integer value mutated only through live `MutRef<T>` guards.
 *
 * Example:
 * @code
 * destruction_ledger ledger;
 * memsafe::Arc<memsafe::Mutex<tracked_payload>> shared(ledger, 4);
 * auto guard = shared->lock();
 * guard->add_delta(3);
 * @endcode
 *
 * @pre The supplied ledger outlives the payload.
 * @post Construction increments `constructed`; destruction increments
 * `destroyed` and records the final value.
 * @invariant `ledger_` is stable for the payload lifetime and `value_` changes
 * only through guarded fuzz-model mutations.
 * @throws Nothing; construction, mutation, observation, and destruction are
 * `noexcept`.
 * @note Ownership/thread-safety: each instance is owned by one
 * `memsafe::Mutex<T>` payload and accessed through that mutex by the harness.
 */
class tracked_payload final {
public:
    /**
     * @brief Construct a tracked payload and record construction.
     *
     * @param ledger Ledger that receives lifetime events.
     * @param initial_value Initial guarded integer value.
     * @return Constructors do not return a value.
     * @pre `ledger` remains alive until this payload is destroyed.
     * @post `ledger.constructed` increased by one and `value()` returns
     * `initial_value`.
     * @invariant Construction performs no allocation so replay failures point
     * at Arc/Mutex storage and synchronization, not payload setup.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes one mutex-owned payload.
     */
    tracked_payload(destruction_ledger& ledger, int initial_value) noexcept
        : ledger_(&ledger), value_(initial_value) {
        ++ledger_->constructed;
    }

    /**
     * @brief Destroy the tracked payload and record final state.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post `ledger_->destroyed` increased by one; `last_destroyed_value`
     * records this object's final value; `double_destroy` is set if destruction
     * outnumbered construction.
     * @invariant Final Arc release should be the only destructor path for the
     * payload in an episode.
     * @throws Nothing.
     * @note Ownership/thread-safety: called by `Mutex<T>` destruction during
     * final Arc release or replay cleanup.
     */
    ~tracked_payload() noexcept {
        if (ledger_->destroyed >= ledger_->constructed) {
            ledger_->double_destroy = true;
        }
        ++ledger_->destroyed;
        ledger_->last_destroyed_value = value_;
    }

    /**
     * @brief Return the current guarded value.
     *
     * @return Current integer value.
     * @pre The payload is alive and the caller holds the mutex guard or is in a
     * single-threaded construction/destruction check.
     * @post The payload is unchanged.
     * @invariant The value is the oracle mirrored by the fuzz model.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; in this harness it is read
     * through `Mutex<T>::lock()` or a live `MutRef<T>`.
     */
    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    /**
     * @brief Add a small byte-derived delta to the guarded value.
     *
     * @param delta Signed mutation delta.
     * @return Nothing.
     * @pre The payload is alive and the caller holds the mutex guard.
     * @post `value()` is increased by `delta`.
     * @invariant `delta` is bounded by `decode_delta`, so the model cannot
     * overflow the integer across the capped command count.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutation occurs only through a live
     * mutex-backed `MutRef<T>`.
     */
    void add_delta(int delta) noexcept {
        value_ += delta;
    }

private:
    tracked_payload(const tracked_payload& other) = delete;
    tracked_payload& operator=(const tracked_payload& other) = delete;

    /// Non-owning pointer to the replay ledger.
    destruction_ledger* ledger_;
    /// Integer guarded by `memsafe::Mutex<tracked_payload>`.
    int value_;
};

/// Arc/Mutex specialization exercised by the replay state machine.
using arc_mutex_type = memsafe::Arc<memsafe::Mutex<tracked_payload>>;

/// Mutex-backed mutable borrow type returned by `Mutex<tracked_payload>::lock`.
using guard_type = memsafe::MutRef<tracked_payload>;

/**
 * @brief Mutable model state for one fuzz input.
 *
 * @pre Construct a fresh state for every `LLVMFuzzerTestOneInput` call.
 * @post Ledgers, Arc slots, and any live guard are owned by this object.
 * @invariant Member order is intentional: ledgers are declared before Arc
 * slots and the optional guard is declared after Arc slots, so destruction
 * releases any guard first, then Arc-owned mutex payloads, then ledgers.
 * @throws Default construction can throw only through `std::array` element
 * construction; the contained Arc handles are default-noexcept.
 * @note Ownership/thread-safety: all fields are local to one single-threaded
 * replay.
 */
struct machine_state final {
    /// Per-episode payload lifetime ledgers.
    std::array<destruction_ledger, k_max_episodes> ledgers{};
    /// Public Arc handle slots controlled by clone/drop bytes.
    std::array<arc_mutex_type, k_arc_slot_count> slots{};
    /// Optional live mutex-backed mutable reference.
    std::optional<guard_type> guard;
    /// Number of initialized ledgers/episodes.
    std::size_t episode_count = 0u;
    /// Number of live Arc slots in `slots`.
    std::size_t live_count = 0u;
    /// Ledger for the currently live episode, or null between episodes.
    destruction_ledger* active_ledger = nullptr;
    /// Shared Mutex payload address every live Arc slot must report.
    memsafe::Mutex<tracked_payload>* active_mutex = nullptr;
    /// Guarded integer value expected by the model.
    int expected_value = 0;
};

/**
 * @brief Report a replay invariant failure.
 *
 * @param step One-based command index, or zero for setup/final cleanup.
 * @param reason Static diagnostic naming the violated invariant.
 * @retval false Always returns false for convenient propagation.
 * @pre `reason != nullptr`.
 * @post A diagnostic line has been written to standard error.
 * @invariant Failures are reported as nonzero fuzz-entry return values rather
 * than sanitizer-negative expectations.
 * @throws Nothing.
 * @note Ownership/thread-safety: writes to process-local `stderr` only.
 */
bool fail_at(std::size_t step, const char* reason) noexcept {
    std::fprintf(stderr,
                 "fuzz arc/mutex failure at step %zu: %s\n",
                 step,
                 reason == nullptr ? "unknown invariant" : reason);
    return false;
}

/**
 * @brief Find the slot index for the requested live Arc ordinal.
 *
 * @param state Current replay state.
 * @param live_ordinal Zero-based ordinal among live Arc slots.
 * @return Slot index, or `k_arc_slot_count` when no such live slot exists.
 * @pre `state` is the active replay model.
 * @post No state is modified.
 * @invariant `Arc<T>::has_value()` is the only liveness observer used by the
 * model.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads input-local state only.
 */
std::size_t nth_live_slot(const machine_state& state,
                          std::size_t live_ordinal) noexcept {
    std::size_t seen = 0u;
    for (std::size_t index = 0u; index < state.slots.size(); ++index) {
        if (!state.slots[index].has_value()) {
            continue;
        }
        if (seen == live_ordinal) {
            return index;
        }
        ++seen;
    }
    return k_arc_slot_count;
}

/**
 * @brief Find the first empty Arc slot.
 *
 * @param state Current replay state.
 * @return Empty slot index, or `k_arc_slot_count` when all slots are live.
 * @pre `state` is the active replay model.
 * @post No state is modified.
 * @invariant Empty slots must report both `has_value() == false` and
 * `strong_count() == 0` in a passing state.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads input-local state only.
 */
std::size_t first_empty_slot(const machine_state& state) noexcept {
    for (std::size_t index = 0u; index < state.slots.size(); ++index) {
        if (!state.slots[index].has_value()) {
            return index;
        }
    }
    return k_arc_slot_count;
}

/**
 * @brief Validate Arc strong-count and shared-payload invariants.
 *
 * @param state Current replay state.
 * @param step Command step used in diagnostics.
 * @retval true Arc slot and active episode invariants hold.
 * @retval false An Arc invariant failed.
 * @pre `state` is the active replay model.
 * @post No state is modified.
 * @invariant Every live slot reports `strong_count() == state.live_count` and
 * the same `active_mutex` address.
 * @throws Nothing.
 * @note Ownership/thread-safety: observes public Arc APIs on one thread.
 */
bool check_arc_invariants(machine_state& state, std::size_t step) noexcept {
    std::size_t observed_live = 0u;

    for (std::size_t index = 0u; index < state.slots.size(); ++index) {
        const arc_mutex_type& slot = state.slots[index];
        if (slot.has_value()) {
            ++observed_live;
            if (slot.strong_count() !=
                static_cast<std::uint64_t>(state.live_count)) {
                return fail_at(step,
                               "Arc strong_count did not match live model");
            }
            if (state.active_mutex == nullptr ||
                slot.get() != state.active_mutex) {
                return fail_at(step,
                               "live Arc slots did not share one Mutex");
            }
        } else if (slot.strong_count() != 0u) {
            return fail_at(step, "empty Arc slot reported nonzero count");
        }
    }

    if (observed_live != state.live_count) {
        return fail_at(step, "live Arc slot count diverged from model");
    }
    if (state.live_count == 0u) {
        if (state.active_mutex != nullptr || state.active_ledger != nullptr) {
            return fail_at(step, "inactive episode retained active pointers");
        }
        if (state.guard.has_value()) {
            return fail_at(step, "mutex guard outlived all Arc owners");
        }
        return true;
    }

    if (state.active_ledger == nullptr || state.active_mutex == nullptr) {
        return fail_at(step, "live episode had no active ledger or mutex");
    }
    if (state.active_ledger->constructed != 1u ||
        state.active_ledger->destroyed != 0u ||
        state.active_ledger->double_destroy) {
        return fail_at(step,
                       "live Arc episode had invalid payload lifetime ledger");
    }
    return true;
}

/**
 * @brief Validate state while a mutex guard is live.
 *
 * @param state Current replay state.
 * @param step Command step used in diagnostics.
 * @retval true Locked-state invariants hold.
 * @retval false The guard, borrow ledger, or guarded value is wrong.
 * @pre `state.guard.has_value()` should be true.
 * @post No state is intentionally modified.
 * @invariant A live guard must imply at least one live Arc owner, a non-null
 * active mutex, `has_borrow() == true`, and the modeled guarded value.
 * @throws Nothing; unexpected dereference exceptions are converted to false.
 * @note Ownership/thread-safety: observes one live guard on one thread.
 */
bool check_locked_state(machine_state& state, std::size_t step) noexcept {
    if (!check_arc_invariants(state, step)) {
        return false;
    }
    if (!state.guard.has_value()) {
        return fail_at(step, "locked-state check had no live guard");
    }
    if (state.active_mutex == nullptr || !state.active_mutex->has_borrow()) {
        return fail_at(step,
                       "Mutex borrow ledger did not record live guard");
    }

    try {
        if ((**state.guard).value() != state.expected_value) {
            return fail_at(step, "guarded value did not match model");
        }
    } catch (...) {
        return fail_at(step, "live MutRef dereference threw");
    }
    return true;
}

/**
 * @brief Validate state while no mutex guard is live.
 *
 * @param state Current replay state.
 * @param step Command step used in diagnostics.
 * @retval true Unlocked-state invariants hold.
 * @retval false The borrow ledger, probe lock, or guarded value is wrong.
 * @pre `state.guard.has_value()` should be false.
 * @post When an active episode exists, the mutex has been briefly locked and
 * unlocked to prove the payload value and ledger transition.
 * @invariant Probe locking is safe because the model is unlocked; it exercises
 * `Mutex<T>::lock()` without risking recursive self-deadlock.
 * @throws Nothing; unexpected lock exceptions are converted to false.
 * @note Ownership/thread-safety: uses one synchronous probe guard.
 */
bool check_unlocked_state(machine_state& state, std::size_t step) noexcept {
    if (!check_arc_invariants(state, step)) {
        return false;
    }
    if (state.guard.has_value()) {
        return fail_at(step, "unlocked-state check had a live guard");
    }
    if (state.active_mutex == nullptr) {
        return true;
    }
    if (state.active_mutex->has_borrow()) {
        return fail_at(step, "Mutex borrow ledger leaked after unlock");
    }

    int observed_value = 0;
    bool probe_recorded_borrow = false;
    try {
        {
            auto probe = state.active_mutex->lock();
            probe_recorded_borrow = state.active_mutex->has_borrow();
            observed_value = probe->value();
        }
    } catch (...) {
        return fail_at(step, "probe Mutex::lock threw while unlocked");
    }

    if (!probe_recorded_borrow) {
        return fail_at(step, "probe lock did not record a borrow");
    }
    if (observed_value != state.expected_value) {
        return fail_at(step, "probe lock observed wrong guarded value");
    }
    if (state.active_mutex->has_borrow()) {
        return fail_at(step, "probe guard did not clear borrow on drop");
    }
    return true;
}

/**
 * @brief Validate the current model according to its lock state.
 *
 * @param state Current replay state.
 * @param step Command step used in diagnostics.
 * @retval true All active invariants hold.
 * @retval false An invariant failed.
 * @pre `state` is the active replay model.
 * @post May perform a probe lock when the model is unlocked and an episode is
 * live.
 * @invariant The branch is determined only by `state.guard.has_value()`.
 * @throws Nothing; helper failures are converted to false.
 * @note Ownership/thread-safety: single-threaded replay only.
 */
bool check_machine_state(machine_state& state, std::size_t step) noexcept {
    return state.guard.has_value() ? check_locked_state(state, step)
                                   : check_unlocked_state(state, step);
}

/**
 * @brief Build the deterministic initial value for a new episode.
 *
 * @param episode_index Zero-based episode index.
 * @param operand Byte-derived operand from the command that needed an episode.
 * @return Initial guarded payload value.
 * @pre `episode_index < k_max_episodes`.
 * @post No state is modified.
 * @invariant Values stay small enough that all later bounded deltas cannot
 * overflow `int` within `k_max_steps`.
 * @throws Nothing.
 * @note Ownership/thread-safety: pure arithmetic helper.
 */
int initial_value_for_episode(std::size_t episode_index,
                              std::uint8_t operand) noexcept {
    const std::size_t raw =
        episode_index * 131u + static_cast<std::size_t>(operand) * 17u + 5u;
    return static_cast<int>(raw % static_cast<std::size_t>(k_initial_value_modulus));
}

/**
 * @brief Start a fresh Arc/Mutex episode when no Arc owners are live.
 *
 * @param state Current replay state to mutate.
 * @param operand Byte-derived operand used to choose the initial value.
 * @param step Command step used in diagnostics.
 * @retval true A live root Arc/Mutex episode exists and invariants hold.
 * @retval false Construction or an initial invariant failed.
 * @pre `state.live_count == 0` and no guard is live.
 * @post Slot zero owns a new `Arc<Mutex<tracked_payload>>` with strong count
 * one when construction succeeds.
 * @invariant Fresh episodes let arbitrary byte streams continue after a final
 * drop while preserving one-payload lifetime accounting.
 * @throws `std::bad_alloc` or propagated construction exceptions from
 * `memsafe::Arc<T>`/`memsafe::Mutex<T>`.
 * @note Ownership/thread-safety: constructs local replay state only.
 */
bool ensure_episode(machine_state& state,
                    std::uint8_t operand,
                    std::size_t step) {
    if (state.live_count > 0u) {
        return true;
    }
    if (state.guard.has_value()) {
        return fail_at(step, "cannot start an episode while guard is live");
    }
    if (state.episode_count >= state.ledgers.size()) {
        return fail_at(step, "episode budget exhausted");
    }

    destruction_ledger& ledger = state.ledgers[state.episode_count];
    ledger = destruction_ledger{};
    const int initial_value =
        initial_value_for_episode(state.episode_count, operand);
    ++state.episode_count;

    state.slots[0] = arc_mutex_type(ledger, initial_value);
    state.live_count = 1u;
    state.active_ledger = &ledger;
    state.active_mutex = state.slots[0].get();
    state.expected_value = initial_value;

    if (state.active_mutex == nullptr) {
        return fail_at(step, "Arc construction produced null Mutex payload");
    }
    if (ledger.constructed != 1u || ledger.destroyed != 0u ||
        ledger.double_destroy) {
        return fail_at(step, "new episode ledger was not initialized once");
    }
    return check_machine_state(state, step);
}

/**
 * @brief Verify and clear the active episode after final Arc release.
 *
 * @param state Current replay state whose final Arc owner was just dropped.
 * @param step Command step used in diagnostics.
 * @retval true The payload was destroyed exactly once with the modeled value.
 * @retval false The final-release lifetime invariant failed.
 * @pre `state.live_count == 0`, no guard is live, and `active_ledger` points
 * to the episode whose final owner was just released.
 * @post Active episode pointers are cleared on success.
 * @invariant Count zero for an Arc episode must coincide with one payload
 * destructor and no double-destruction flag.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads the stack-owned ledger only.
 */
bool verify_final_release(machine_state& state, std::size_t step) noexcept {
    destruction_ledger* const ledger = state.active_ledger;
    if (ledger == nullptr) {
        return fail_at(step, "final Arc release had no active ledger");
    }
    if (state.guard.has_value()) {
        return fail_at(step, "final Arc release occurred while guard live");
    }
    if (ledger->constructed != 1u || ledger->destroyed != 1u ||
        ledger->double_destroy) {
        return fail_at(step,
                       "Arc final release did not destroy payload exactly once");
    }
    if (ledger->last_destroyed_value != state.expected_value) {
        return fail_at(step,
                       "payload destructor observed wrong final value");
    }

    state.active_ledger = nullptr;
    state.active_mutex = nullptr;
    state.expected_value = 0;
    return check_machine_state(state, step);
}

/**
 * @brief Mutate through the currently live mutex guard.
 *
 * @param state Current replay state.
 * @param operand Byte-derived operand used to select the delta.
 * @param step Command step used in diagnostics.
 * @retval true The mutation and locked-state invariants succeeded.
 * @retval false No guard existed or the guarded value diverged.
 * @pre `state.guard.has_value()` is true.
 * @post The payload and model values have both been changed by the same
 * bounded delta.
 * @invariant Every payload write happens through the public `MutRef<T>`
 * returned by `Mutex<T>::lock()`.
 * @throws Nothing; unexpected guard exceptions become false.
 * @note Ownership/thread-safety: mutates one guarded payload on one thread.
 */
bool mutate_live_guard(machine_state& state,
                       std::uint8_t operand,
                       std::size_t step) noexcept {
    if (!state.guard.has_value()) {
        return fail_at(step, "mutation requested without a live guard");
    }

    try {
        const int delta = decode_delta(operand);
        (**state.guard).add_delta(delta);
        state.expected_value += delta;
    } catch (...) {
        return fail_at(step, "mutating through live MutRef threw");
    }
    return check_locked_state(state, step);
}

/**
 * @brief Apply one clone command.
 *
 * @param state Current replay state.
 * @param operand Byte-derived operand selecting the source live Arc.
 * @param step Command step used in diagnostics.
 * @retval true The clone command preserved all invariants.
 * @retval false An invariant failed.
 * @pre `state` is the active replay model.
 * @post If an empty slot was available, one new Arc handle shares the active
 * Mutex payload; otherwise the command is a bounded no-op.
 * @invariant Cloning copies ownership only; it must not duplicate or destroy
 * the guarded payload.
 * @throws `memsafe::violation` if Arc retain exhausts the strong count, which
 * is unreachable for the bounded slot count unless the code under test is
 * broken.
 * @note Ownership/thread-safety: mutates local Arc handle slots only.
 */
bool apply_clone(machine_state& state,
                 std::uint8_t operand,
                 std::size_t step) {
    if (!ensure_episode(state, operand, step)) {
        return false;
    }

    const std::size_t target_index = first_empty_slot(state);
    if (target_index >= state.slots.size()) {
        return check_machine_state(state, step);
    }

    const std::size_t source_index =
        nth_live_slot(state, operand % state.live_count);
    if (source_index >= state.slots.size()) {
        return fail_at(step, "clone command could not find live source");
    }

    state.slots[target_index] = state.slots[source_index];
    ++state.live_count;
    return check_machine_state(state, step);
}

/**
 * @brief Drop one selected live Arc slot.
 *
 * @param state Current replay state.
 * @param slot_index Slot index to reset.
 * @param step Command step used in diagnostics.
 * @retval true Drop preserved every invariant.
 * @retval false Drop exposed an ownership or lifetime invariant failure.
 * @pre `slot_index < state.slots.size()` and the slot is live.
 * @post The selected slot is empty. If it was the final owner, the episode has
 * been verified and cleared.
 * @invariant The final owner is never dropped while `state.guard` is live,
 * avoiding destruction of a locked mutex and preserving sanitizer cleanliness.
 * @throws Nothing; `Arc<T>::reset()` is `noexcept`.
 * @note Ownership/thread-safety: releases one local Arc handle.
 */
bool drop_live_slot(machine_state& state,
                    std::size_t slot_index,
                    std::size_t step) noexcept {
    if (slot_index >= state.slots.size() ||
        !state.slots[slot_index].has_value()) {
        return fail_at(step, "drop command selected an invalid live slot");
    }

    const bool dropping_final_owner = state.live_count == 1u;
    if (dropping_final_owner && state.guard.has_value()) {
        return fail_at(step, "attempted to drop final Arc while guard live");
    }

    state.slots[slot_index].reset();
    --state.live_count;

    if (dropping_final_owner) {
        return verify_final_release(state, step);
    }
    return check_machine_state(state, step);
}

/**
 * @brief Apply one drop command.
 *
 * @param state Current replay state.
 * @param operand Byte-derived operand selecting the live Arc slot to drop.
 * @param step Command step used in diagnostics.
 * @retval true The drop command preserved all invariants.
 * @retval false An invariant failed.
 * @pre `state` is the active replay model.
 * @post One live Arc may have been reset. If only the guard-protecting final
 * owner existed, the command is a no-op that validates the locked state.
 * @invariant Arbitrary bytes cannot force destruction of a Mutex while its
 * returned `MutRef<T>` is still live.
 * @throws `std::bad_alloc` or construction exceptions only if the command had
 * to bootstrap a fresh episode first.
 * @note Ownership/thread-safety: mutates local Arc handle slots only.
 */
bool apply_drop(machine_state& state,
                std::uint8_t operand,
                std::size_t step) {
    if (!ensure_episode(state, operand, step)) {
        return false;
    }

    if (state.guard.has_value() && state.live_count == 1u) {
        return check_locked_state(state, step);
    }

    const std::size_t target_index =
        nth_live_slot(state, operand % state.live_count);
    return drop_live_slot(state, target_index, step);
}

/**
 * @brief Apply one lock command.
 *
 * @param state Current replay state.
 * @param operand Byte-derived operand selecting the source Arc and mutation
 * delta.
 * @param step Command step used in diagnostics.
 * @retval true The lock command preserved all invariants.
 * @retval false Locking, mutation, or locked-state validation failed.
 * @pre `state` is the active replay model.
 * @post If the model was unlocked, `state.guard` owns a new mutex-backed
 * `MutRef<T>`. If already locked, the existing guard is used for another
 * bounded mutation and no recursive lock is attempted.
 * @invariant Repeated lock bytes are total and non-blocking even though
 * `std::mutex` is non-recursive.
 * @throws `std::bad_alloc` or construction exceptions only if the command had
 * to bootstrap a fresh episode first.
 * @note Ownership/thread-safety: obtains at most one guard on one thread.
 */
bool apply_lock(machine_state& state,
                std::uint8_t operand,
                std::size_t step) {
    if (!ensure_episode(state, operand, step)) {
        return false;
    }

    if (state.guard.has_value()) {
        return mutate_live_guard(state, operand, step);
    }

    const std::size_t source_index =
        nth_live_slot(state, operand % state.live_count);
    if (source_index >= state.slots.size()) {
        return fail_at(step, "lock command could not find live source");
    }

    try {
        state.guard.emplace(state.slots[source_index]->lock());
    } catch (...) {
        return fail_at(step, "Mutex::lock threw while model was unlocked");
    }

    return mutate_live_guard(state, operand, step);
}

/**
 * @brief Apply one unlock command.
 *
 * @param state Current replay state.
 * @param operand Byte-derived operand, unused except for total dispatch
 * symmetry with other commands.
 * @param step Command step used in diagnostics.
 * @retval true The unlock command preserved all invariants.
 * @retval false A guard, borrow-ledger, or value invariant failed.
 * @pre `state` is the active replay model.
 * @post Any live guard has been destroyed and the mutex borrow ledger is clear.
 * @invariant Unlock without a live guard is a safe no-op because byte streams
 * are arbitrary; it still validates the unlocked state.
 * @throws Nothing; `MutRef<T>` destruction is `noexcept`.
 * @note Ownership/thread-safety: releases one optional guard on one thread.
 */
bool apply_unlock(machine_state& state,
                  std::uint8_t operand,
                  std::size_t step) noexcept {
    (void)operand;
    if (!state.guard.has_value()) {
        return check_unlocked_state(state, step);
    }
    if (!check_locked_state(state, step)) {
        return false;
    }

    state.guard.reset();
    return check_unlocked_state(state, step);
}

/**
 * @brief Dispatch one decoded command.
 *
 * @param state Current replay state.
 * @param command Command selector.
 * @param operand Byte-derived operand.
 * @param step One-based command index used in diagnostics.
 * @retval true The command preserved all modeled invariants.
 * @retval false A modeled invariant failed.
 * @pre `command` is one of the declared `arc_mutex_command` values.
 * @post The command transition and its invariant checks have run.
 * @invariant Dispatch covers exactly the clone/drop/lock/unlock domain.
 * @throws Propagates allocation/construction exceptions from episode bootstrap
 * or Arc retain; the fuzz entry point converts them to status 1.
 * @note Ownership/thread-safety: all state belongs to one replay.
 */
bool apply_command(machine_state& state,
                   arc_mutex_command command,
                   std::uint8_t operand,
                   std::size_t step) {
    switch (command) {
        case arc_mutex_command::clone:
            return apply_clone(state, operand, step);
        case arc_mutex_command::drop:
            return apply_drop(state, operand, step);
        case arc_mutex_command::lock:
            return apply_lock(state, operand, step);
        case arc_mutex_command::unlock:
            return apply_unlock(state, operand, step);
    }
    return fail_at(step, "unknown Arc/Mutex command");
}

/**
 * @brief Drop all remaining state and verify all episode ledgers.
 *
 * @param state Current replay state to drain.
 * @param step Step count associated with cleanup diagnostics.
 * @retval true Cleanup reached a fully released, balanced state.
 * @retval false Cleanup exposed a leaked guard, wrong final value, or payload
 * lifetime imbalance.
 * @pre `state` may be locked or unlocked and may have zero or more live Arc
 * slots.
 * @post On success, no guard is live, no Arc slot is live, and every initialized
 * ledger has one construction and one destruction.
 * @invariant Even an empty byte input exercises one Arc/Mutex episode before
 * cleanup so the zero-length replay path tests construction, lock probing, and
 * final release.
 * @throws `std::bad_alloc` or construction exceptions if the empty-input path
 * must create its first episode.
 * @note Ownership/thread-safety: drains local replay state only.
 */
bool finalize_input(machine_state& state, std::size_t step) {
    if (state.episode_count == 0u &&
        !ensure_episode(state, 0u, step)) {
        return false;
    }

    if (state.guard.has_value()) {
        if (!check_locked_state(state, step)) {
            return false;
        }
        state.guard.reset();
        if (!check_unlocked_state(state, step)) {
            return false;
        }
    }

    while (state.live_count > 0u) {
        const std::size_t target_index = nth_live_slot(state, 0u);
        if (!drop_live_slot(state, target_index, step)) {
            return false;
        }
    }

    for (std::size_t index = 0u; index < state.episode_count; ++index) {
        const destruction_ledger& ledger = state.ledgers[index];
        if (ledger.constructed != 1u || ledger.destroyed != 1u ||
            ledger.double_destroy) {
            return fail_at(step,
                           "episode ledger did not end balanced exactly once");
        }
    }
    return true;
}

/**
 * @brief Run the byte-driven Arc/Mutex state machine for one input.
 *
 * @param data Pointer to input bytes, or null when `size == 0`.
 * @param size Number of input bytes.
 * @retval true Every modeled invariant held through command execution and
 * cleanup.
 * @retval false An invariant failed.
 * @pre `data` points to at least `size` readable bytes, or `size == 0`.
 * @post A fresh replay model has been exercised and fully drained.
 * @invariant At most `k_max_steps` bytes are executed, and every command is
 * reduced to in-bounds public Arc/Mutex operations.
 * @throws `std::bad_alloc` or construction exceptions from model setup; the
 * fuzz entry point converts them to status 1.
 * @note Ownership/thread-safety: all state is input-local on one thread.
 */
bool run_state_machine(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr && size != 0u) {
        return fail_at(0u, "nonempty input had null data pointer");
    }

    machine_state state;
    const std::size_t steps = size < k_max_steps ? size : k_max_steps;

    for (std::size_t i = 0u; i < steps; ++i) {
        const std::uint8_t byte = data[i];
        if (!apply_command(state,
                           decode_command(byte),
                           decode_operand(byte),
                           i + 1u)) {
            return false;
        }
    }

    return finalize_input(state, steps);
}

/**
 * @brief Build the built-in corpus replayed when no file path is supplied.
 *
 * @return Vector of named byte sequences covering representative command
 * programs.
 * @pre None.
 * @post The returned corpus owns all byte vectors and names.
 * @invariant Seeds cover empty input, final Arc release, clone/drop count
 * changes, lock/unlock ledger transitions, drop while locked, repeated lock
 * bytes, and a long mixed walk.
 * @throws `std::bad_alloc` if seed construction fails.
 * @note Ownership/thread-safety: builds local replay data only.
 */
std::vector<memsafe::test_support::fuzz::byte_sequence> built_in_seeds() {
    namespace fz = memsafe::test_support::fuzz;
    std::vector<fz::byte_sequence> seeds;

    seeds.push_back(fz::bytes_case("empty", {}));

    seeds.push_back(fz::bytes_case(
        "clone-drop-final",
        {command_byte(arc_mutex_command::clone, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::drop, 0u)}));

    seeds.push_back(fz::bytes_case(
        "lock-unlock-final",
        {command_byte(arc_mutex_command::lock, 5u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 0u)}));

    seeds.push_back(fz::bytes_case(
        "drop-while-locked-keeps-owner",
        {command_byte(arc_mutex_command::lock, 8u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::clone, 0u),
         command_byte(arc_mutex_command::drop, 1u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 0u)}));

    seeds.push_back(fz::bytes_case(
        "fill-clone-slots",
        {command_byte(arc_mutex_command::clone, 0u),
         command_byte(arc_mutex_command::clone, 1u),
         command_byte(arc_mutex_command::clone, 2u),
         command_byte(arc_mutex_command::clone, 3u),
         command_byte(arc_mutex_command::clone, 4u),
         command_byte(arc_mutex_command::clone, 5u),
         command_byte(arc_mutex_command::clone, 6u),
         command_byte(arc_mutex_command::clone, 7u),
         command_byte(arc_mutex_command::clone, 8u),
         command_byte(arc_mutex_command::clone, 9u),
         command_byte(arc_mutex_command::clone, 10u),
         command_byte(arc_mutex_command::clone, 11u),
         command_byte(arc_mutex_command::clone, 12u),
         command_byte(arc_mutex_command::clone, 13u),
         command_byte(arc_mutex_command::clone, 14u),
         command_byte(arc_mutex_command::clone, 15u),
         command_byte(arc_mutex_command::lock, 4u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::drop, 0u)}));

    seeds.push_back(fz::bytes_case(
        "mixed-walk",
        {command_byte(arc_mutex_command::clone, 0u),
         command_byte(arc_mutex_command::lock, 2u),
         command_byte(arc_mutex_command::lock, 7u),
         command_byte(arc_mutex_command::clone, 1u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 3u),
         command_byte(arc_mutex_command::lock, 16u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::clone, 5u),
         command_byte(arc_mutex_command::clone, 6u),
         command_byte(arc_mutex_command::lock, 14u),
         command_byte(arc_mutex_command::drop, 1u),
         command_byte(arc_mutex_command::unlock, 0u),
         command_byte(arc_mutex_command::drop, 0u),
         command_byte(arc_mutex_command::drop, 0u)}));

    return seeds;
}

} // namespace

/**
 * @brief libFuzzer-shaped entry point for Arc/Mutex state-machine input.
 *
 * @details The same entry point is used by deterministic replay `main()` and
 * can be linked with a true libFuzzer driver later. The oracle intentionally
 * returns nonzero only for model or library invariant failures; arbitrary byte
 * shape is never a failure by itself.
 *
 * @param data Pointer to input bytes, or null when `size == 0`. The pointer is
 * not dereferenced when the size is zero.
 * @param size Number of input bytes.
 * @retval 0 The input preserved every Arc/Mutex invariant.
 * @retval 1 The input exposed an invariant failure or the harness bookkeeping
 * threw.
 * @pre The pointed-to bytes remain valid for the duration of the call.
 * @post A fresh model was exercised and destroyed; no state persists to the
 * next input.
 * @invariant Sanitizer lanes must see no unexpected report for any input; the
 * harness avoids undefined operations such as recursive lock or destroying a
 * mutex under a live guard.
 * @throws Nothing; all exceptions are caught and converted to status 1.
 * @note Ownership/thread-safety: single-threaded; owns no state across calls.
 *
 * Example:
 * @code
 * const std::uint8_t seed[] = {2u, 3u};
 * int status = LLVMFuzzerTestOneInput(seed, sizeof(seed));
 * @endcode
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    try {
        return run_state_machine(data, size) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "fuzz arc/mutex: harness bookkeeping threw: %s\n",
                     ex.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "fuzz arc/mutex: harness bookkeeping threw\n");
        return 1;
    }
}

/**
 * @brief Deterministic replay entry point for the Arc/Mutex fuzz harness.
 *
 * @param argc Standard argument count.
 * @param argv Standard argument vector. Extra arguments are treated as binary
 * corpus file paths and replayed instead of the built-in seeds.
 * @retval 0 Every replayed input passed.
 * @retval nonzero A corpus file could not be read, or an input exposed an
 * Arc/Mutex invariant failure.
 * @pre Pass the original `argc` and `argv` from the standalone executable.
 * @post Either supplied corpus files or built-in seeds have been driven through
 * `LLVMFuzzerTestOneInput`.
 * @invariant This remains a normal exit-code F4 fuzz replay executable while
 * preserving a true-libFuzzer-compatible entry point for optional future
 * infrastructure.
 * @throws Nothing escapes; `replay_main` and the entry point convert failures
 * into nonzero status.
 * @note Ownership/thread-safety: single-threaded; owns loaded corpus bytes.
 */
int main(int argc, char** argv) {
    return memsafe::test_support::fuzz::replay_main(
        argc, argv, &LLVMFuzzerTestOneInput, built_in_seeds());
}
