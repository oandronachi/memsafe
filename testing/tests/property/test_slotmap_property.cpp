/**
 * @file test_slotmap_property.cpp
 * @brief Stateful property test for `memsafe::SlotMap<T>` handle lifetimes.
 *
 * @details
 * Work package: CPP_MEMSAFE-0220-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0205-FUNC bounded `SlotMap<T>`
 *   allocator with generated allocate, deallocate, live dereference, stale
 *   dereference, and duplicate-deallocate command sequences.
 * - Use the CPP_MEMSAFE-0035-TEST `property_driver.hpp` stateful threshold so
 *   the property lane executes at least 1 000 generated command sequences per
 *   normal run and receives the documented 10x multiplier under nightly macros.
 * - Report the deterministic replay seed and smallest failing command prefix
 *   known to reproduce any SlotMap invariant failure.
 *
 * Key invariants:
 * - Every handle in the generated live model dereferences successfully and
 *   reads the payload id/value originally allocated for that handle.
 * - Every stale handle produced by deallocation is rejected immediately, after
 *   same-index slot reuse, during generated stale-dereference commands, and
 *   after final cleanup.
 * - The public `SlotMap::size()` observer equals the generated live-handle
 *   count after every command.
 * - After generated cleanup, a full-capacity refill must allocate every freed
 *   slot index exactly once, then reject one additional allocation without
 *   constructing or destroying a payload.
 * - Each constructed payload is destroyed exactly once before the sequence
 *   ends; map teardown after cleanup must not destroy an additional payload.
 *
 * Ownership and thread-safety:
 * - All generated state is owned by one test process and one test thread.
 * - The property deliberately avoids cross-thread access; Treiber-stack
 *   interleavings belong to CPP_MEMSAFE-0240-TEST, while this package verifies
 *   the single-threaded stateful lifetime contract required by F2/F3.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force runtime generation checks for the SlotMap property test.
 *
 * @retval 1 Enables stale-handle checks even if an outer release lane supplied
 * `MEMSAFE_RELEASE_CHECKS=0`.
 * @pre Define before including any memsafe feature header.
 * @post `SlotMap<T>::get`, `SlotMap<T>::deref`, and `SlotMap<T>::deallocate`
 * report stale handles through the configured violation policy.
 * @invariant CPP_MEMSAFE-0220-TEST asserts stale handles are always detected,
 * so this translation unit cannot inherit a check-free release configuration.
 * @throws Nothing directly; the macro affects later inline checks.
 * @note Ownership/thread-safety: preprocessor configuration only, with no
 * runtime storage.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception reporting for generated stale-handle probes.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs memsafe violation reports to throw
 * `memsafe::violation`.
 * @pre Define before including `<memsafe/handle.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post The property can assert stale-handle detection in-process without
 * aborting the test executable.
 * @invariant Only generated stale operations are expected to throw; live
 * operations must complete normally.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: compile-time selection only. No handler slot
 * is installed by this test.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../../test_harness.hpp"

#include "../support/property_driver.hpp"

#include <memsafe/handle.hpp>
#include <memsafe/violation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

/// Compile-time SlotMap capacity used by generated stateful sequences.
constexpr std::size_t k_slot_capacity = 8u;

/// Maximum generated command count for one SlotMap property sequence.
constexpr std::size_t k_max_commands_per_sequence = 48u;

/// Payload id storage bound for generated creates plus the cleanup refill probe.
constexpr std::size_t k_max_payload_records =
    k_max_commands_per_sequence + k_slot_capacity;

/// Exclusive upper bound for generated payload values.
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
 * @brief Generate a deterministic payload value for a new slot.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return Unsigned payload value stored in the next tracked object.
 * @pre `rng` is the deterministic engine supplied to the sequence.
 * @post `rng` has advanced by one draw.
 * @invariant Values are bounded so failure diagnostics remain readable while
 * still changing often enough to expose wrong-slot dereferences.
 * @throws Nothing.
 * @note Ownership/thread-safety: local deterministic random generation only.
 */
std::uint64_t draw_payload(std::mt19937_64& rng) noexcept {
    return rng() % k_payload_value_modulus;
}

/**
 * @brief Mutable construction/destruction ledger for one property sequence.
 *
 * @details
 * Payload objects record their construction id and destruction id here. The
 * ledger outlives the `SlotMap` instance under test, allowing the property to
 * detect payload leaks, duplicate destruction, and unexpected map-destructor
 * cleanup after all model-live handles have already been deallocated.
 *
 * @pre The ledger must outlive every `tracked_payload` constructed with it.
 * @post Direct field updates represent observed payload lifetime events for
 * exactly one generated command sequence.
 * @invariant For a passing sequence, each constructed id in
 * `constructed_ids` has exactly one matching entry in `destroyed_ids`,
 * `constructed == destroyed`, and none of the error flags are set.
 * @throws Nothing; fixed-size arrays and scalar counters only.
 * @note Ownership/thread-safety: owned by one property sequence and accessed on
 * the single test thread.
 */
struct slot_lifetime_ledger final {
    /// Number of tracked payload constructors observed.
    std::size_t constructed = 0u;
    /// Number of tracked payload destructors observed.
    std::size_t destroyed = 0u;
    /// True when construction used an out-of-range or duplicate payload id.
    bool construction_error = false;
    /// True when destruction used an out-of-range or never-constructed id.
    bool unexpected_destroy = false;
    /// True when one payload id was destroyed more than once.
    bool duplicate_destroy = false;
    /// Per-id construction markers.
    std::array<bool, k_max_payload_records> constructed_ids{};
    /// Per-id destruction markers.
    std::array<bool, k_max_payload_records> destroyed_ids{};

    /**
     * @brief Record one tracked payload construction.
     *
     * @param id Payload id assigned by the generated sequence.
     * @return No value.
     * @pre `id` should be smaller than `k_max_payload_records` and unused.
     * @post `constructed` has increased by one; either the id is marked as
     * constructed or `construction_error` records an invalid construction.
     * @invariant Payload ids are intended to be unique within one generated
     * sequence, making duplicate construction a model or allocator failure.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutates sequence-local ledger state only.
     */
    void record_construction(std::size_t id) noexcept {
        ++constructed;
        if (id >= constructed_ids.size() || constructed_ids[id]) {
            construction_error = true;
            return;
        }
        constructed_ids[id] = true;
    }

    /**
     * @brief Record one tracked payload destruction.
     *
     * @param id Payload id stored in the object whose destructor is running.
     * @return No value.
     * @pre The id should have been recorded by `record_construction` exactly
     * once and not yet destroyed.
     * @post `destroyed` has increased by one; duplicate or unexpected ids set
     * the corresponding error flag.
     * @invariant A passing SlotMap deallocation path cannot destroy an id twice
     * and cannot destroy storage that was never constructed.
     * @throws Nothing.
     * @note Ownership/thread-safety: called only by generated payload
     * destructors on the single test thread.
     */
    void record_destruction(std::size_t id) noexcept {
        ++destroyed;
        if (id >= destroyed_ids.size() || !constructed_ids[id]) {
            unexpected_destroy = true;
            return;
        }
        if (destroyed_ids[id]) {
            duplicate_destroy = true;
            return;
        }
        destroyed_ids[id] = true;
    }

    /**
     * @brief Report whether any lifetime-accounting error flag is set.
     *
     * @retval true Construction, unexpected-destruction, or duplicate-destroy
     * state has been observed.
     * @retval false No direct ledger error has been observed.
     * @pre The ledger is alive.
     * @post No state is modified.
     * @invariant This helper does not treat not-yet-destroyed live ids as an
     * error; final leak detection is handled by
     * `all_constructed_destroyed_once`.
     * @throws Nothing.
     * @note Ownership/thread-safety: reads sequence-local scalar flags only.
     */
    bool has_error() const noexcept {
        return construction_error || unexpected_destroy || duplicate_destroy;
    }

    /**
     * @brief Validate final no-leak and no-double-destroy accounting.
     *
     * @retval true Every constructed payload id has been destroyed exactly once.
     * @retval false At least one payload leaked, was destroyed twice, or used an
     * invalid construction/destruction id.
     * @pre All generated live handles have been deallocated before this check.
     * @post No state is modified.
     * @invariant Passing final state has equal construction and destruction
     * counts and identical per-id construction/destruction marker sets.
     * @throws Nothing.
     * @note Ownership/thread-safety: reads fixed sequence-local arrays only.
     */
    bool all_constructed_destroyed_once() const noexcept {
        if (has_error() || constructed != destroyed) {
            return false;
        }

        for (std::size_t id = 0u; id < constructed_ids.size(); ++id) {
            if (constructed_ids[id] != destroyed_ids[id]) {
                return false;
            }
        }

        return true;
    }
};

/**
 * @brief Payload object stored in generated SlotMap slots.
 *
 * @details
 * The payload carries a unique id, a generated value, and a non-owning pointer
 * to the sequence ledger. Construction and destruction update that ledger so
 * the property can detect leaked slots and double-free behavior through public
 * `SlotMap` operations.
 *
 * Example:
 * @code
 * slot_lifetime_ledger ledger;
 * tracked_payload payload(ledger, 0u, 42u);
 * CHECK(payload.value() == 42u);
 * @endcode
 *
 * @pre The supplied ledger outlives the payload object.
 * @post Construction records one id; destruction records the same id exactly
 * once for a correct lifetime.
 * @invariant `ledger_`, `id_`, and `value_` remain stable for the object's
 * lifetime.
 * @throws Nothing; construction, observation, and destruction are `noexcept`.
 * @note Ownership/thread-safety: instances are owned exclusively by one
 * `SlotMap` slot and used on one test thread.
 */
class tracked_payload final {
public:
    /**
     * @brief Construct a payload and record its construction id.
     *
     * @param ledger Sequence ledger that records this object's lifetime.
     * @param id Unique payload id assigned by the generated model.
     * @param value Generated value expected through live-handle dereferences.
     * @return Constructors do not return a value.
     * @pre `ledger` remains alive until this payload is destroyed.
     * @post `ledger.constructed` has increased by one and this object stores
     * `id` plus `value`.
     * @invariant Construction performs no allocation, keeping property failures
     * focused on SlotMap storage and generation behavior.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes only the receiving object and
     * the caller-owned sequence ledger.
     */
    tracked_payload(slot_lifetime_ledger& ledger,
                    std::size_t id,
                    std::uint64_t value) noexcept
        : ledger_(&ledger),
          id_(id),
          value_(value) {
        ledger_->record_construction(id_);
    }

    /**
     * @brief Destroy the payload and record its destruction id.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post The ledger has one additional destruction event for `id_`.
     * @invariant The destructor is allocation-free and `noexcept`, so property
     * failures cannot be hidden by payload teardown exceptions.
     * @throws Nothing.
     * @note Ownership/thread-safety: invoked by `SlotMap` during generated
     * single-threaded deallocation or map teardown.
     */
    ~tracked_payload() noexcept {
        ledger_->record_destruction(id_);
    }

    /**
     * @brief Return the unique payload id assigned by the generated model.
     *
     * @return Payload id.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The id remains stable and lets the property distinguish slot
     * reuse from accidental stale-handle access to a new payload.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; the caller owns ordinary
     * synchronization. The property uses it on one thread.
     */
    [[nodiscard]] std::size_t id() const noexcept {
        return id_;
    }

    /**
     * @brief Return the generated payload value.
     *
     * @return Value supplied during allocation.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The value is immutable for the lifetime of this payload.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; the property uses it on one
     * thread through a live handle.
     */
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_;
    }

private:
    tracked_payload(const tracked_payload&) = delete;
    tracked_payload& operator=(const tracked_payload&) = delete;
    tracked_payload(tracked_payload&&) = delete;
    tracked_payload& operator=(tracked_payload&&) = delete;

    slot_lifetime_ledger* ledger_;
    std::size_t id_;
    std::uint64_t value_;
};

/// SlotMap specialization exercised by this stateful property test.
using slot_map_type = memsafe::SlotMap<tracked_payload, k_slot_capacity>;

/// Handle type issued by the tested SlotMap specialization.
using handle_type = slot_map_type::handle_type;

/// Slot index component type used for same-index stale-handle probes.
using index_type = handle_type::index_type;

/**
 * @brief Command kinds generated for the SlotMap state machine.
 *
 * @pre Values are produced only by `draw_command`.
 * @post Dispatching a command mutates the generated live/stale model or is a
 * documented no-op when the required model state is absent.
 * @invariant The command set covers the CPP_MEMSAFE-0220-TEST surface:
 * allocation, live deallocation, live dereference, stale dereference, and
 * duplicate stale deallocation.
 * @throws Nothing; enum values own no resources.
 * @note Ownership/thread-safety: values are local to one property sequence.
 */
enum class command_kind {
    /// Allocate a new slot, or check capacity exhaustion when the map is full.
    allocate,
    /// Deallocate one generated live handle.
    deallocate_live,
    /// Dereference one generated live handle.
    dereference_live,
    /// Dereference one generated stale handle and require rejection.
    dereference_stale,
    /// Attempt to deallocate one stale handle and require rejection.
    deallocate_stale
};

/**
 * @brief Draw a generated SlotMap command from the deterministic engine.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return One command kind from the SlotMap command set.
 * @pre `rng` is the deterministic engine supplied to the sequence.
 * @post `rng` has advanced by one draw.
 * @invariant The switch arm count mirrors the `command_kind` enumerators so
 * every state transition has a stable draw path.
 * @throws Nothing.
 * @note Ownership/thread-safety: local deterministic random generation only.
 */
command_kind draw_command(std::mt19937_64& rng) noexcept {
    switch (bounded_random(rng, 5u)) {
        case 0u:
            return command_kind::allocate;
        case 1u:
            return command_kind::deallocate_live;
        case 2u:
            return command_kind::dereference_live;
        case 3u:
            return command_kind::dereference_stale;
        default:
            return command_kind::deallocate_stale;
    }
}

/**
 * @brief Model record for one currently live SlotMap handle.
 *
 * @pre `handle` was returned by the map under test and has not been
 * deallocated by the generated model.
 * @post The aggregate owns only scalar replay data and the non-owning handle
 * value.
 * @invariant `id` and `value` must match the `tracked_payload` observed through
 * `handle` while the record remains in the live model.
 * @throws Nothing; the aggregate owns no dynamic resources.
 * @note Ownership/thread-safety: owned by one property sequence and used on one
 * thread.
 */
struct live_slot_record final {
    /// Handle currently expected to dereference successfully.
    handle_type handle{};
    /// Payload id associated with the live handle.
    std::size_t id = 0u;
    /// Payload value associated with the live handle.
    std::uint64_t value = 0u;
};

/**
 * @brief Captured outcome of an operation expected to report a violation.
 *
 * @pre A default-constructed capture represents an operation that did not
 * throw.
 * @post Test helpers fill the fields after invoking one operation.
 * @invariant `kind` is meaningful only when `threw_memsafe_violation == true`.
 * @throws Nothing; the aggregate owns no dynamic resources.
 * @note Ownership/thread-safety: stack-owned by one synchronous property
 * operation.
 */
struct violation_capture final {
    /// True when the operation threw `memsafe::violation`.
    bool threw_memsafe_violation = false;
    /// True when the operation threw a non-memsafe exception type.
    bool threw_unexpected_exception = false;
    /// Violation kind observed from `memsafe::violation::kind()`.
    memsafe::violation_kind kind = memsafe::violation_kind::null_access;
};

/**
 * @brief Mutable model and public SlotMap object for one generated sequence.
 *
 * @pre `ledger` and `map` references outlive this model object.
 * @post Vectors contain all generated live and stale handles for the active
 * sequence.
 * @invariant `map.size()` must match `live.size()` after each completed
 * command, and every handle in `stale` must be rejected by SlotMap lookup.
 * @throws Vector growth can throw `std::bad_alloc`; the property driver
 * converts escaping allocation failures into deterministic failures with seed
 * metadata.
 * @note Ownership/thread-safety: all state is stack-owned by one generated
 * sequence and accessed on one thread.
 */
struct sequence_state final {
    /// Lifetime ledger updated by payload constructors and destructors.
    slot_lifetime_ledger& ledger;
    /// SlotMap instance under test.
    slot_map_type& map;
    /// Handles still expected to name live payloads.
    std::vector<live_slot_record> live;
    /// Handles already deallocated by the generated model.
    std::vector<handle_type> stale;
    /// Next unique payload id assigned on successful allocation.
    std::size_t next_payload_id = 0u;
};

/**
 * @brief Result of executing one generated SlotMap command sequence.
 *
 * @pre Default construction represents a passing sequence before any commands
 * have been executed.
 * @post Fields identify the first detected invariant failure, if any, plus a
 * compact state snapshot for replay diagnostics.
 * @invariant `ok == false` implies `reason` names the violated invariant and
 * `failing_step` identifies the command prefix or cleanup step that exposed it.
 * @throws Nothing; the aggregate owns no dynamic storage.
 * @note Ownership/thread-safety: owned by one property sequence.
 */
struct sequence_report final {
    /// True when every checked SlotMap invariant held.
    bool ok = true;
    /// Number of commands generated before prefix truncation.
    std::size_t generated_steps = 0u;
    /// Number of generated commands actually executed for this replay.
    std::size_t executed_steps = 0u;
    /// One-based command index that first exposed a failure, or zero on pass.
    std::size_t failing_step = 0u;
    /// Static diagnostic string naming the violated invariant.
    const char* reason = "no failure";
    /// Live-handle count at the first failure snapshot.
    std::size_t live_handles = 0u;
    /// Stale-handle count at the first failure snapshot.
    std::size_t stale_handles = 0u;
    /// Public `SlotMap::size()` value at the first failure snapshot.
    std::size_t map_size = 0u;
    /// Ledger construction count at the first failure snapshot.
    std::size_t constructed = 0u;
    /// Ledger destruction count at the first failure snapshot.
    std::size_t destroyed = 0u;
};

/**
 * @brief Snapshot generated state into a sequence report.
 *
 * @param report Report to update.
 * @param state Generated sequence state to observe.
 * @return No value.
 * @pre `state.map` and `state.ledger` are alive.
 * @post The report carries a compact count snapshot for diagnostics.
 * @invariant Snapshotting reads only public SlotMap state and sequence-owned
 * model containers; it never inspects private allocator internals.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads single-threaded sequence state only.
 */
void snapshot_state(sequence_report& report,
                    const sequence_state& state) noexcept {
    report.live_handles = state.live.size();
    report.stale_handles = state.stale.size();
    report.map_size = state.map.size();
    report.constructed = state.ledger.constructed;
    report.destroyed = state.ledger.destroyed;
}

/**
 * @brief Snapshot post-teardown ledger state into a sequence report.
 *
 * @param report Report to update.
 * @param ledger Sequence lifetime ledger to observe.
 * @return No value.
 * @pre The SlotMap under test has already been destroyed.
 * @post The report carries final ledger counts and zero live map/model counts.
 * @invariant Any destructor activity after explicit cleanup is treated as a
 * leak or duplicate-destroy symptom because no model-live handles remain.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads single-threaded ledger state only.
 */
void snapshot_teardown(sequence_report& report,
                       const slot_lifetime_ledger& ledger) noexcept {
    report.live_handles = 0u;
    report.stale_handles = 0u;
    report.map_size = 0u;
    report.constructed = ledger.constructed;
    report.destroyed = ledger.destroyed;
}

/**
 * @brief Mark a sequence report as failed if it has not failed already.
 *
 * @param report Report to update.
 * @param state Generated sequence state to snapshot.
 * @param step One-based command index or cleanup prefix associated with the
 * failure.
 * @param reason Static diagnostic string naming the violated invariant.
 * @return No value.
 * @pre `reason != nullptr` and `state` is alive.
 * @post The first failure is preserved and later failures do not overwrite it.
 * @invariant A sequence reports only the earliest observed invariant failure,
 * keeping shrink diagnostics stable across replays.
 * @throws Nothing.
 * @note Ownership/thread-safety: mutates caller-owned local report state.
 */
void fail_sequence(sequence_report& report,
                   const sequence_state& state,
                   std::size_t step,
                   const char* reason) noexcept {
    if (report.ok) {
        report.ok = false;
        report.failing_step = step;
        report.reason = reason;
        snapshot_state(report, state);
    }
}

/**
 * @brief Mark a post-teardown sequence report as failed if still passing.
 *
 * @param report Report to update.
 * @param ledger Final sequence ledger to snapshot.
 * @param step Command prefix associated with cleanup.
 * @param reason Static diagnostic string naming the violated invariant.
 * @return No value.
 * @pre `reason != nullptr` and the SlotMap under test has been destroyed.
 * @post The first failure is preserved and later failures do not overwrite it.
 * @invariant Post-teardown failures are reserved for leak/double-destroy
 * symptoms observable only after the map destructor has run.
 * @throws Nothing.
 * @note Ownership/thread-safety: mutates caller-owned local report state.
 */
void fail_after_teardown(sequence_report& report,
                         const slot_lifetime_ledger& ledger,
                         std::size_t step,
                         const char* reason) noexcept {
    if (report.ok) {
        report.ok = false;
        report.failing_step = step;
        report.reason = reason;
        snapshot_teardown(report, ledger);
    }
}

/**
 * @brief Run an operation and capture its violation outcome.
 *
 * @tparam Operation Callable type invocable with no arguments.
 * @param operation Operation expected to trigger a memsafe violation.
 * @return Captured memsafe violation kind or unexpected-exception marker.
 * @pre `operation` is callable exactly once and any referenced test objects
 * outlive this call.
 * @post The operation has been invoked. No exception escapes this helper.
 * @invariant The helper does not synthesize violations; it records only the
 * result of public `SlotMap<T>` operations.
 * @throws Nothing; all exceptions from `operation` are caught.
 * @note Ownership/thread-safety: the callable and captures are used
 * synchronously on the calling thread.
 *
 * Example:
 * @code
 * violation_capture capture = capture_violation([&map, stale] {
 *     (void)map.deref(stale);
 * });
 * @endcode
 */
template <typename Operation>
violation_capture capture_violation(Operation&& operation) noexcept {
    violation_capture capture;
    try {
        operation();
    } catch (const memsafe::violation& caught) {
        capture.threw_memsafe_violation = true;
        capture.kind = caught.kind();
    } catch (...) {
        capture.threw_unexpected_exception = true;
    }
    return capture;
}

/**
 * @brief Report whether a capture represents stale-handle detection.
 *
 * @param capture Captured operation outcome to inspect.
 * @retval true The operation threw a memsafe stale-handle class violation.
 * @retval false The operation succeeded, threw an unexpected exception, or
 * reported an unrelated memsafe violation kind.
 * @pre `capture` was produced by `capture_violation`.
 * @post No state is modified.
 * @invariant SlotMap currently reports stale generation and duplicate-free
 * paths as `use_after_free`; `generation_mismatch` is also accepted as an
 * equivalent stale-handle classification if the policy vocabulary is refined.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads immutable capture data only.
 */
bool is_stale_detection(const violation_capture& capture) noexcept {
    if (!capture.threw_memsafe_violation ||
        capture.threw_unexpected_exception) {
        return false;
    }

    return capture.kind == memsafe::violation_kind::use_after_free ||
           capture.kind == memsafe::violation_kind::generation_mismatch;
}

/**
 * @brief Report whether a capture represents capacity exhaustion.
 *
 * @param capture Captured operation outcome to inspect.
 * @retval true The operation threw `violation_kind::capacity_exhausted`.
 * @retval false The operation succeeded, threw unexpectedly, or reported a
 * different memsafe violation kind.
 * @pre `capture` was produced by `capture_violation`.
 * @post No state is modified.
 * @invariant A full bounded SlotMap must reject extra allocations without
 * constructing or publishing a new live slot.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads immutable capture data only.
 */
bool is_capacity_exhaustion(const violation_capture& capture) noexcept {
    return capture.threw_memsafe_violation &&
           !capture.threw_unexpected_exception &&
           capture.kind == memsafe::violation_kind::capacity_exhausted;
}

/**
 * @brief Require that a stale handle is rejected by dereference.
 *
 * @param map SlotMap instance under test.
 * @param stale Handle already deallocated by the generated model.
 * @retval true `map.deref(stale)` reported a stale-handle violation.
 * @retval false Dereference succeeded or reported an unrelated failure.
 * @pre `stale` is no longer in the generated live model for `map`.
 * @post The map is unchanged unless the implementation under test is broken.
 * @invariant The property probes the public dereference path, not private slot
 * state, because CPP_MEMSAFE-0220-TEST requires stale handles to be detected
 * by the shipped API surface.
 * @throws Nothing intentionally; all operation exceptions are captured.
 * @note Ownership/thread-safety: executes synchronously on one test thread.
 */
bool stale_deref_is_detected(slot_map_type& map,
                             handle_type stale) noexcept {
    const violation_capture capture = capture_violation([&map, stale] {
        const tracked_payload& payload = map.deref(stale);
        (void)payload;
    });
    return is_stale_detection(capture);
}

/**
 * @brief Require that a stale handle is rejected by duplicate deallocation.
 *
 * @param map SlotMap instance under test.
 * @param stale Handle already deallocated by the generated model.
 * @retval true `map.deallocate(stale)` reported a stale-handle violation.
 * @retval false Deallocation succeeded or reported an unrelated failure.
 * @pre `stale` is no longer in the generated live model for `map`.
 * @post The map is unchanged unless the implementation under test is broken.
 * @invariant Duplicate deallocation must not destroy another payload, decrement
 * size, or push the slot index a second time.
 * @throws Nothing intentionally; all operation exceptions are captured.
 * @note Ownership/thread-safety: executes synchronously on one test thread.
 */
bool stale_deallocate_is_detected(slot_map_type& map,
                                  handle_type stale) noexcept {
    const violation_capture capture = capture_violation([&map, stale] {
        map.deallocate(stale);
    });
    return is_stale_detection(capture);
}

/**
 * @brief Remove one live model record by swapping it with the vector tail.
 *
 * @param state Generated sequence state to mutate.
 * @param index Index of the live record to remove.
 * @return Removed record.
 * @pre `index < state.live.size()`.
 * @post The selected record has been removed and the vector remains compact.
 * @invariant Order of live records is not semantically meaningful to the
 * property, so swap-pop removal keeps command execution deterministic without
 * preserving insertion order.
 * @throws Nothing under the reserved-vector property setup.
 * @note Ownership/thread-safety: mutates sequence-local vector state only.
 */
live_slot_record remove_live_at(sequence_state& state,
                                std::size_t index) noexcept {
    const live_slot_record removed = state.live[index];
    state.live[index] = state.live.back();
    state.live.pop_back();
    return removed;
}

/**
 * @brief Verify one live handle dereferences to its model payload.
 *
 * @param state Generated sequence state containing the map under test.
 * @param record Live handle record to verify.
 * @retval true Dereference succeeded and observed the expected id/value.
 * @retval false Dereference threw or returned the wrong payload.
 * @pre `record` is present in `state.live`.
 * @post No model state is modified.
 * @invariant A live handle must never be rejected as stale and must not resolve
 * to a different reused slot.
 * @throws Nothing intentionally; all unexpected exceptions are converted to
 * `false`.
 * @note Ownership/thread-safety: reads one sequence-local SlotMap and model
 * record on the test thread.
 */
bool live_record_dereferences_ok(sequence_state& state,
                                 const live_slot_record& record) noexcept {
    try {
        const tracked_payload& payload = state.map.deref(record.handle);
        return payload.id() == record.id && payload.value() == record.value;
    } catch (...) {
        return false;
    }
}

/**
 * @brief Validate all generated live handles through public dereference.
 *
 * @param state Generated sequence state to inspect.
 * @param report Report updated on the first failure.
 * @param step One-based command index associated with the check.
 * @return No value.
 * @pre `state.map` is alive.
 * @post The report is failed if any live handle rejects or reads wrong data.
 * @invariant This is the primary live-handle property required by
 * CPP_MEMSAFE-0220-TEST: live handles dereference successfully after every
 * generated state transition.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local state only.
 */
void assert_live_handles_ok(sequence_state& state,
                            sequence_report& report,
                            std::size_t step) noexcept {
    if (!report.ok) {
        return;
    }

    for (const live_slot_record& record : state.live) {
        if (!live_record_dereferences_ok(state, record)) {
            fail_sequence(report,
                          state,
                          step,
                          "live SlotMap handle did not dereference correctly");
            return;
        }
    }
}

/**
 * @brief Validate public size and lifetime-ledger accounting.
 *
 * @param state Generated sequence state to inspect.
 * @param report Report updated on the first failure.
 * @param step One-based command index associated with the check.
 * @return No value.
 * @pre `state.map` and `state.ledger` are alive.
 * @post The report is failed if public size or payload lifetime accounting is
 * inconsistent with the generated model.
 * @invariant `SlotMap::size()` must track model-live handles, and the ledger
 * must not observe duplicate or unexpected destructor calls.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local state only.
 */
void assert_accounting_ok(const sequence_state& state,
                          sequence_report& report,
                          std::size_t step) noexcept {
    if (!report.ok) {
        return;
    }

    if (state.map.size() != state.live.size()) {
        fail_sequence(report,
                      state,
                      step,
                      "SlotMap::size did not match generated live handles");
        return;
    }

    if (state.ledger.has_error()) {
        fail_sequence(report,
                      state,
                      step,
                      "payload ledger observed duplicate or invalid lifetime");
    }
}

/**
 * @brief Validate all per-step invariants after a generated command.
 *
 * @param state Generated sequence state to inspect.
 * @param report Report updated on the first failure.
 * @param step One-based command index associated with the check.
 * @return No value.
 * @pre `state` is alive and the command for `step` has completed or failed.
 * @post The report reflects any live-deref, size, or lifetime-accounting
 * invariant failure.
 * @invariant Per-step validation stays on public SlotMap APIs plus the
 * test-owned ledger; private free-list internals are never inspected.
 * @throws Nothing.
 * @note Ownership/thread-safety: single-threaded property validation only.
 */
void assert_step_invariants(sequence_state& state,
                            sequence_report& report,
                            std::size_t step) noexcept {
    assert_accounting_ok(state, report, step);
    assert_live_handles_ok(state, report, step);
}

/**
 * @brief Probe stale handles that name a newly allocated slot index.
 *
 * @param state Generated sequence state containing stale handles.
 * @param index Slot index returned by the successful allocation.
 * @param report Report updated on the first stale-detection failure.
 * @param step One-based command index associated with the allocation.
 * @return No value.
 * @pre The allocation for `index` has succeeded and the new live handle is in
 * the generated model.
 * @post Any stale handle naming `index` has been checked through `deref`.
 * @invariant Reuse of the same slot index is the critical generation-safety
 * point: an implementation that fails to bump generations would let an old
 * handle dereference the new payload here.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local stale handle values and
 * invokes the public SlotMap dereference path on one thread.
 */
void probe_matching_stale_handles(sequence_state& state,
                                  index_type index,
                                  sequence_report& report,
                                  std::size_t step) noexcept {
    if (!report.ok) {
        return;
    }

    for (handle_type stale : state.stale) {
        if (stale.index() == index && !stale_deref_is_detected(state.map, stale)) {
            fail_sequence(report,
                          state,
                          step,
                          "stale handle dereferenced after same-index reuse");
            return;
        }
    }
}

/**
 * @brief Apply one generated allocation command.
 *
 * @param state Generated sequence state to mutate.
 * @param rng Random engine used to draw the new payload value.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `state.map` is alive.
 * @post Either one live handle has been added, or a full map has reported
 * capacity exhaustion without changing model-live state.
 * @invariant Successful allocation constructs exactly one payload and returns a
 * valid handle; full-map allocation must not leak a slot.
 * @throws `std::bad_alloc` if reserved-vector assumptions are broken or string
 * allocation inside a memsafe exception fails; the property driver catches
 * escaping exceptions and records seed metadata.
 * @note Ownership/thread-safety: mutates sequence-local SlotMap and model
 * state only.
 */
void apply_allocate(sequence_state& state,
                    std::mt19937_64& rng,
                    sequence_report& report,
                    std::size_t step) {
    const std::uint64_t value = draw_payload(rng);

    if (state.live.size() >= state.map.capacity()) {
        const std::size_t size_before = state.map.size();
        const std::size_t destroyed_before = state.ledger.destroyed;
        const std::size_t probe_id = state.next_payload_id;
        const violation_capture capture = capture_violation([&state,
                                                              probe_id,
                                                              value] {
            const handle_type extra =
                state.map.allocate(state.ledger, probe_id, value);
            (void)extra;
        });
        if (!is_capacity_exhaustion(capture)) {
            fail_sequence(report,
                          state,
                          step,
                          "full SlotMap allocation did not report capacity");
            return;
        }
        if (state.map.size() != size_before ||
            state.ledger.destroyed != destroyed_before) {
            fail_sequence(report,
                          state,
                          step,
                          "capacity failure changed SlotMap accounting");
        }
        return;
    }

    if (state.next_payload_id >= k_max_payload_records) {
        fail_sequence(report,
                      state,
                      step,
                      "generated more payload ids than fixed ledger capacity");
        return;
    }

    const std::size_t id = state.next_payload_id;
    handle_type handle{};
    try {
        handle = state.map.allocate(state.ledger, id, value);
    } catch (...) {
        fail_sequence(report,
                      state,
                      step,
                      "live SlotMap allocation threw unexpectedly");
        return;
    }

    if (!handle.is_valid()) {
        fail_sequence(report,
                      state,
                      step,
                      "SlotMap allocation returned an invalid handle");
        return;
    }

    ++state.next_payload_id;
    state.live.push_back(live_slot_record{handle, id, value});
    probe_matching_stale_handles(state, handle.index(), report, step);
}

/**
 * @brief Deallocate one selected live handle and move it to the stale model.
 *
 * @param state Generated sequence state to mutate.
 * @param index Index in `state.live` naming the handle to deallocate.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `index < state.live.size()`.
 * @post On success, the handle has moved from `live` to `stale`, the payload
 * destructor has run once, and immediate stale dereference is rejected.
 * @invariant Deallocation is the point where a live handle becomes stale; the
 * property checks both lifetime accounting and generation rejection at that
 * transition.
 * @throws Nothing intentionally; unexpected SlotMap exceptions are captured as
 * property failures.
 * @note Ownership/thread-safety: mutates sequence-local SlotMap and model
 * state only.
 */
void deallocate_live_at(sequence_state& state,
                        std::size_t index,
                        sequence_report& report,
                        std::size_t step) {
    const live_slot_record removed = state.live[index];
    const std::size_t size_before = state.map.size();
    const std::size_t destroyed_before = state.ledger.destroyed;

    try {
        state.map.deallocate(removed.handle);
    } catch (...) {
        fail_sequence(report,
                      state,
                      step,
                      "deallocating a generated live handle threw");
        return;
    }

    (void)remove_live_at(state, index);
    state.stale.push_back(removed.handle);

    if (state.map.size() + 1u != size_before ||
        state.ledger.destroyed != destroyed_before + 1u ||
        !state.ledger.destroyed_ids[removed.id]) {
        fail_sequence(report,
                      state,
                      step,
                      "live deallocation did not retire exactly one payload");
        return;
    }

    if (!stale_deref_is_detected(state.map, removed.handle)) {
        fail_sequence(report,
                      state,
                      step,
                      "freshly deallocated handle was not rejected");
    }
}

/**
 * @brief Apply one generated live-deallocation command.
 *
 * @param state Generated sequence state to mutate.
 * @param rng Random engine used to choose the live handle.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `state.map` is alive.
 * @post If a live handle existed, one has been deallocated and moved to the
 * stale model; otherwise the command is a no-op.
 * @invariant No-op behavior when no live handle exists keeps generated command
 * prefixes valid without manufacturing an invalid API call.
 * @throws Nothing intentionally except allocation failures from vector growth,
 * which are caught by `property_driver.hpp`.
 * @note Ownership/thread-safety: mutates sequence-local state only.
 */
void apply_deallocate_live(sequence_state& state,
                           std::mt19937_64& rng,
                           sequence_report& report,
                           std::size_t step) {
    if (state.live.empty()) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.live.size());
    deallocate_live_at(state, index, report, step);
}

/**
 * @brief Apply one generated live-dereference command.
 *
 * @param state Generated sequence state to inspect.
 * @param rng Random engine used to choose the live handle.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `state.map` is alive.
 * @post No model state is modified.
 * @invariant Live dereference commands explicitly sample the same property that
 * is also checked globally after every state transition.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local state only.
 */
void apply_dereference_live(sequence_state& state,
                            std::mt19937_64& rng,
                            sequence_report& report,
                            std::size_t step) noexcept {
    if (state.live.empty()) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.live.size());
    if (!live_record_dereferences_ok(state, state.live[index])) {
        fail_sequence(report,
                      state,
                      step,
                      "generated live dereference failed");
    }
}

/**
 * @brief Apply one generated stale-dereference command.
 *
 * @param state Generated sequence state to inspect and possibly mutate.
 * @param rng Random engine used to choose the stale handle or create one.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `state.map` is alive.
 * @post If a stale handle existed or could be created from a live handle, one
 * stale dereference has been required to report a violation.
 * @invariant The helper creates a stale handle from a live one when necessary
 * so generated stale-dereference commands exercise real stale handles instead
 * of degenerating into no-ops.
 * @throws Nothing intentionally except allocation failures from vector growth,
 * which are caught by `property_driver.hpp`.
 * @note Ownership/thread-safety: mutates sequence-local state only when it must
 * create the first stale handle.
 */
void apply_dereference_stale(sequence_state& state,
                             std::mt19937_64& rng,
                             sequence_report& report,
                             std::size_t step) {
    if (state.stale.empty() && !state.live.empty()) {
        const std::size_t live_index = bounded_random(rng, state.live.size());
        deallocate_live_at(state, live_index, report, step);
    }

    if (state.stale.empty() || !report.ok) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.stale.size());
    if (!stale_deref_is_detected(state.map, state.stale[index])) {
        fail_sequence(report,
                      state,
                      step,
                      "generated stale dereference was not rejected");
    }
}

/**
 * @brief Apply one generated stale-deallocation command.
 *
 * @param state Generated sequence state to inspect and possibly mutate.
 * @param rng Random engine used to choose the stale handle or create one.
 * @param report Report updated on the first failure.
 * @param step One-based command index being executed.
 * @return No value.
 * @pre `state.map` is alive.
 * @post If a stale handle existed or could be created from a live handle, one
 * duplicate deallocation has been required to report a stale-handle violation
 * without changing size or destruction counts.
 * @invariant Duplicate deallocation is the public way this property detects
 * double-free behavior without inspecting private free-list nodes.
 * @throws Nothing intentionally except allocation failures from vector growth,
 * which are caught by `property_driver.hpp`.
 * @note Ownership/thread-safety: mutates sequence-local state only when it must
 * create the first stale handle.
 */
void apply_deallocate_stale(sequence_state& state,
                            std::mt19937_64& rng,
                            sequence_report& report,
                            std::size_t step) {
    if (state.stale.empty() && !state.live.empty()) {
        const std::size_t live_index = bounded_random(rng, state.live.size());
        deallocate_live_at(state, live_index, report, step);
    }

    if (state.stale.empty() || !report.ok) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.stale.size());
    const std::size_t size_before = state.map.size();
    const std::size_t destroyed_before = state.ledger.destroyed;

    if (!stale_deallocate_is_detected(state.map, state.stale[index])) {
        fail_sequence(report,
                      state,
                      step,
                      "generated stale deallocation was not rejected");
        return;
    }

    if (state.map.size() != size_before ||
        state.ledger.destroyed != destroyed_before) {
        fail_sequence(report,
                      state,
                      step,
                      "stale deallocation changed SlotMap accounting");
    }
}

/**
 * @brief Apply one generated command to the SlotMap state machine.
 *
 * @param state Generated sequence state to mutate.
 * @param command Command to execute.
 * @param rng Random engine owned by the active sequence.
 * @param report Report updated on command failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre `state` satisfies the model invariant before dispatch.
 * @post Either the command has updated `state`, or the first failure has been
 * recorded in `report`.
 * @invariant Dispatch uses only public SlotMap operations and the generated
 * model, preserving black-box property coverage for CPP_MEMSAFE-0220-TEST.
 * @throws `std::bad_alloc` if vector operations or exception formatting
 * allocate and fail; the property driver records such failures with seed
 * metadata.
 * @note Ownership/thread-safety: all state is local to the generated sequence.
 */
void apply_command(sequence_state& state,
                   command_kind command,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) {
    if (state.live.empty() && state.stale.empty() &&
        command != command_kind::allocate) {
        /*
         * CPP_MEMSAFE-0220-TEST requires random alloc/dealloc/deref sequences,
         * not prefixes that do nothing because no handle has ever existed. The
         * first non-allocation command is therefore interpreted as the sequence
         * bootstrap allocation while still consuming the random command draw.
         */
        apply_allocate(state, rng, report, step);
        return;
    }

    switch (command) {
        case command_kind::allocate:
            apply_allocate(state, rng, report, step);
            break;
        case command_kind::deallocate_live:
            apply_deallocate_live(state, rng, report, step);
            break;
        case command_kind::dereference_live:
            apply_dereference_live(state, rng, report, step);
            break;
        case command_kind::dereference_stale:
            apply_dereference_stale(state, rng, report, step);
            break;
        case command_kind::deallocate_stale:
            apply_deallocate_stale(state, rng, report, step);
            break;
    }
}

/**
 * @brief Deallocate every model-live handle at the end of a prefix replay.
 *
 * @param state Generated sequence state to drain.
 * @param report Report updated on the first cleanup failure.
 * @return No value.
 * @pre `state.map` is alive.
 * @post The generated live vector is empty unless an implementation failure
 * prevented cleanup from completing.
 * @invariant Every prefix replay ends at a complete SlotMap lifetime boundary
 * so leak and double-free checks are comparable across original and shrunk
 * sequences.
 * @throws Nothing intentionally except reserved-vector assumptions; unexpected
 * SlotMap failures are recorded in `report`.
 * @note Ownership/thread-safety: mutates sequence-local state only.
 */
void cleanup_live_handles(sequence_state& state,
                          sequence_report& report) {
    while (!state.live.empty()) {
        const std::size_t step =
            report.executed_steps == 0u ? 0u : report.executed_steps;
        deallocate_live_at(state, state.live.size() - 1u, report, step);
        if (!report.ok) {
            break;
        }
    }
}

/**
 * @brief Verify all stale handles are rejected after final cleanup.
 *
 * @param state Generated sequence state to inspect.
 * @param report Report updated on the first stale-detection failure.
 * @return No value.
 * @pre All model-live handles have been deallocated.
 * @post No model state is modified.
 * @invariant A passing cleanup leaves every handle in the sequence's stale pool
 * invalid, including handles for slots that were reused many times.
 * @throws Nothing.
 * @note Ownership/thread-safety: invokes public SlotMap dereference on one
 * thread.
 */
void assert_all_stale_rejected_after_cleanup(sequence_state& state,
                                             sequence_report& report) noexcept {
    if (!report.ok) {
        return;
    }

    const std::size_t step =
        report.executed_steps == 0u ? 0u : report.executed_steps;
    for (handle_type stale : state.stale) {
        if (!stale_deref_is_detected(state.map, stale)) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup left a stale handle dereferenceable");
            return;
        }
    }
}

/**
 * @brief Prove generated cleanup returned every slot to the free list once.
 *
 * @details
 * The lifetime ledger proves payload construction and destruction, but it
 * cannot by itself prove that `SlotMap::deallocate()` republished the freed
 * slot indices correctly. This helper performs the CPP_MEMSAFE-0220-TEST
 * no-leaked-or-double-freed-slots check through public API calls: after all
 * generated live handles have been deallocated, it allocates exactly
 * `capacity()` new payloads, requires every issued index to be unique, requires
 * a capacity+1 allocation to fail without lifetime side effects, then
 * deallocates the refill handles before final ledger teardown checks.
 *
 * @param state Generated sequence state after the ordinary cleanup drain.
 * @param report Report updated on the first free-list integrity failure.
 * @return No value.
 * @pre `state.map` is alive, `state.live.empty()`, and `state.map.size() == 0`.
 * @post On success, all refill handles have been moved to `state.stale`,
 * `state.live` is empty again, and `state.map.size() == 0`.
 * @invariant A leaked slot makes the refill stop before `capacity()`; a
 * double-freed slot makes a duplicate index appear or lets the capacity+1
 * allocation succeed. Both cases are detected without inspecting private
 * SlotMap storage, satisfying CPP_MEMSAFE-0220-TEST's slot-accounting clause.
 * @throws `std::bad_alloc` only if the reserved model vectors cannot store the
 * bounded refill records; escaping allocation failures are reported by
 * `property_driver.hpp` with seed metadata.
 * @note Ownership/thread-safety: all allocations, dereferences, and
 * deallocations occur on the sequence-local SlotMap on one test thread.
 */
void assert_cleanup_reclaims_each_slot_once(sequence_state& state,
                                            sequence_report& report) {
    if (!report.ok) {
        return;
    }

    const std::size_t step =
        report.executed_steps == 0u ? 0u : report.executed_steps;
    if (!state.live.empty() || state.map.size() != 0u) {
        fail_sequence(report,
                      state,
                      step,
                      "free-list refill probe started before cleanup completed");
        return;
    }

    std::array<bool, k_slot_capacity> seen_indices{};
    for (std::size_t refill_count = 0u;
         refill_count < state.map.capacity();
         ++refill_count) {
        if (state.next_payload_id >= k_max_payload_records) {
            fail_sequence(report,
                          state,
                          step,
                          "refill probe exceeded fixed ledger capacity");
            return;
        }

        const std::size_t id = state.next_payload_id;
        const std::uint64_t value =
            k_payload_value_modulus +
            (static_cast<std::uint64_t>(id) << 8u) +
            static_cast<std::uint64_t>(refill_count);

        handle_type handle{};
        try {
            handle = state.map.allocate(state.ledger, id, value);
        } catch (...) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup leaked a slot before capacity refill");
            return;
        }

        if (!handle.is_valid()) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup refill returned an invalid handle");
            return;
        }

        const std::size_t index = static_cast<std::size_t>(handle.index());
        if (index >= state.map.capacity()) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup refill returned an out-of-capacity index");
            return;
        }

        if (seen_indices[index]) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup double-freed a slot index");
            return;
        }

        seen_indices[index] = true;
        ++state.next_payload_id;
        state.live.push_back(live_slot_record{handle, id, value});

        /*
         * Same-index stale probes remain important during the refill pass: a
         * correct free list is insufficient if a reused slot revives an old
         * generation. This ties the slot-accounting check back to F3
         * `Generation_Checked`.
         */
        probe_matching_stale_handles(state, handle.index(), report, step);
        assert_step_invariants(state, report, step);
        if (!report.ok) {
            return;
        }
    }

    for (std::size_t index = 0u; index < state.map.capacity(); ++index) {
        if (!seen_indices[index]) {
            fail_sequence(report,
                          state,
                          step,
                          "cleanup refill missed a slot index");
            return;
        }
    }

    const std::size_t size_before = state.map.size();
    const std::size_t constructed_before = state.ledger.constructed;
    const std::size_t destroyed_before = state.ledger.destroyed;
    const std::size_t probe_id = state.next_payload_id;
    const std::uint64_t probe_value =
        k_payload_value_modulus + static_cast<std::uint64_t>(probe_id);
    const violation_capture exhausted = capture_violation([&state,
                                                           probe_id,
                                                           probe_value] {
        const handle_type extra =
            state.map.allocate(state.ledger, probe_id, probe_value);
        (void)extra;
    });

    if (!is_capacity_exhaustion(exhausted)) {
        fail_sequence(report,
                      state,
                      step,
                      "cleanup refill did not exhaust exactly at capacity");
        return;
    }

    if (state.map.size() != size_before ||
        state.ledger.constructed != constructed_before ||
        state.ledger.destroyed != destroyed_before) {
        fail_sequence(report,
                      state,
                      step,
                      "capacity+1 refill probe changed SlotMap accounting");
        return;
    }

    cleanup_live_handles(state, report);
}

/**
 * @brief Verify final no-leak and no-double-free state before map teardown.
 *
 * @param state Generated sequence state to inspect after cleanup.
 * @param report Report updated on the first final-accounting failure.
 * @return No value.
 * @pre Cleanup has attempted to deallocate every generated live handle.
 * @post No model state is modified.
 * @invariant Before the SlotMap destructor runs, every constructed payload must
 * already have been destroyed exactly once through explicit generated
 * deallocation.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local model and ledger state.
 */
void assert_final_accounting_before_teardown(
    const sequence_state& state,
    sequence_report& report) noexcept {
    if (!report.ok) {
        return;
    }

    const std::size_t step =
        report.executed_steps == 0u ? 0u : report.executed_steps;
    if (!state.live.empty() || state.map.size() != 0u) {
        fail_sequence(report,
                      state,
                      step,
                      "cleanup did not retire every live SlotMap handle");
        return;
    }

    if (!state.ledger.all_constructed_destroyed_once()) {
        fail_sequence(report,
                      state,
                      step,
                      "payloads were leaked or destroyed more than once");
    }
}

/**
 * @brief Execute one deterministic generated SlotMap command sequence.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @param prefix_limit Maximum number of generated commands to execute, or
 * `k_full_sequence_prefix` to execute the entire generated sequence.
 * @return Sequence report containing pass/fail status and replay metadata.
 * @pre `rng` must be the per-sequence engine supplied by
 * `memsafe::test_support::property::run_sequences`, or another engine seeded
 * with the reported failing sequence seed.
 * @post The tested `SlotMap` has been destroyed, and the returned report has
 * observed both pre-teardown and post-teardown lifetime accounting.
 * @invariant Generated operation streams contain public allocate, deallocate,
 * and dereference commands; every prefix is forced through cleanup plus a
 * capacity refill so leaked or double-freed slots are visible to public API
 * checks and the lifetime ledger.
 * @throws `std::bad_alloc` if vector storage or exception formatting fails.
 * The property driver catches escaping exceptions and records seed metadata.
 * @note Ownership/thread-safety: all SlotMap instances, handles, and ledgers
 * are local to the sequence and single-threaded.
 */
sequence_report run_slotmap_sequence(std::mt19937_64& rng,
                                     std::size_t prefix_limit) {
    sequence_report report;
    report.generated_steps =
        1u + bounded_random(rng, k_max_commands_per_sequence);
    report.executed_steps = report.generated_steps < prefix_limit
                                ? report.generated_steps
                                : prefix_limit;

    slot_lifetime_ledger ledger;
    std::size_t destroyed_before_map_teardown = 0u;

    {
        slot_map_type map;
        sequence_state state{ledger, map, {}, {}, 0u};
        state.live.reserve(k_max_payload_records);
        state.stale.reserve(k_max_payload_records);

        for (std::size_t step = 1u; step <= report.executed_steps; ++step) {
            /*
             * F2 classifies SlotMap as a stateful property target. The command
             * stream mutates the live/stale handle model, then checks the
             * public API invariants produced by that exact prefix.
             */
            const command_kind command = draw_command(rng);
            apply_command(state, command, rng, report, step);
            assert_step_invariants(state, report, step);
            if (!report.ok) {
                break;
            }
        }

        cleanup_live_handles(state, report);
        assert_all_stale_rejected_after_cleanup(state, report);
        assert_cleanup_reclaims_each_slot_once(state, report);
        assert_all_stale_rejected_after_cleanup(state, report);
        assert_final_accounting_before_teardown(state, report);
        destroyed_before_map_teardown = ledger.destroyed;
    }

    if (ledger.destroyed != destroyed_before_map_teardown) {
        fail_after_teardown(report,
                            ledger,
                            report.executed_steps,
                            "SlotMap destructor destroyed a payload after cleanup");
    }

    if (!ledger.all_constructed_destroyed_once()) {
        fail_after_teardown(report,
                            ledger,
                            report.executed_steps,
                            "final payload ledger was not balanced");
    }

    return report;
}

/**
 * @brief Replay one generated SlotMap sequence from a deterministic seed.
 *
 * @param seed Seed reported by `property_driver.hpp` for a specific sequence.
 * @param prefix_limit Command prefix to execute before forced cleanup.
 * @return Sequence report for the replayed prefix.
 * @pre `seed` must be the per-sequence seed, not the property run base seed.
 * @post No global state is modified.
 * @invariant Replays use the same command generator and cleanup checks as the
 * property body.
 * @throws `std::bad_alloc` under the same conditions as
 * `run_slotmap_sequence`.
 * @note Ownership/thread-safety: all replay state is local to the call.
 */
sequence_report replay_slotmap_sequence(std::uint64_t seed,
                                        std::size_t prefix_limit) {
    std::mt19937_64 rng(seed);
    return run_slotmap_sequence(rng, prefix_limit);
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
 * @invariant Prefixes are tested from shortest to longest to produce stable,
 * minimal diagnostics without requiring a full shrink tree.
 * @throws `std::bad_alloc` if a replay allocation fails.
 * @note Ownership/thread-safety: all replay attempts are independent and local.
 */
std::size_t find_minimal_failing_prefix(std::uint64_t seed,
                                        std::size_t original_steps) {
    for (std::size_t prefix = 0u; prefix <= original_steps; ++prefix) {
        if (!replay_slotmap_sequence(seed, prefix).ok) {
            return prefix;
        }
    }

    return original_steps;
}

/**
 * @brief Build a diagnostic shrink note for a failing SlotMap sequence.
 *
 * @param report Failed sequence report.
 * @return Human-readable note containing the first failing invariant and state
 * snapshot needed for replay triage.
 * @pre `report.ok == false`.
 * @post The returned string owns its diagnostic text.
 * @invariant The note is compact but includes the failure step, generated
 * command count, model counts, map size, and ledger counters.
 * @throws `std::bad_alloc` if string allocation fails.
 * @note Ownership/thread-safety: all state is local to the returned string.
 */
std::string describe_failure(const sequence_report& report) {
    std::string note = report.reason;
    note += "; failing_step=";
    note += std::to_string(report.failing_step);
    note += "; generated_steps=";
    note += std::to_string(report.generated_steps);
    note += "; executed_steps=";
    note += std::to_string(report.executed_steps);
    note += "; live_handles=";
    note += std::to_string(report.live_handles);
    note += "; stale_handles=";
    note += std::to_string(report.stale_handles);
    note += "; map_size=";
    note += std::to_string(report.map_size);
    note += "; constructed=";
    note += std::to_string(report.constructed);
    note += "; destroyed=";
    note += std::to_string(report.destroyed);
    return note;
}

/**
 * @brief Print property-driver failure metadata in replay form.
 *
 * @param result Failed property result returned by `run_sequences`.
 * @return No value.
 * @pre `result.passed() == false`.
 * @post A single diagnostic line containing seed and shrunk-prefix metadata has
 * been written to standard error.
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
 * @brief Check the completed SlotMap property result against F2 thresholds.
 *
 * @param result Result returned by `property::run_sequences`.
 * @return No value.
 * @pre `result` was produced by this translation unit's stateful property
 * configuration.
 * @post Harness failures are recorded for property failures or insufficient
 * sequence budgets.
 * @invariant A passing run executes the property driver's active stateful
 * threshold, which is 1 000 normally and at least 10 000 when a supported
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
 * @brief Run the stateful `memsafe::SlotMap<T>` property test.
 *
 * @retval 0 All generated command sequences satisfied the SlotMap live/stale
 * handle and payload lifetime invariants.
 * @retval 1 One or more harness checks failed, with replay seed and shrunk
 * prefix reported for property failures.
 * @pre The executable is run by the F4 property lane, which discovers
 * `testing/tests/property/*.cpp` and defines any lane-specific property macros
 * before including `property_driver.hpp`.
 * @post The process exit code is the test verdict consumed by CTest.
 * @invariant One named stateful property target executes for `SlotMap<T>` and
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
        prop::make_config("slotmap.alloc-dealloc-deref.generations",
                          prop::workload::stateful,
                          0x0220u);

    const prop::run_result result = prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const sequence_report report =
                run_slotmap_sequence(rng, k_full_sequence_prefix);
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

    RUN_TESTS("test_slotmap_property");
}
