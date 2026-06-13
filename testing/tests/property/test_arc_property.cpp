/**
 * @file test_arc_property.cpp
 * @brief Stateful property test for Slice 5 `memsafe::Arc<T>` ownership.
 *
 * @details
 * Work package: CPP_MEMSAFE-0535-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0500-FUNC `memsafe::Arc<T>` artifact
 *   through generated single-threaded clone, drop, and dereference command
 *   sequences.
 * - Use the CPP_MEMSAFE-0035-TEST `property_driver.hpp` stateful threshold so
 *   the property lane executes at least 1 000 sequences in normal CI and
 *   receives the documented 10x nightly multiplier automatically.
 * - Report the deterministic replay seed and the smallest command prefix known
 *   to reproduce any Arc refcount or final-destruction invariant failure.
 *
 * Key invariants:
 * - Every live `Arc<T>` slot in the generated model shares the same payload
 *   address within one episode.
 * - `Arc<T>::strong_count()` equals the number of live public Arc handles after
 *   every generated clone, drop, dereference, and cleanup drop step.
 * - The tracked payload is not destroyed while the generated live-handle count
 *   is nonzero.
 * - The tracked payload is destroyed exactly once when the live-handle count
 *   reaches zero.
 *
 * Ownership and thread-safety:
 * - All generated state is owned by one test process and one test thread.
 * - This property deliberately avoids cross-thread interleavings; those remain
 *   the responsibility of the Slice 5 Relacy concurrency package.
 */
#include "../../test_harness.hpp"

#include "../support/property_driver.hpp"

#include <memsafe/sync.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>

namespace {

/// Maximum generated command count for one stateful Arc property sequence.
constexpr std::size_t k_max_commands_per_sequence = 64u;

/// Fixed Arc handle slots; a pure clone stream needs the root plus all commands.
constexpr std::size_t k_arc_slot_count = k_max_commands_per_sequence + 1u;

/// Payload value modulus used to keep replay diagnostics compact.
constexpr std::uint64_t k_payload_value_modulus = 1000003ull;

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
 * @brief Command kinds generated for the Arc sequential state machine.
 *
 * @pre Values are produced only by `draw_arc_command`.
 * @post Dispatching a command mutates or observes one generated Arc episode.
 * @invariant The command set is limited to clone, drop, and dereference, which
 * is the CPP_MEMSAFE-0535-TEST Arc acceptance surface.
 * @throws Nothing; enum values own no resources.
 * @note Ownership/thread-safety: values are local to one property sequence.
 */
enum class arc_command {
    /// Copy one live Arc handle into an empty generated slot.
    clone,
    /// Reset one live Arc handle, possibly releasing the final strong owner.
    drop,
    /// Dereference one live Arc handle and validate the shared payload.
    deref
};

/**
 * @brief Mutable ledger that records one Arc-managed payload lifetime.
 *
 * @pre The ledger must outlive the tracked payload that points at it.
 * @post Payload construction and destruction update the counters directly.
 * @invariant A passing Arc episode has `constructed == 1` and `destroyed == 1`
 * after the generated live-handle count reaches zero.
 * @throws Nothing; the aggregate owns only scalar fields.
 * @note Ownership/thread-safety: stack-owned by one generated sequence and
 * accessed on one test thread.
 */
struct destruction_ledger final {
    /// Number of tracked payload constructors that completed.
    std::size_t constructed = 0u;
    /// Number of tracked payload destructors observed.
    std::size_t destroyed = 0u;
    /// Last payload value observed by a destructor.
    std::uint64_t last_destroyed_value = 0u;
};

/**
 * @brief Arc-managed payload with observable construction and destruction.
 *
 * @details
 * The payload is intentionally non-copyable so copying `memsafe::Arc<T>` can
 * only satisfy the property by retaining the shared control block. The value
 * observer lets dereference commands verify that every live handle reaches the
 * same object.
 *
 * Example:
 * @code
 * destruction_ledger ledger;
 * memsafe::Arc<tracked_arc_payload> root(ledger, 7u);
 * memsafe::Arc<tracked_arc_payload> clone(root);
 * CHECK(root.strong_count() == clone.strong_count());
 * @endcode
 *
 * @pre The supplied `destruction_ledger` outlives this payload.
 * @post Construction increments `constructed`; destruction increments
 * `destroyed` once and records the final value.
 * @invariant `ledger_` remains a stable non-owning pointer for the payload
 * lifetime.
 * @throws Nothing; construction, destruction, and observation are `noexcept`.
 * @note Ownership/thread-safety: instances are owned by one `Arc<T>` control
 * block. This property accesses the payload on one thread only.
 */
class tracked_arc_payload final {
public:
    /**
     * @brief Construct a tracked payload and record the construction event.
     *
     * @param ledger Ledger receiving construction and destruction counts.
     * @param initial_value Value returned by `value()`.
     * @return Constructors do not return a value.
     * @pre `ledger` remains alive until the final Arc release destroys this
     * payload.
     * @post `ledger.constructed` has increased by one and `value()` returns
     * `initial_value`.
     * @invariant The payload stores only a non-owning ledger pointer and an
     * immutable generated value.
     * @throws Nothing.
     * @note Ownership/thread-safety: called by the Arc control-block
     * constructor on the property thread.
     */
    tracked_arc_payload(destruction_ledger& ledger,
                        std::uint64_t initial_value) noexcept
        : ledger_(&ledger), value_(initial_value) {
        ++ledger_->constructed;
    }

    /**
     * @brief Record the final Arc-managed payload destruction.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post The ledger has one additional destruction event and records the
     * final payload value.
     * @invariant C++ invokes this destructor at most once for this object; the
     * ledger makes that final-release event visible to the property.
     * @throws Nothing.
     * @note Ownership/thread-safety: called by the final `Arc<T>` release on
     * the property thread.
     */
    ~tracked_arc_payload() noexcept {
        ++ledger_->destroyed;
        ledger_->last_destroyed_value = value_;
    }

    /**
     * @brief Return the immutable generated payload value.
     *
     * @return Payload value supplied at construction.
     * @pre The payload object is alive.
     * @post The payload is unchanged.
     * @invariant Every live Arc clone in an episode must observe this same
     * value through `operator*` and `operator->`.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; the property is
     * single-threaded.
     */
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_;
    }

private:
    tracked_arc_payload(const tracked_arc_payload& other) = delete;
    tracked_arc_payload& operator=(const tracked_arc_payload& other) = delete;

    /// Non-owning pointer to the lifetime ledger.
    destruction_ledger* ledger_;
    /// Immutable payload used to validate dereference commands.
    std::uint64_t value_;
};

/// Arc specialization exercised by this property target.
using arc_type = memsafe::Arc<tracked_arc_payload>;

/**
 * @brief Result of executing one generated Arc command sequence.
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
    /// True when every checked Arc invariant held.
    bool ok = true;
    /// Number of commands generated before prefix truncation.
    std::size_t generated_steps = 0u;
    /// Number of generated commands actually executed for this replay.
    std::size_t executed_steps = 0u;
    /// One-based command index that first exposed a failure, or zero on pass.
    std::size_t failing_step = 0u;
    /// Static diagnostic string naming the violated invariant.
    const char* reason = "no failure";
};

/**
 * @brief Mutable model state for one Arc property sequence replay.
 *
 * @pre Construct a fresh state for every replay seed and prefix.
 * @post Slots own any live `Arc<T>` handles until explicit reset or state
 * destruction.
 * @invariant `live_count` equals the number of non-empty slots in a passing
 * state; `active_ledger` is non-null exactly while the current episode has a
 * positive live count.
 * @throws Construction can throw only if default-constructing an Arc slot
 * throws, which the Arc contract does not do.
 * @note Ownership/thread-safety: all fields are local to one property replay
 * and are not shared across threads.
 */
struct arc_sequence_state final {
    /// Public Arc handles used by the generated clone/drop model.
    std::array<arc_type, k_arc_slot_count> slots{};
    /// Lifetime ledgers for every episode that may be started in this replay.
    std::array<destruction_ledger, k_max_commands_per_sequence + 1u> ledgers{};
    /// Count of ledgers initialized so far.
    std::size_t episode_count = 0u;
    /// Ledger for the currently live payload, or null between episodes.
    destruction_ledger* active_ledger = nullptr;
    /// Payload address every live Arc in the current episode must share.
    tracked_arc_payload* active_payload = nullptr;
    /// Generated payload value expected through every dereference.
    std::uint64_t active_value = 0u;
    /// Modeled number of live Arc handles in `slots`.
    std::size_t live_count = 0u;
};

/**
 * @brief Mark a sequence report as failed if no earlier failure exists.
 *
 * @param report Report to update.
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
                   std::size_t step,
                   const char* reason) noexcept {
    if (report.ok) {
        report.ok = false;
        report.failing_step = step;
        report.reason = reason;
    }
}

/**
 * @brief Find the generated slot for the requested live Arc ordinal.
 *
 * @param state Current Arc sequence model.
 * @param live_ordinal Zero-based ordinal among live slots.
 * @return Slot index for the requested live handle, or `k_arc_slot_count` if
 * no such live slot exists.
 * @pre `state` is the active sequence state.
 * @post No state is modified.
 * @invariant The function treats `Arc<T>::has_value()` as the public liveness
 * observer for model slots.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads local sequence state only.
 */
std::size_t nth_live_slot(const arc_sequence_state& state,
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
 * @brief Find the first empty generated Arc slot.
 *
 * @param state Current Arc sequence model.
 * @return Slot index that is empty, or `k_arc_slot_count` when none is empty.
 * @pre `state` is the active sequence state.
 * @post No state is modified.
 * @invariant Empty slots must report both `has_value() == false` and
 * `strong_count() == 0` in passing states.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads local sequence state only.
 */
std::size_t first_empty_slot(const arc_sequence_state& state) noexcept {
    for (std::size_t index = 0u; index < state.slots.size(); ++index) {
        if (!state.slots[index].has_value()) {
            return index;
        }
    }
    return k_arc_slot_count;
}

/**
 * @brief Validate Arc public strong-count and payload identity invariants.
 *
 * @param state Current Arc sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being checked.
 * @return True when all count and identity invariants hold.
 * @pre `state.live_count` is the model's expected number of live slots.
 * @post `report` is failed if any slot disagrees with the model.
 * @invariant For every live slot, `strong_count()` must equal
 * `state.live_count`; for every empty slot, `strong_count()` must be zero.
 * @throws Nothing.
 * @note Ownership/thread-safety: observes public Arc APIs on one thread.
 */
bool check_arc_count_invariant(arc_sequence_state& state,
                               sequence_report& report,
                               std::size_t step) noexcept {
    std::size_t observed_live = 0u;

    for (std::size_t index = 0u; index < state.slots.size(); ++index) {
        const arc_type& slot = state.slots[index];
        if (slot.has_value()) {
            ++observed_live;
            if (slot.strong_count() !=
                static_cast<std::uint64_t>(state.live_count)) {
                fail_sequence(report,
                              step,
                              "Arc strong_count did not match live model");
            }
            if (state.active_payload == nullptr ||
                slot.get() != state.active_payload) {
                fail_sequence(report,
                              step,
                              "live Arc handles did not share one payload");
            }
        } else if (slot.strong_count() != 0u) {
            fail_sequence(report,
                          step,
                          "empty Arc slot reported a nonzero strong_count");
        }
    }

    if (observed_live != state.live_count) {
        fail_sequence(report, step, "live Arc slot count diverged from model");
    }
    if (state.live_count > 0u && state.active_ledger == nullptr) {
        fail_sequence(report, step, "live Arc episode had no active ledger");
    }
    if (state.live_count > 0u && state.active_ledger != nullptr &&
        state.active_ledger->destroyed != 0u) {
        fail_sequence(report,
                      step,
                      "Arc payload was destroyed before count reached zero");
    }

    return report.ok;
}

/**
 * @brief Validate the final-release destruction invariant for an episode.
 *
 * @param state Current Arc sequence model whose live count just reached zero.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being checked.
 * @return No value.
 * @pre `state.live_count == 0` and `state.active_ledger != nullptr`.
 * @post The active episode pointers are cleared after the destruction check.
 * @invariant Count zero must correspond to exactly one tracked payload
 * destructor and the destructor must observe the generated payload value.
 * @throws Nothing.
 * @note Ownership/thread-safety: checks local ledger state on one thread.
 */
void verify_zero_release(arc_sequence_state& state,
                         sequence_report& report,
                         std::size_t step) noexcept {
    if (state.active_ledger == nullptr) {
        fail_sequence(report, step, "count zero episode had no active ledger");
        return;
    }

    if (state.active_ledger->constructed != 1u ||
        state.active_ledger->destroyed != 1u) {
        fail_sequence(report,
                      step,
                      "Arc payload was not destroyed exactly once at zero");
    }
    if (state.active_ledger->last_destroyed_value != state.active_value) {
        fail_sequence(report,
                      step,
                      "Arc payload destructor observed the wrong value");
    }

    state.active_ledger = nullptr;
    state.active_payload = nullptr;
    state.active_value = 0u;
}

/**
 * @brief Start a fresh Arc episode when no generated Arc handles are live.
 *
 * @param state Current sequence state to initialize.
 * @param rng Random engine supplying the tracked payload value.
 * @param report Report updated on construction or count invariant failure.
 * @param step One-based command prefix that required a fresh episode.
 * @return No value.
 * @pre `state.live_count == 0`.
 * @post Slot zero holds a new `Arc<tracked_arc_payload>` with strong count one
 * when construction succeeds.
 * @invariant A new episode is test setup that lets a longer generated command
 * stream continue after a random drop released the previous final owner.
 * @throws `std::bad_alloc` or propagated payload-construction exceptions from
 * `memsafe::Arc<T>` allocation; the property driver records escaping
 * exceptions with seed metadata.
 * @note Ownership/thread-safety: constructs a payload in local state only.
 */
void begin_arc_episode(arc_sequence_state& state,
                       std::mt19937_64& rng,
                       sequence_report& report,
                       std::size_t step) {
    if (state.live_count != 0u) {
        fail_sequence(report, step, "began Arc episode while handles live");
        return;
    }
    if (state.episode_count >= state.ledgers.size()) {
        fail_sequence(report, step, "too many generated Arc episodes");
        return;
    }

    destruction_ledger& ledger = state.ledgers[state.episode_count];
    ledger = destruction_ledger{};
    ++state.episode_count;

    state.active_ledger = &ledger;
    state.active_value =
        static_cast<std::uint64_t>(bounded_random(rng,
                                                  k_payload_value_modulus));
    state.slots[0] = arc_type(ledger, state.active_value);
    state.active_payload = state.slots[0].get();
    state.live_count = 1u;

    if (state.active_payload == nullptr) {
        fail_sequence(report, step, "Arc construction produced a null payload");
    }
    if (ledger.constructed != 1u || ledger.destroyed != 0u) {
        fail_sequence(report, step, "Arc payload construction ledger mismatch");
    }
    check_arc_count_invariant(state, report, step);
}

/**
 * @brief Draw one Arc command from the deterministic command stream.
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
arc_command draw_arc_command(std::mt19937_64& rng) noexcept {
    switch (bounded_random(rng, 3u)) {
    case 0u:
        return arc_command::clone;
    case 1u:
        return arc_command::drop;
    default:
        return arc_command::deref;
    }
}

/**
 * @brief Execute a generated Arc clone command.
 *
 * @param state Current Arc sequence model.
 * @param rng Random engine used to choose the source live slot.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre At least one generated Arc slot is live and at least one slot is empty.
 * @post One empty slot now shares the selected source payload and the modeled
 * live count has increased by one.
 * @invariant Copying must retain the same payload and increment the shared
 * strong count rather than cloning the payload.
 * @throws `memsafe::violation` under THROW policy if Arc retain overflows;
 * normal property inputs do not approach that limit.
 * @note Ownership/thread-safety: copies Arc handles on one thread only.
 */
void execute_clone(arc_sequence_state& state,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) {
    const std::size_t source_index =
        nth_live_slot(state, bounded_random(rng, state.live_count));
    const std::size_t target_index = first_empty_slot(state);

    if (source_index >= state.slots.size() ||
        target_index >= state.slots.size()) {
        fail_sequence(report, step, "Arc clone command had no valid slot");
        return;
    }

    state.slots[target_index] = state.slots[source_index];
    ++state.live_count;
    check_arc_count_invariant(state, report, step);
}

/**
 * @brief Execute a generated Arc drop command.
 *
 * @param state Current Arc sequence model.
 * @param rng Random engine used to choose the live slot to reset.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre At least one generated Arc slot is live.
 * @post One live slot is empty; if that slot was the final owner, the payload
 * destruction ledger has been checked and the episode is closed.
 * @invariant Resetting a non-final owner must not destroy the payload, while
 * resetting the final owner must destroy it exactly once.
 * @throws Nothing; `Arc<T>::reset()` is `noexcept`.
 * @note Ownership/thread-safety: resets one generated Arc handle on one
 * thread.
 */
void execute_drop(arc_sequence_state& state,
                  std::mt19937_64& rng,
                  sequence_report& report,
                  std::size_t step) noexcept {
    const std::size_t target_index =
        nth_live_slot(state, bounded_random(rng, state.live_count));
    if (target_index >= state.slots.size()) {
        fail_sequence(report, step, "Arc drop command had no live slot");
        return;
    }

    const bool dropping_final_owner = state.live_count == 1u;
    state.slots[target_index].reset();
    --state.live_count;

    if (dropping_final_owner) {
        verify_zero_release(state, report, step);
    }
    check_arc_count_invariant(state, report, step);
}

/**
 * @brief Execute a generated Arc dereference command.
 *
 * @param state Current Arc sequence model.
 * @param rng Random engine used to choose the live slot to dereference.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre At least one generated Arc slot is live.
 * @post No ownership state is intentionally changed.
 * @invariant `get()`, `operator*`, and `operator->` must all reach the same
 * payload object for the selected live handle.
 * @throws Nothing for passing states because the selected slot is non-empty.
 * @note Ownership/thread-safety: dereferences a live Arc on one thread only.
 */
void execute_deref(arc_sequence_state& state,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) noexcept {
    const std::size_t target_index =
        nth_live_slot(state, bounded_random(rng, state.live_count));
    if (target_index >= state.slots.size()) {
        fail_sequence(report, step, "Arc deref command had no live slot");
        return;
    }

    const arc_type& selected = state.slots[target_index];
    if (selected.get() != state.active_payload) {
        fail_sequence(report, step, "Arc::get returned unexpected payload");
    }
    if ((*selected).value() != state.active_value ||
        selected->value() != state.active_value) {
        fail_sequence(report, step, "Arc dereference returned wrong value");
    }
    check_arc_count_invariant(state, report, step);
}

/**
 * @brief Dispatch one generated Arc command.
 *
 * @param command Command kind to execute.
 * @param state Current Arc sequence model.
 * @param rng Random engine owned by the active property sequence.
 * @param report Report updated on the first invariant failure.
 * @param step One-based command prefix being executed.
 * @return No value.
 * @pre A fresh Arc episode has been started when no handles were live before
 * dispatch.
 * @post The command's state transition and invariant checks have run.
 * @invariant Dispatch covers exactly the clone/drop/deref command domain.
 * @throws Propagates only clone-time Arc retain exceptions, which the property
 * driver converts to failure metadata.
 * @note Ownership/thread-safety: all state belongs to the current replay.
 */
void execute_arc_command(arc_command command,
                         arc_sequence_state& state,
                         std::mt19937_64& rng,
                         sequence_report& report,
                         std::size_t step) {
    switch (command) {
    case arc_command::clone:
        execute_clone(state, rng, report, step);
        break;
    case arc_command::drop:
        execute_drop(state, rng, report, step);
        break;
    case arc_command::deref:
        execute_deref(state, rng, report, step);
        break;
    }
}

/**
 * @brief Drop every remaining live Arc handle at replay cleanup.
 *
 * @param state Current Arc sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step Prefix length associated with cleanup diagnostics.
 * @return No value.
 * @pre `state` may have zero or more live Arc handles.
 * @post No generated Arc handle remains live when no invariant failure stops
 * cleanup.
 * @invariant Cleanup is part of every replay so final-release destruction is
 * checked even when the random stream did not drop the last owner.
 * @throws Nothing; `Arc<T>::reset()` is `noexcept`.
 * @note Ownership/thread-safety: resets local Arc handles on one thread.
 */
void cleanup_arc_sequence(arc_sequence_state& state,
                          sequence_report& report,
                          std::size_t step) noexcept {
    while (report.ok && state.live_count > 0u) {
        const std::size_t target_index = nth_live_slot(state, 0u);
        if (target_index >= state.slots.size()) {
            fail_sequence(report, step, "Arc cleanup had no live slot");
            return;
        }

        const bool dropping_final_owner = state.live_count == 1u;
        state.slots[target_index].reset();
        --state.live_count;
        if (dropping_final_owner) {
            verify_zero_release(state, report, step);
        }
        check_arc_count_invariant(state, report, step);
    }
}

/**
 * @brief Validate all episode ledgers after replay cleanup.
 *
 * @param state Completed Arc sequence model.
 * @param report Report updated on the first invariant failure.
 * @param step Prefix length associated with final diagnostics.
 * @return No value.
 * @pre Cleanup has dropped every live generated Arc handle.
 * @post `report` is failed if any episode missed or duplicated destruction.
 * @invariant Every episode constructs exactly one payload and reaches exactly
 * one payload destructor before the sequence returns.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads local ledger state only.
 */
void check_all_payloads_destroyed_once(const arc_sequence_state& state,
                                       sequence_report& report,
                                       std::size_t step) noexcept {
    for (std::size_t index = 0u; index < state.episode_count; ++index) {
        const destruction_ledger& ledger = state.ledgers[index];
        if (ledger.constructed != 1u || ledger.destroyed != 1u) {
            fail_sequence(report,
                          step,
                          "Arc episode did not end with one destruction");
            return;
        }
    }
}

/**
 * @brief Execute one deterministic generated Arc command sequence.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @param prefix_limit Maximum number of generated commands to execute, or
 * `k_full_sequence_prefix` to execute the full generated sequence.
 * @return Sequence report containing pass/fail status and replay metadata.
 * @pre `rng` is the per-sequence engine supplied by
 * `memsafe::test_support::property::run_sequences`, or another engine seeded
 * with the reported failing sequence seed.
 * @post All live Arc handles created during the replay have been reset before
 * the function returns unless an invariant failure stopped execution early.
 * @invariant The generated operation stream contains only clone, drop, and
 * dereference commands; fresh episodes are setup that occurs only after a drop
 * command has released the previous final owner.
 * @throws `std::bad_alloc` from Arc allocation or diagnostic string allocation.
 * The property driver catches escaping exceptions and records seed metadata.
 * @note Ownership/thread-safety: the sequence is single-threaded by design.
 */
sequence_report run_arc_sequence(std::mt19937_64& rng,
                                 std::size_t prefix_limit) {
    sequence_report report;
    report.generated_steps =
        1u + bounded_random(rng, k_max_commands_per_sequence);
    report.executed_steps = report.generated_steps < prefix_limit
                                ? report.generated_steps
                                : prefix_limit;

    arc_sequence_state state;
    begin_arc_episode(state, rng, report, 0u);

    for (std::size_t step = 1u; report.ok && step <= report.executed_steps;
         ++step) {
        /*
         * F2 classifies Arc as a stateful property target. If a generated drop
         * has released the final owner, a fresh episode lets the remaining
         * command prefix continue to exercise clone/drop/deref transitions.
         */
        if (state.live_count == 0u) {
            begin_arc_episode(state, rng, report, step);
        }
        if (!report.ok) {
            break;
        }

        const arc_command command = draw_arc_command(rng);
        execute_arc_command(command, state, rng, report, step);
    }

    if (report.ok) {
        cleanup_arc_sequence(state, report, report.executed_steps);
    }
    if (report.ok) {
        check_all_payloads_destroyed_once(state,
                                          report,
                                          report.executed_steps);
    }

    return report;
}

/**
 * @brief Replay one generated Arc sequence from a deterministic sequence seed.
 *
 * @param seed Seed reported by `property_driver.hpp` for a specific sequence.
 * @param prefix_limit Command prefix to execute before cleanup.
 * @return Sequence report for the replayed prefix.
 * @pre `seed` is the per-sequence seed, not the property run base seed.
 * @post No global state is modified.
 * @invariant Replays use the same command generator as the property body.
 * @throws `std::bad_alloc` under the same conditions as `run_arc_sequence`.
 * @note Ownership/thread-safety: all replay state is local to the call.
 */
sequence_report replay_arc_sequence(std::uint64_t seed,
                                    std::size_t prefix_limit) {
    std::mt19937_64 rng(seed);
    return run_arc_sequence(rng, prefix_limit);
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
 * @throws `std::bad_alloc` if a replay allocation fails.
 * @note Ownership/thread-safety: all replay attempts are independent.
 */
std::size_t find_minimal_failing_prefix(std::uint64_t seed,
                                        std::size_t original_steps) {
    for (std::size_t prefix = 0u; prefix <= original_steps; ++prefix) {
        if (!replay_arc_sequence(seed, prefix).ok) {
            return prefix;
        }
    }
    return original_steps;
}

/**
 * @brief Build a shrink note for a failing Arc sequence.
 *
 * @param report Failed sequence report.
 * @return Human-readable note containing the invariant and first failing step.
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
 * @brief Check a completed Arc property result against F2 thresholds.
 *
 * @param result Result returned by `property::run_sequences`.
 * @return No value.
 * @pre `result` was produced by this translation unit's stateful Arc property
 * configuration.
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
 * @brief Run the `memsafe::Arc<T>` stateful property test.
 *
 * @retval 0 All generated Arc command sequences satisfied the strong-count and
 * final-destruction invariants.
 * @retval 1 One or more harness checks failed, with replay seed and minimal
 * prefix reported for property failures.
 * @pre The executable is run by the F4 property lane, which discovers
 * `testing/tests/property/*.cpp` and defines any lane-specific nightly
 * property macros before including `property_driver.hpp`.
 * @post The process exit code is the test verdict consumed by CTest.
 * @invariant One named stateful property target executes for `Arc<T>` and
 * receives the property driver's full stateful threshold.
 * @throws The test does not intentionally throw. Unexpected allocation
 * failures inside the property body are caught by `property_driver.hpp` and
 * surfaced as property failures with seed metadata.
 * @note Ownership/thread-safety: all test state is automatic storage or
 * property-driver local state; no worker threads are started.
 */
int main() {
    namespace prop = memsafe::test_support::property;

    prop::run_config config =
        prop::make_config("arc.clone-drop-deref.refcount",
                          prop::workload::stateful,
                          0x0535a1u);

    const prop::run_result result = prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const sequence_report report =
                run_arc_sequence(rng, k_full_sequence_prefix);
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

    RUN_TESTS("test_arc_property");
}
