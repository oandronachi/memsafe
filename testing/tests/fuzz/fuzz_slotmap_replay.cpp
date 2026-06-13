/**
 * @file fuzz_slotmap_replay.cpp
 * @brief Byte-driven fuzz replay harness for `memsafe::SlotMap<T>`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0230-TEST.
 *
 * Purpose:
 * - Drive the finalized CPP_MEMSAFE-0205-FUNC bounded `SlotMap<T>` allocator
 *   from an arbitrary input byte stream, exactly the way a libFuzzer target
 *   would (`LLVMFuzzerTestOneInput(const uint8_t*, size_t)`), but wrap that
 *   entry point in a deterministic replay `main()` built on the
 *   CPP_MEMSAFE-0035-TEST `fuzz_replay.hpp` adapter.
 * - Replay a set of built-in byte seeds plus any checked-in corpus files passed
 *   on the command line, so the fuzz lane runs as a normal exit-0 F4 test.
 * - Keep the *same* byte-to-command state-machine shape that a true timed
 *   libFuzzer build would consume, so promoting this file to a real libFuzzer
 *   target later requires no change to the oracle. True timed libFuzzer
 *   execution (`>=5 min/change`, `24 CPU-h nightly` per F2) is documented as
 *   optional future infrastructure, not a required F4 drop-in artifact
 *   (matching the CPP_MEMSAFE-0035-TEST `fuzz_replay.hpp` contract).
 *
 * State machine (identical under replay and a future libFuzzer build):
 * - The harness consumes the input one byte at a time. Each byte selects one of
 *   five commands (`byte % 5`) and carries a 0..51 operand (`byte / 5`) used to
 *   pick a handle out of the live or stale model pools:
 *     0 = allocate, 1 = deallocate a live handle, 2 = dereference a live handle,
 *     3 = dereference a stale handle, 4 = deallocate a stale handle.
 * - A model mirrors the map: `live` (handle + expected id/value) and `stale`
 *   (previously deallocated handles). Live operations must succeed and observe
 *   the originally allocated payload; stale operations must be rejected by the
 *   per-slot generation check; a full-capacity allocate must report
 *   `capacity_exhausted` without constructing a payload.
 *
 * Key invariants:
 * - The harness is total: *any* byte sequence (including empty, all-zero, and
 *   adversarial corpus bytes) drives only well-defined, in-bounds public
 *   `SlotMap<T>` calls. Handle selection is always reduced modulo the live/stale
 *   pool size, so the harness itself never performs an out-of-bounds or
 *   use-after-free access.
 * - Stale-handle access is detected *before* any freed payload is touched,
 *   because the THROW violation policy converts the generation mismatch into a
 *   thrown `memsafe::violation`. Therefore the sanitizer lanes (ASan/UBSan) see
 *   no use-after-free, no out-of-bounds, and no undefined behaviour from this
 *   harness: any report would be a genuine defect in the code under test.
 * - The entry point returns 0 only when every modelled invariant held for the
 *   whole input; a detected invariant break returns nonzero, which the replay
 *   adapter turns into a nonzero process status (an F4 failure verdict).
 * - Payload construction and destruction are balanced at the end of every
 *   input: a leaked or double-destroyed slot is reported.
 *
 * Ownership and thread-safety:
 * - Every `LLVMFuzzerTestOneInput` call owns a fresh `SlotMap`, model vectors,
 *   and lifetime ledger; nothing persists between inputs, so replay order does
 *   not affect the verdict.
 * - The harness is single-threaded by construction. Lock-free Treiber-stack
 *   interleavings are the subject of CPP_MEMSAFE-0240-TEST; this package
 *   verifies the single-threaded byte-driven lifetime contract.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force runtime generation checks for the SlotMap fuzz replay harness.
 *
 * @retval 1 Enables stale-handle and capacity checks even when an outer release
 * lane supplied `MEMSAFE_RELEASE_CHECKS=0`.
 * @pre Define before including any memsafe feature header.
 * @post `SlotMap<T>::deref`, `SlotMap<T>::get`, and `SlotMap<T>::deallocate`
 * report stale handles, and `allocate` reports capacity exhaustion, through the
 * configured violation policy.
 * @invariant CPP_MEMSAFE-0230-TEST must observe stale-handle detection on every
 * lane, so the fuzz harness cannot inherit a check-free release configuration;
 * disabling checks would also reintroduce real use-after-free for the sanitizer
 * lanes, defeating the acceptance criterion.
 * @throws Nothing directly; the macro only affects later inline checks.
 * @note Ownership/thread-safety: preprocessor configuration only, no runtime
 * storage.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception reporting for the fuzz harness violation oracle.
 *
 * @retval MEMSAFE_VIOLATION_THROW Routes every memsafe violation report to
 * throw `memsafe::violation`.
 * @pre Define before including `<memsafe/handle.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post Stale-handle access and capacity exhaustion can be asserted in-process
 * without aborting the replay executable, and the violation is raised *before*
 * any freed storage is dereferenced.
 * @invariant Only stale or over-capacity operations are expected to throw; live
 * operations must complete normally. Using THROW (rather than ABORT) is what
 * lets the deterministic replay `main()` continue through the whole corpus and
 * still keep the sanitizer lanes report-free.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: compile-time selection only. No violation
 * handler is installed by this harness.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../support/fuzz_replay.hpp"

#include <memsafe/handle.hpp>
#include <memsafe/violation.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {

/// Compile-time SlotMap capacity exercised by the byte-driven state machine.
constexpr std::size_t k_slot_capacity = 8u;

/**
 * @brief Upper bound on commands executed for one input.
 *
 * @details Bounds replay wall-clock and memory for adversarially long corpus
 * files. The replay harness only needs to exercise the state machine shape on
 * each seed quickly; the long timed budget (F2 `>=5 min/change`,
 * `24 CPU-h nightly`) belongs to the optional future libFuzzer infrastructure,
 * not to this exit-0 F4 artifact. Seeds and realistic corpora are far shorter
 * than this bound, so the cap never truncates intended coverage.
 */
constexpr std::size_t k_max_steps = 4096u;

/**
 * @brief Upper bound on retained stale handles per input.
 *
 * @details Each deallocated handle is checked for immediate rejection inline,
 * so capping the retained pool does not weaken the oracle: it only bounds the
 * memory used to keep a population of older stale handles around for
 * same-index reuse probing. Without a cap a long input could grow the stale
 * vector without limit.
 */
constexpr std::size_t k_max_stale = 64u;

/**
 * @brief Number of distinct command kinds in the byte-driven state machine.
 *
 * @invariant Must equal the number of arms handled in `apply_command`; the
 * input byte is reduced modulo this value to choose a command.
 */
constexpr std::uint8_t k_command_count = 5u;

/**
 * @brief Mutable construction/destruction ledger for one fuzz input.
 *
 * @details Payload objects bump these counters on construction and destruction.
 * The ledger outlives the `SlotMap` under test so the harness can prove, after
 * the map is torn down, that every constructed payload was destroyed exactly
 * once. Because `SlotMap<T>` stores payloads inline (it never uses the heap),
 * ASan cannot observe a leaked or double-destroyed slot; this ledger is the
 * oracle that can.
 *
 * @pre Must outlive every `fuzz_payload` constructed with it.
 * @post `constructed` and `destroyed` count observed lifetime events for one
 * input.
 * @invariant For a correct input, `constructed == destroyed` once the map and
 * all model handles are gone, and `double_destroy` stays false.
 * @throws Nothing; scalar counters only.
 * @note Ownership/thread-safety: owned by one input and used on one thread.
 */
struct lifetime_ledger final {
    /// Number of payload constructors observed for this input.
    std::size_t constructed = 0u;
    /// Number of payload destructors observed for this input.
    std::size_t destroyed = 0u;
    /// True if a destructor ran while `destroyed` already met `constructed`.
    bool double_destroy = false;
};

/**
 * @brief Payload stored in generated SlotMap slots.
 *
 * @details Carries a unique id, an expected value, and a non-owning pointer to
 * the input ledger. Construction and destruction update the ledger so the
 * harness can detect leaks and double destruction through public `SlotMap`
 * operations only.
 *
 * @pre The referenced ledger outlives the payload.
 * @post Construction records one event; destruction records exactly one
 * matching event for a correct lifetime.
 * @invariant `id_` and `value_` are immutable for the object's lifetime and let
 * the harness prove a live handle reads its own payload, not a reused slot's.
 * @throws Nothing; construction, observation, and destruction are `noexcept`.
 * @note Ownership/thread-safety: each instance is owned by one `SlotMap` slot
 * and used on one thread.
 *
 * Example:
 * @code
 * lifetime_ledger ledger;
 * fuzz_payload p(ledger, 0u, 42u);
 * // p.value() == 42u
 * @endcode
 */
class fuzz_payload final {
public:
    /**
     * @brief Construct a payload and record its construction.
     *
     * @param ledger Input ledger that records this object's lifetime.
     * @param id Unique payload id assigned by the model.
     * @param value Value expected through live-handle dereferences.
     * @return Constructors do not return a value.
     * @pre `ledger` remains alive until this payload is destroyed.
     * @post `ledger.constructed` increased by one; this object stores `id` and
     * `value`.
     * @invariant Construction performs no allocation, keeping any failure
     * attributable to SlotMap storage rather than the payload.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes the receiving object and the
     * caller-owned ledger only.
     */
    fuzz_payload(lifetime_ledger& ledger,
                 std::size_t id,
                 std::uint64_t value) noexcept
        : ledger_(&ledger), id_(id), value_(value) {
        ++ledger_->constructed;
    }

    /**
     * @brief Destroy the payload and record its destruction.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post `ledger_->destroyed` increased by one; a destruction beyond the
     * construction count sets `double_destroy`.
     * @invariant The destructor is allocation-free and `noexcept`, so teardown
     * cannot mask a SlotMap defect with an exception.
     * @throws Nothing.
     * @note Ownership/thread-safety: invoked by `SlotMap` during single-threaded
     * deallocation or map teardown.
     */
    ~fuzz_payload() noexcept {
        if (ledger_->destroyed >= ledger_->constructed) {
            ledger_->double_destroy = true;
        }
        ++ledger_->destroyed;
    }

    /**
     * @brief Return the unique payload id assigned by the model.
     *
     * @return Payload id.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The id is stable and distinguishes slot reuse from accidental
     * stale access to a freshly reused slot.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; used on one thread.
     */
    std::size_t id() const noexcept { return id_; }

    /**
     * @brief Return the expected payload value.
     *
     * @return Value supplied at allocation.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The value is immutable for the payload's lifetime.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; used on one thread.
     */
    std::uint64_t value() const noexcept { return value_; }

private:
    fuzz_payload(const fuzz_payload&) = delete;
    fuzz_payload& operator=(const fuzz_payload&) = delete;
    fuzz_payload(fuzz_payload&&) = delete;
    fuzz_payload& operator=(fuzz_payload&&) = delete;

    lifetime_ledger* ledger_;
    std::size_t id_;
    std::uint64_t value_;
};

/// SlotMap specialization driven by the byte-driven state machine.
using slot_map_type = memsafe::SlotMap<fuzz_payload, k_slot_capacity>;

/// Handle type issued by the tested SlotMap specialization.
using handle_type = slot_map_type::handle_type;

/// Slot index component type, used for same-index stale-handle probing.
using index_type = handle_type::index_type;

/**
 * @brief Model record for one currently live SlotMap handle.
 *
 * @pre `handle` was returned by the map under test and not yet deallocated.
 * @post The aggregate owns only scalar replay data and the non-owning handle.
 * @invariant `id` and `value` must match the payload observed through `handle`
 * while the record is live.
 * @throws Nothing; owns no dynamic resources.
 * @note Ownership/thread-safety: owned by one input, used on one thread.
 */
struct live_record final {
    /// Handle currently expected to dereference successfully.
    handle_type handle{};
    /// Payload id associated with the live handle.
    std::size_t id = 0u;
    /// Payload value associated with the live handle.
    std::uint64_t value = 0u;
};

/**
 * @brief Mutable model plus public SlotMap object for one fuzz input.
 *
 * @pre `ledger` and `map` references outlive this model object.
 * @post `live` and `stale` hold the model handles for the active input.
 * @invariant `map.size()` matches `live.size()` after each completed command,
 * and every handle in `stale` is rejected by SlotMap lookup.
 * @throws Vector growth can throw `std::bad_alloc`; the entry point catches
 * escaping exceptions and converts them to a failing status.
 * @note Ownership/thread-safety: stack-owned by one input on one thread.
 */
struct machine_state final {
    /// Lifetime ledger updated by payload constructors and destructors.
    lifetime_ledger& ledger;
    /// SlotMap instance under test for this input.
    slot_map_type& map;
    /// Handles still expected to name live payloads.
    std::vector<live_record> live;
    /// Handles already deallocated by the model (bounded by `k_max_stale`).
    std::vector<handle_type> stale;
    /// Next unique payload id assigned on a successful allocation.
    std::size_t next_id = 0u;
};

/**
 * @brief Report whether a caught violation classifies as stale-handle rejection.
 *
 * @param caught Violation thrown by a SlotMap operation.
 * @retval true The violation is a stale-handle class report.
 * @retval false The violation is some other kind.
 * @pre `caught` came from a public SlotMap lookup/deallocate path.
 * @post No state is modified.
 * @invariant SlotMap reports stale generation and duplicate-free paths as
 * `use_after_free`; `generation_mismatch` is accepted as an equivalent stale
 * classification if the policy vocabulary is refined later.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads the immutable exception only.
 */
bool is_stale_kind(const memsafe::violation& caught) noexcept {
    return caught.kind() == memsafe::violation_kind::use_after_free ||
           caught.kind() == memsafe::violation_kind::generation_mismatch;
}

/**
 * @brief Require a stale handle to be rejected by dereference.
 *
 * @param map SlotMap instance under test.
 * @param stale Handle already deallocated by the model.
 * @retval true `map.deref(stale)` reported a stale-handle violation.
 * @retval false Dereference succeeded or threw an unrelated exception.
 * @pre `stale` is no longer live for `map`.
 * @post The map is unchanged unless the code under test is broken.
 * @invariant The probe uses the public dereference path, never private slot
 * state, and never touches freed storage because the generation check throws
 * first.
 * @throws Nothing; all operation exceptions are captured.
 * @note Ownership/thread-safety: synchronous, one thread.
 */
bool stale_deref_rejected(slot_map_type& map, handle_type stale) noexcept {
    try {
        const fuzz_payload& payload = map.deref(stale);
        (void)payload;
        return false;
    } catch (const memsafe::violation& caught) {
        return is_stale_kind(caught);
    } catch (...) {
        return false;
    }
}

/**
 * @brief Require a stale handle to be rejected by duplicate deallocation.
 *
 * @param map SlotMap instance under test.
 * @param stale Handle already deallocated by the model.
 * @retval true `map.deallocate(stale)` reported a stale-handle violation.
 * @retval false Deallocation succeeded or threw an unrelated exception.
 * @pre `stale` is no longer live for `map`.
 * @post The map is unchanged unless the code under test is broken.
 * @invariant Duplicate deallocation must not destroy another payload, decrement
 * size, or push the slot index again.
 * @throws Nothing; all operation exceptions are captured.
 * @note Ownership/thread-safety: synchronous, one thread.
 */
bool stale_deallocate_rejected(slot_map_type& map, handle_type stale) noexcept {
    try {
        map.deallocate(stale);
        return false;
    } catch (const memsafe::violation& caught) {
        return is_stale_kind(caught);
    } catch (...) {
        return false;
    }
}

/**
 * @brief Record a deallocated handle in the bounded stale pool.
 *
 * @param state Model to mutate.
 * @param handle Handle just deallocated by the model.
 * @return No value.
 * @pre `handle` has already been verified to reject immediately by the caller.
 * @post `handle` is retained for later same-index reuse probing unless the pool
 * is already at `k_max_stale`, in which case it is dropped (its immediate
 * rejection was already proven).
 * @invariant The stale pool never exceeds `k_max_stale`, bounding memory for
 * adversarially long inputs.
 * @throws `std::bad_alloc` only if the bounded vector reallocates and fails.
 * @note Ownership/thread-safety: mutates input-local state only.
 */
void retain_stale(machine_state& state, handle_type handle) {
    if (state.stale.size() < k_max_stale) {
        state.stale.push_back(handle);
    }
}

/**
 * @brief Verify one live handle dereferences to its modelled payload.
 *
 * @param state Model containing the map under test.
 * @param record Live handle record to verify.
 * @retval true Dereference succeeded and observed the expected id and value.
 * @retval false Dereference threw or returned the wrong payload.
 * @pre `record` is present in `state.live`.
 * @post No model state is modified.
 * @invariant A live handle must never be rejected as stale and must never
 * resolve to a different reused slot's payload.
 * @throws Nothing; unexpected exceptions become `false`.
 * @note Ownership/thread-safety: reads input-local state on one thread.
 */
bool live_deref_ok(machine_state& state, const live_record& record) noexcept {
    try {
        const fuzz_payload& payload = state.map.deref(record.handle);
        return payload.id() == record.id && payload.value() == record.value;
    } catch (...) {
        return false;
    }
}

/**
 * @brief After a reuse, require every stale handle naming that index to reject.
 *
 * @param state Model containing the stale pool.
 * @param index Slot index just returned by a successful allocation.
 * @retval true Every stale handle naming `index` was rejected.
 * @retval false A stale handle dereferenced a reused slot (missing generation
 * bump).
 * @pre The allocation for `index` succeeded and is now live.
 * @post No model state is modified.
 * @invariant Same-index reuse is the critical generation-safety point: an
 * implementation that fails to bump generations would let an old handle observe
 * the new payload here. This ties the harness to F3 `Generation_Checked`.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads input-local state on one thread.
 */
bool stale_handles_for_index_reject(machine_state& state,
                                    index_type index) noexcept {
    for (handle_type stale : state.stale) {
        if (stale.index() == index && !stale_deref_rejected(state.map, stale)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Apply one allocation command.
 *
 * @param state Model to mutate.
 * @param operand Byte-derived operand (unused for allocation; reserved for
 * shape symmetry with handle-selecting commands).
 * @retval true The command preserved every checked invariant.
 * @retval false An invariant failed (wrong capacity behaviour, bad handle,
 * wrong accounting, or a revived stale handle).
 * @pre `state.map` is alive.
 * @post On a non-full map, one live handle is added; on a full map, capacity
 * exhaustion is required without constructing a payload.
 * @invariant Successful allocation constructs exactly one payload and returns a
 * valid handle; full-map allocation must not leak or construct a slot.
 * @throws `std::bad_alloc` if the model vector reallocates and fails; the entry
 * point converts this into a failing status.
 * @note Ownership/thread-safety: mutates input-local state only.
 */
bool apply_allocate(machine_state& state, std::uint8_t operand) {
    const std::uint64_t value =
        (static_cast<std::uint64_t>(state.next_id) << 8u) ^
        (static_cast<std::uint64_t>(operand) + 1u);

    if (state.live.size() >= state.map.capacity()) {
        // Full map: an extra allocation must report capacity exhaustion and
        // must not construct a payload (CPP_MEMSAFE-0205-FUNC acceptance).
        const std::size_t size_before = state.map.size();
        const std::size_t constructed_before = state.ledger.constructed;
        bool reported_capacity = false;
        try {
            const handle_type extra =
                state.map.allocate(state.ledger, state.next_id, value);
            (void)extra;
        } catch (const memsafe::violation& caught) {
            reported_capacity =
                caught.kind() == memsafe::violation_kind::capacity_exhausted;
        } catch (...) {
            reported_capacity = false;
        }
        if (!reported_capacity) {
            std::fprintf(stderr,
                         "fuzz: full SlotMap allocation did not report "
                         "capacity_exhausted\n");
            return false;
        }
        if (state.map.size() != size_before ||
            state.ledger.constructed != constructed_before) {
            std::fprintf(stderr,
                         "fuzz: capacity failure changed SlotMap accounting\n");
            return false;
        }
        return true;
    }

    handle_type handle{};
    try {
        handle = state.map.allocate(state.ledger, state.next_id, value);
    } catch (...) {
        std::fprintf(stderr, "fuzz: live SlotMap allocation threw\n");
        return false;
    }
    if (!handle.is_valid()) {
        std::fprintf(stderr, "fuzz: allocation returned an invalid handle\n");
        return false;
    }

    const std::size_t id = state.next_id;
    ++state.next_id;
    state.live.push_back(live_record{handle, id, value});
    if (state.map.size() != state.live.size()) {
        std::fprintf(stderr, "fuzz: SlotMap::size did not match live handles\n");
        return false;
    }
    return stale_handles_for_index_reject(state, handle.index());
}

/**
 * @brief Deallocate one selected live handle and move it to the stale model.
 *
 * @param state Model to mutate.
 * @param index Index in `state.live` of the handle to deallocate.
 * @retval true Deallocation retired exactly one payload and the freshly stale
 * handle was rejected immediately.
 * @retval false Deallocation threw, mis-accounted, or the freed handle was not
 * rejected.
 * @pre `index < state.live.size()`.
 * @post The handle has moved from `live` to the stale pool and the payload
 * destructor ran once.
 * @invariant Deallocation is the live-to-stale transition; both lifetime
 * accounting and immediate generation rejection are checked here.
 * @throws `std::bad_alloc` from model vector growth; converted to a failing
 * status by the entry point.
 * @note Ownership/thread-safety: mutates input-local state only.
 */
bool deallocate_live_at(machine_state& state, std::size_t index) {
    const live_record removed = state.live[index];
    const std::size_t size_before = state.map.size();
    const std::size_t destroyed_before = state.ledger.destroyed;

    try {
        state.map.deallocate(removed.handle);
    } catch (...) {
        std::fprintf(stderr, "fuzz: deallocating a live handle threw\n");
        return false;
    }

    // Swap-pop: live order is not semantically meaningful to the model.
    state.live[index] = state.live.back();
    state.live.pop_back();

    if (state.map.size() + 1u != size_before ||
        state.ledger.destroyed != destroyed_before + 1u) {
        std::fprintf(stderr,
                     "fuzz: live deallocation did not retire exactly one "
                     "payload\n");
        return false;
    }
    if (!stale_deref_rejected(state.map, removed.handle)) {
        std::fprintf(stderr,
                     "fuzz: freshly deallocated handle was not rejected\n");
        return false;
    }
    retain_stale(state, removed.handle);
    return true;
}

/**
 * @brief Apply one live-deallocation command.
 *
 * @param state Model to mutate.
 * @param operand Byte-derived operand selecting which live handle to free.
 * @retval true The command preserved every checked invariant (a no-op when no
 * live handle exists also returns true).
 * @retval false A deallocation invariant failed.
 * @pre `state.map` is alive.
 * @post If a live handle existed, one was deallocated; otherwise the command is
 * a no-op.
 * @invariant No-op on an empty live pool keeps every byte sequence valid
 * without manufacturing an invalid API call.
 * @throws `std::bad_alloc` from model vector growth.
 * @note Ownership/thread-safety: mutates input-local state only.
 */
bool apply_deallocate_live(machine_state& state, std::uint8_t operand) {
    if (state.live.empty()) {
        return true;
    }
    const std::size_t index = operand % state.live.size();
    return deallocate_live_at(state, index);
}

/**
 * @brief Apply one live-dereference command.
 *
 * @param state Model containing the map under test.
 * @param operand Byte-derived operand selecting which live handle to read.
 * @retval true The selected live handle read its expected payload (or there was
 * no live handle to read).
 * @retval false The live dereference threw or read the wrong payload.
 * @pre `state.map` is alive.
 * @post No model state is modified.
 * @invariant Explicit live dereferences sample the same property checked after
 * every allocation, increasing coverage of long-lived handles.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads input-local state only.
 */
bool apply_dereference_live(machine_state& state, std::uint8_t operand) noexcept {
    if (state.live.empty()) {
        return true;
    }
    const std::size_t index = operand % state.live.size();
    if (!live_deref_ok(state, state.live[index])) {
        std::fprintf(stderr, "fuzz: live dereference failed\n");
        return false;
    }
    return true;
}

/**
 * @brief Apply one stale-dereference command.
 *
 * @param state Model to inspect and possibly mutate.
 * @param operand Byte-derived operand selecting which stale handle to probe.
 * @retval true The selected stale handle was rejected (or none could be made).
 * @retval false A stale handle dereferenced successfully (a generation-safety
 * defect).
 * @pre `state.map` is alive.
 * @post If no stale handle existed, one is created from a live handle so the
 * command exercises a real stale handle instead of degenerating into a no-op.
 * @invariant Stale dereference must always be rejected by the generation check.
 * @throws `std::bad_alloc` from model vector growth.
 * @note Ownership/thread-safety: mutates input-local state only when it must
 * create the first stale handle.
 */
bool apply_dereference_stale(machine_state& state, std::uint8_t operand) {
    if (state.stale.empty() && !state.live.empty()) {
        if (!deallocate_live_at(state, operand % state.live.size())) {
            return false;
        }
    }
    if (state.stale.empty()) {
        return true;
    }
    const std::size_t index = operand % state.stale.size();
    if (!stale_deref_rejected(state.map, state.stale[index])) {
        std::fprintf(stderr, "fuzz: stale dereference was not rejected\n");
        return false;
    }
    return true;
}

/**
 * @brief Apply one stale-deallocation command.
 *
 * @param state Model to inspect and possibly mutate.
 * @param operand Byte-derived operand selecting which stale handle to probe.
 * @retval true The selected stale handle was rejected without side effects (or
 * none could be made).
 * @retval false A duplicate deallocation succeeded or changed accounting.
 * @pre `state.map` is alive.
 * @post If no stale handle existed, one is created from a live handle first.
 * @invariant Duplicate deallocation is the public way the harness detects
 * double-free behaviour without inspecting private free-list nodes; it must not
 * destroy a payload or change `size()`.
 * @throws `std::bad_alloc` from model vector growth.
 * @note Ownership/thread-safety: mutates input-local state only when it must
 * create the first stale handle.
 */
bool apply_deallocate_stale(machine_state& state, std::uint8_t operand) {
    if (state.stale.empty() && !state.live.empty()) {
        if (!deallocate_live_at(state, operand % state.live.size())) {
            return false;
        }
    }
    if (state.stale.empty()) {
        return true;
    }
    const std::size_t index = operand % state.stale.size();
    const std::size_t size_before = state.map.size();
    const std::size_t destroyed_before = state.ledger.destroyed;

    if (!stale_deallocate_rejected(state.map, state.stale[index])) {
        std::fprintf(stderr, "fuzz: stale deallocation was not rejected\n");
        return false;
    }
    if (state.map.size() != size_before ||
        state.ledger.destroyed != destroyed_before) {
        std::fprintf(stderr,
                     "fuzz: stale deallocation changed SlotMap accounting\n");
        return false;
    }
    return true;
}

/**
 * @brief Dispatch one command byte to the SlotMap state machine.
 *
 * @param state Model to mutate.
 * @param command Command selector in `[0, k_command_count)`.
 * @param operand Byte-derived operand used to select a handle.
 * @retval true The command preserved every checked invariant.
 * @retval false An invariant failed.
 * @pre `command < k_command_count` and `state` satisfies the model invariant.
 * @post Either the command updated `state` or a failure was reported.
 * @invariant When both pools are empty, any non-allocate command is reinterpreted
 * as the bootstrap allocation so a byte stream that never draws `allocate`
 * still exercises the map instead of doing nothing.
 * @throws `std::bad_alloc` from model vector growth.
 * @note Ownership/thread-safety: all state is input-local.
 */
bool apply_command(machine_state& state,
                   std::uint8_t command,
                   std::uint8_t operand) {
    if (state.live.empty() && state.stale.empty() && command != 0u) {
        return apply_allocate(state, operand);
    }

    switch (command) {
        case 0u:
            return apply_allocate(state, operand);
        case 1u:
            return apply_deallocate_live(state, operand);
        case 2u:
            return apply_dereference_live(state, operand);
        case 3u:
            return apply_dereference_stale(state, operand);
        default:
            return apply_deallocate_stale(state, operand);
    }
}

/**
 * @brief Drain remaining live handles and verify final slot accounting.
 *
 * @param state Model to drain.
 * @retval true Cleanup deallocated every live handle, all stale handles stayed
 * rejected, a full-capacity refill reclaimed every slot exactly once, and the
 * lifetime ledger balanced.
 * @retval false A cleanup invariant failed (leak, double-free, revived stale
 * handle, or unbalanced lifetime).
 * @pre `state.map` is alive.
 * @post On success, `state.live` is empty and `state.map.size() == 0`.
 * @invariant Ending every input at a complete lifetime boundary lets the
 * harness perform the CPP_MEMSAFE-0220-style no-leak / no-double-free slot check
 * through public API only: after draining, exactly `capacity()` fresh
 * allocations must each return a distinct in-range index, and a `capacity()+1`
 * allocation must fail.
 * @throws `std::bad_alloc` from model vector growth; converted to a failing
 * status by the entry point.
 * @note Ownership/thread-safety: mutates input-local state only.
 */
bool finalize_input(machine_state& state) {
    while (!state.live.empty()) {
        if (!deallocate_live_at(state, state.live.size() - 1u)) {
            return false;
        }
    }

    for (handle_type stale : state.stale) {
        if (!stale_deref_rejected(state.map, stale)) {
            std::fprintf(stderr,
                         "fuzz: cleanup left a stale handle dereferenceable\n");
            return false;
        }
    }

    // Public-API free-list integrity check: every freed slot must be reclaimed
    // exactly once. A leaked slot stops the refill early; a double-freed slot
    // either yields a duplicate index or lets the capacity+1 allocation succeed.
    bool seen_index[k_slot_capacity] = {};
    for (std::size_t filled = 0u; filled < state.map.capacity(); ++filled) {
        handle_type handle{};
        try {
            handle = state.map.allocate(state.ledger, state.next_id,
                                        static_cast<std::uint64_t>(filled));
        } catch (...) {
            std::fprintf(stderr, "fuzz: cleanup leaked a slot before refill\n");
            return false;
        }
        ++state.next_id;
        if (!handle.is_valid()) {
            std::fprintf(stderr, "fuzz: refill returned an invalid handle\n");
            return false;
        }
        const std::size_t idx = static_cast<std::size_t>(handle.index());
        if (idx >= state.map.capacity() || seen_index[idx]) {
            std::fprintf(stderr, "fuzz: cleanup double-freed a slot index\n");
            return false;
        }
        seen_index[idx] = true;
        state.live.push_back(live_record{handle, 0u, 0u});
    }

    // capacity+1 must fail now that every slot is full again.
    bool overflow_reported = false;
    try {
        const handle_type extra =
            state.map.allocate(state.ledger, state.next_id, 0u);
        (void)extra;
    } catch (const memsafe::violation& caught) {
        overflow_reported =
            caught.kind() == memsafe::violation_kind::capacity_exhausted;
    } catch (...) {
        overflow_reported = false;
    }
    if (!overflow_reported) {
        std::fprintf(stderr, "fuzz: refilled map accepted an extra allocation\n");
        return false;
    }

    while (!state.live.empty()) {
        if (!deallocate_live_at(state, state.live.size() - 1u)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Run the byte-driven SlotMap state machine for one input.
 *
 * @param data Pointer to input bytes, or null when `size == 0`.
 * @param size Number of input bytes.
 * @retval true Every modelled invariant held across the whole input and final
 * cleanup.
 * @retval false An invariant failed.
 * @pre `data` points to at least `size` readable bytes, or `size == 0`.
 * @post A fresh SlotMap, model, and ledger were exercised and torn down.
 * @invariant Handle selection is always reduced modulo the relevant pool size,
 * so the harness performs only in-bounds, well-defined public calls regardless
 * of the input bytes. At most `k_max_steps` command bytes are processed.
 * @throws `std::bad_alloc` from model vector growth; the caller converts it to a
 * failing status.
 * @note Ownership/thread-safety: all state is input-local on one thread.
 */
bool run_state_machine(const std::uint8_t* data, std::size_t size) {
    lifetime_ledger ledger;
    slot_map_type map;
    machine_state state{ledger, map, {}, {}, 0u};

    const std::size_t steps = size < k_max_steps ? size : k_max_steps;
    for (std::size_t i = 0u; i < steps; ++i) {
        const std::uint8_t byte = data[i];
        const std::uint8_t command = static_cast<std::uint8_t>(byte % k_command_count);
        const std::uint8_t operand = static_cast<std::uint8_t>(byte / k_command_count);
        if (!apply_command(state, command, operand)) {
            return false;
        }
    }

    if (!finalize_input(state)) {
        return false;
    }

    // After every model handle (including the refill probe) has been
    // deallocated, the map holds no live payload, so payload constructions and
    // destructions must balance and no payload may have been destroyed twice.
    // The inline `SlotMap<T>` storage is never heap-backed, so this ledger is
    // the only oracle that can observe a leaked or double-destroyed slot.
    if (state.ledger.double_destroy ||
        state.ledger.constructed != state.ledger.destroyed) {
        std::fprintf(stderr,
                     "fuzz: payload lifetime imbalance (constructed=%zu "
                     "destroyed=%zu double_destroy=%d)\n",
                     state.ledger.constructed, state.ledger.destroyed,
                     static_cast<int>(state.ledger.double_destroy));
        return false;
    }
    return true;
}

/**
 * @brief Build the built-in byte seeds replayed when no corpus file is given.
 *
 * @return Vector of named byte sequences covering representative state-machine
 * shapes.
 * @pre None.
 * @post The returned corpus owns its byte vectors.
 * @invariant The seeds deterministically cover: the empty input; a fill to
 * capacity and beyond; allocate/free/reuse with same-index stale probing;
 * repeated stale dereference and stale deallocation; and a long mixed walk.
 * Command bytes are chosen as `command + 5*operand` so each seed reads as an
 * explicit command program.
 * @throws `std::bad_alloc` if seed construction fails.
 * @note Ownership/thread-safety: builds local state only.
 */
std::vector<memsafe::test_support::fuzz::byte_sequence> built_in_seeds() {
    namespace fz = memsafe::test_support::fuzz;
    std::vector<fz::byte_sequence> seeds;

    // Empty input: exercises the zero-length path and the cleanup refill check.
    seeds.push_back(fz::bytes_case("empty", {}));

    // Allocate well past capacity (k_slot_capacity == 8): the first 8 fill the
    // map, the rest hit the capacity-exhausted branch. command 0 == allocate.
    seeds.push_back(fz::bytes_case(
        "fill-and-overflow",
        {0u, 5u, 10u, 15u, 20u, 25u, 30u, 35u, 40u, 45u, 50u, 0u}));

    // allocate, allocate, deallocate-live, allocate (reuse), deref-stale.
    // 0 == allocate; 1 == deallocate_live; 3 == dereference_stale.
    seeds.push_back(fz::bytes_case(
        "alloc-free-reuse", {0u, 5u, 1u, 0u, 3u, 8u}));

    // Repeated stale operations: 3 == dereference_stale, 4 == deallocate_stale.
    seeds.push_back(fz::bytes_case(
        "stale-storm", {0u, 0u, 1u, 3u, 4u, 3u, 4u, 9u, 14u}));

    // Long mixed walk across all five commands and many operands.
    seeds.push_back(fz::bytes_case(
        "mixed-walk",
        {0u,  6u,  12u, 18u, 24u, 31u, 37u, 43u, 49u, 2u,
         8u,  14u, 1u,  7u,  13u, 0u,  19u, 25u, 3u,  4u,
         11u, 17u, 23u, 29u, 0u,  5u,  2u,  1u,  3u,  4u}));

    return seeds;
}

} // namespace

/**
 * @brief libFuzzer-shaped entry point driving `SlotMap<T>` from input bytes.
 *
 * @details This is the single state-machine definition shared by the
 * deterministic replay `main()` below and any future true libFuzzer build. It
 * deliberately uses the canonical `extern "C" int(const uint8_t*, size_t)`
 * signature and external linkage of `LLVMFuzzerTestOneInput`, so promoting this
 * file to a timed libFuzzer target (optional future infrastructure, per F2 and
 * the CPP_MEMSAFE-0035-TEST contract) requires linking a libFuzzer driver and
 * no change to the oracle below.
 *
 * @param data Pointer to the first input byte, or null when `size == 0`. Not
 * dereferenced when `size == 0`.
 * @param size Number of input bytes.
 * @retval 0 The input drove the SlotMap with every modelled invariant intact.
 * @retval 1 A modelled invariant failed, or model bookkeeping threw (for
 * example, `std::bad_alloc`).
 * @pre The pointed-to bytes remain valid for the duration of the call.
 * @post A fresh SlotMap and model were exercised and destroyed; no state
 * persists to the next call.
 * @invariant Returns nonzero only on a genuine defect in the code under test,
 * never on arbitrary input shape, because the harness performs only in-bounds,
 * well-defined operations. Under ASan/UBSan lanes a clean run must produce no
 * sanitizer report.
 * @throws Nothing; all exceptions are caught and converted to status 1
 * (libFuzzer entry points must not let exceptions escape).
 * @note Ownership/thread-safety: single-threaded; owns no state across calls.
 *
 * Example:
 * @code
 * const std::uint8_t seed[] = {0u, 1u, 0u};
 * int status = LLVMFuzzerTestOneInput(seed, sizeof(seed));
 * @endcode
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    try {
        return run_state_machine(data, size) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "fuzz: harness bookkeeping threw: %s\n", ex.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "fuzz: harness bookkeeping threw\n");
        return 1;
    }
}

/**
 * @brief Deterministic replay entry point for the SlotMap fuzz harness.
 *
 * @param argc Standard argument count.
 * @param argv Standard argument vector. Extra arguments are treated as binary
 * corpus file paths and replayed instead of the built-in seeds.
 * @retval 0 Every replayed input passed.
 * @retval nonzero A corpus file could not be read, or an input exposed a SlotMap
 * invariant failure.
 * @pre Pass the original `argc`/`argv` from the standalone executable.
 * @post Either the supplied corpus files or the built-in seeds have been driven
 * through `LLVMFuzzerTestOneInput`.
 * @invariant This stays a normal exit-0 F4 test even though the entry point can
 * later be reused by true libFuzzer infrastructure (CPP_MEMSAFE-0035-TEST /
 * CPP_MEMSAFE-0230-TEST contract).
 * @throws Nothing escapes; `replay_main` and the entry point convert failures
 * into a nonzero status.
 * @note Ownership/thread-safety: single-threaded; owns loaded file bytes.
 */
int main(int argc, char** argv) {
    return memsafe::test_support::fuzz::replay_main(
        argc, argv, &LLVMFuzzerTestOneInput, built_in_seeds());
}
