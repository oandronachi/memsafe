/**
 * @file test_owner_property.cpp
 * @brief Property tests for Slice 1 `Owner`, `Ref`, and `MutRef` borrow rules.
 *
 * @details
 * Work package: CPP_MEMSAFE-0140-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0100-FUNC ownership primitives through
 *   generated construct, borrow, move, mutation, and drop command sequences.
 * - Use the CPP_MEMSAFE-0035-TEST `property_driver.hpp` threshold machinery so
 *   each Owner/Ref/MutRef property target runs at least 10 000 non-stateful
 *   sequences in the property lane and receives the required 10x nightly
 *   multiplier when a supported nightly macro is defined.
 * - Report the deterministic replay seed and the smallest command prefix known
 *   to reproduce an invariant failure.
 *
 * Key invariants:
 * - The generated model never owns both a live `MutRef<T>` and any live
 *   `Ref<T>` at the same time.
 * - A live immutable borrow permits additional immutable borrows and rejects an
 *   attempted mutable borrow with `violation_kind::borrow_exclusivity`.
 * - A live mutable borrow rejects an attempted immutable borrow with
 *   `violation_kind::borrow_exclusivity`.
 * - Moving the owner preserves the payload value observed through existing
 *   borrow handles because `Owner<T>` moves the control-block pointer rather
 *   than relocating the control block itself.
 *
 * Ownership and thread-safety:
 * - All generated state is owned by one test process and one test thread.
 * - Borrow handles are destroyed before their owning owner is destroyed, which
 *   respects the unsynchronized Slice 1 preconditions while still exercising
 *   random drop ordering for owners, refs, and mutable refs.
 */

#ifdef MEMSAFE_RELEASE_CHECKS
#  undef MEMSAFE_RELEASE_CHECKS
#endif

/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Force checked borrow counters for the Owner/Ref/MutRef property test.
 *
 * @retval 1 Enables the Slice 1 shared and mutable borrow ledger even when an
 * outer infra lane supplies release definitions.
 * @pre Define before including any memsafe feature header.
 * @post Conflicting generated borrow attempts are observable in-process rather
 * than compiled out.
 * @invariant CPP_MEMSAFE-0140-TEST asserts the checked borrow invariant after
 * every generated step, so this translation unit must not inherit
 * `MEMSAFE_RELEASE_CHECKS=0` from a release lane.
 * @throws Nothing directly; this is compile-time configuration only.
 * @note Ownership/thread-safety: the macro owns no storage and affects only the
 * inline code emitted for this test translation unit.
 */
#define MEMSAFE_RELEASE_CHECKS 1

#ifdef MEMSAFE_ON_VIOLATION
#  undef MEMSAFE_ON_VIOLATION
#endif

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select exception reporting for generated negative borrow attempts.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs borrow-rule violations to throw
 * `memsafe::violation`.
 * @pre Define before including `<memsafe/owner.hpp>` or
 * `<memsafe/violation.hpp>`.
 * @post The property can assert that the generated conflicting borrow attempts
 * report `violation_kind::borrow_exclusivity` without aborting the process.
 * @invariant Only library-managed overlap attempts are expected to throw; all
 * other violations are treated as property failures.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: this preprocessor selection owns no runtime
 * state and installs no global handler.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../../test_harness.hpp"

#include "../support/property_driver.hpp"

#include <memsafe/owner.hpp>
#include <memsafe/violation.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace {

/// Maximum generated command count for one non-stateful owner property sequence.
constexpr std::size_t k_max_commands_per_sequence = 24u;

/// Sentinel prefix limit that means "execute the full generated sequence".
constexpr std::size_t k_full_sequence_prefix =
    (std::numeric_limits<std::size_t>::max)();

/// Upper bound used when generating payload values for the tracked object.
constexpr std::uint64_t k_payload_value_modulus = 1000003ull;

/**
 * @brief Command kinds generated for the Owner/Ref/MutRef state machine.
 *
 * @pre Values are produced only by `draw_command`.
 * @post Dispatching a command either mutates the generated sequence state or is
 * a documented no-op when the required precondition, such as a live owner, is
 * absent.
 * @invariant The command set contains construction, borrowing, moving, and
 * dropping operations required by CPP_MEMSAFE-0140-TEST acceptance.
 * @throws Nothing; enum values own no resources.
 * @note Ownership/thread-safety: values are local to one property sequence.
 */
enum class command_kind {
    /// Construct an owner when absent, or replace it when no borrow is live.
    construct_owner,
    /// Destroy the owner when no borrow is live.
    drop_owner,
    /// Move-construct the owner into a fresh owner object.
    move_construct_owner,
    /// Move-assign the owner into a fresh owner object.
    move_assign_owner,
    /// Mutate through direct owner access when no borrow is live.
    direct_owner_write,
    /// Request an immutable `Ref<T>` from the owner.
    borrow_shared,
    /// Copy one existing immutable borrow.
    copy_shared,
    /// Move one existing immutable borrow.
    move_shared,
    /// Drop one existing immutable borrow.
    drop_shared,
    /// Request an exclusive `MutRef<T>` from the owner.
    borrow_mutable,
    /// Move the existing exclusive mutable borrow.
    move_mutable,
    /// Mutate the payload through the existing exclusive mutable borrow.
    write_mutable,
    /// Drop the existing exclusive mutable borrow.
    drop_mutable
};

/**
 * @brief Tracked payload stored inside the generated `memsafe::Owner`.
 *
 * @details
 * The payload is intentionally small and trivially movable so property failures
 * point at borrow bookkeeping rather than payload behavior. The mutable integer
 * lets the test verify that reads through `Ref<T>`, writes through
 * `MutRef<T>`, owner moves, and owner replacement all agree on one model value.
 *
 * Example:
 * @code
 * tracked_value value(7);
 * value.payload = 9;
 * @endcode
 *
 * @pre The constructor accepts any signed 64-bit payload value.
 * @post `payload` equals the supplied constructor argument.
 * @invariant `payload` is the only modeled user data and is updated only by
 * generated owner or mutable-borrow write commands.
 * @throws Nothing; construction and destruction are trivial.
 * @note Ownership/thread-safety: instances are uniquely owned by one
 * `memsafe::Owner<tracked_value>` and used on one test thread.
 */
struct tracked_value final {
    /**
     * @brief Construct a payload with the generated value.
     *
     * @param initial_value Value stored in `payload`.
     * @return Constructors do not return a value.
     * @pre None.
     * @post `payload == initial_value`.
     * @invariant Construction performs no allocation and touches no shared
     * state, keeping generated sequence failures focused on borrow behavior.
     * @throws Nothing.
     * @note Ownership/thread-safety: the object is intended to be owned by one
     * single-threaded `memsafe::Owner<tracked_value>`.
     */
    explicit tracked_value(std::int64_t initial_value) noexcept
        : payload(initial_value) {}

    /// Current generated payload value expected by the sequence model.
    std::int64_t payload;
};

/**
 * @brief Mutable model and live objects for one generated property sequence.
 *
 * @pre Borrow vectors must be cleared before the owner is reset unless the
 * command explicitly moves the owner and preserves the control block.
 * @post Destruction clears mutable refs, then immutable refs, then the owner,
 * matching the declaration order and avoiding dangling borrow-control pointers.
 * @invariant `mutable_refs.size()` is either zero or one, and a nonzero value
 * requires `shared_refs.empty()`.
 * @throws Vector growth and owner allocation can throw `std::bad_alloc`; the
 * property driver converts escaping allocation failures into deterministic
 * failures with seed metadata.
 * @note Ownership/thread-safety: the state is stack-owned by one generated
 * sequence and accessed by one thread.
 */
struct sequence_state final {
    /// Owner type under test for this property.
    using owner_type = memsafe::Owner<tracked_value>;
    /// Immutable borrow type under test for this property.
    using shared_ref_type = memsafe::Ref<tracked_value>;
    /// Exclusive mutable borrow type under test for this property.
    using mutable_ref_type = memsafe::MutRef<tracked_value>;

    /// Current owner, if the generated sequence has a live owner.
    std::unique_ptr<owner_type> owner;
    /// Live immutable borrows held by the generated model.
    std::vector<shared_ref_type> shared_refs;
    /// Live exclusive mutable borrow held by the generated model.
    std::vector<mutable_ref_type> mutable_refs;
    /// Payload value that every live owner or borrow should currently observe.
    std::int64_t expected_payload = 0;
};

/**
 * @brief Result of executing one generated Owner/Ref/MutRef command sequence.
 *
 * @pre Default construction represents a passing sequence before any commands
 * have been executed.
 * @post Fields identify the first detected invariant failure, if any.
 * @invariant `ok == false` implies `reason` names the violated invariant and
 * `failing_step` identifies the one-based command prefix that exposed it.
 * @throws Nothing; the aggregate owns no dynamic storage.
 * @note Ownership/thread-safety: owned by one property sequence.
 */
struct sequence_report final {
    /// True when every checked borrow invariant held.
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
 * @brief Mark a sequence report as failed if it has not failed already.
 *
 * @param report Report to update.
 * @param step One-based command index associated with the failure.
 * @param reason Static diagnostic string naming the violated invariant.
 * @return No value.
 * @pre `reason != nullptr`.
 * @post The first failure is preserved and later failures do not overwrite it.
 * @invariant A sequence reports only the earliest observed invariant failure,
 * which keeps shrink diagnostics stable across replays.
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
 * @brief Generate the next tracked payload value for a sequence.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return Signed payload value in the generated test domain.
 * @pre `rng` is the deterministic engine supplied to the sequence.
 * @post `rng` has advanced by one draw.
 * @invariant Values are bounded so diagnostics remain readable while still
 * changing often enough to expose stale reads after moves or drops.
 * @throws Nothing.
 * @note Ownership/thread-safety: local deterministic random generation only.
 */
std::int64_t draw_payload(std::mt19937_64& rng) noexcept {
    return static_cast<std::int64_t>(rng() % k_payload_value_modulus);
}

/**
 * @brief Draw a generated command kind from the deterministic engine.
 *
 * @param rng Random engine owned by the active property sequence.
 * @return One command kind from the Owner/Ref/MutRef command set.
 * @pre `rng` is the deterministic engine supplied to the sequence.
 * @post `rng` has advanced by one draw.
 * @invariant The number of cases mirrors the `command_kind` enumerators so
 * every construct, borrow, move, and drop operation has a stable draw path.
 * @throws Nothing.
 * @note Ownership/thread-safety: local deterministic random generation only.
 */
command_kind draw_command(std::mt19937_64& rng) noexcept {
    switch (bounded_random(rng, 13u)) {
        case 0u:
            return command_kind::construct_owner;
        case 1u:
            return command_kind::drop_owner;
        case 2u:
            return command_kind::move_construct_owner;
        case 3u:
            return command_kind::move_assign_owner;
        case 4u:
            return command_kind::direct_owner_write;
        case 5u:
            return command_kind::borrow_shared;
        case 6u:
            return command_kind::copy_shared;
        case 7u:
            return command_kind::move_shared;
        case 8u:
            return command_kind::drop_shared;
        case 9u:
            return command_kind::borrow_mutable;
        case 10u:
            return command_kind::move_mutable;
        case 11u:
            return command_kind::write_mutable;
        default:
            return command_kind::drop_mutable;
    }
}

/**
 * @brief Report whether the generated model has any live borrow handle.
 *
 * @param state Generated sequence state to inspect.
 * @retval true At least one immutable or mutable borrow is live.
 * @retval false No generated borrow handle is live.
 * @pre `state` is alive.
 * @post No state is modified.
 * @invariant The model treats `mutable_refs` as a zero-or-one vector so this
 * helper is equivalent to the public borrow invariant's "any borrow" concept.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads sequence-local containers only.
 */
bool has_live_borrow(const sequence_state& state) noexcept {
    return !state.shared_refs.empty() || !state.mutable_refs.empty();
}

/**
 * @brief Ensure a sequence has a live owner before a command needs one.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to initialize a newly constructed owner.
 * @return No value.
 * @pre If `state.owner` is null, no live generated borrow may exist.
 * @post `state.owner` is non-null and `expected_payload` matches its payload.
 * @invariant Construction is the only way a no-owner sequence returns to the
 * active owner state.
 * @throws `std::bad_alloc` if owner allocation fails.
 * @note Ownership/thread-safety: the new owner is sequence-local and
 * single-threaded.
 */
void ensure_owner(sequence_state& state, std::mt19937_64& rng) {
    if (!state.owner) {
        state.expected_payload = draw_payload(rng);
        state.owner = std::make_unique<sequence_state::owner_type>(
            tracked_value(state.expected_payload));
    }
}

/**
 * @brief Return whether a generated borrow attempt reports exclusivity.
 *
 * @tparam BorrowAttempt Callable type that performs one borrow attempt.
 * @param attempt Callable invoked exactly once.
 * @retval true The attempt threw `memsafe::violation` with
 * `violation_kind::borrow_exclusivity`.
 * @retval false The attempt succeeded, threw another memsafe violation kind, or
 * threw a non-memsafe exception.
 * @pre The callable must not retain references to local borrow temporaries it
 * creates.
 * @post Any borrow temporary created by the attempt has left scope before this
 * function returns.
 * @invariant Only borrow-exclusivity reports satisfy the generated negative
 * oracle; null access or other violation kinds indicate a broken model or API
 * contract.
 * @throws Nothing intentionally. Unexpected exceptions are caught and reported
 * as `false` so the caller can attach deterministic shrink metadata.
 * @note Ownership/thread-safety: executes synchronously on sequence-local
 * owner state.
 */
template <typename BorrowAttempt>
bool reports_borrow_exclusivity(BorrowAttempt&& attempt) noexcept {
    try {
        attempt();
    } catch (const memsafe::violation& caught) {
        return caught.kind() == memsafe::violation_kind::borrow_exclusivity;
    } catch (...) {
        return false;
    }

    return false;
}

/**
 * @brief Validate that one immutable borrow observes the model payload.
 *
 * @param ref Immutable borrow to inspect.
 * @param expected_payload Payload value expected by the model.
 * @retval true The borrow dereferenced successfully and observed the expected
 * payload.
 * @retval false Dereference threw or returned an unexpected value.
 * @pre `ref` is expected to point at the live owner control block.
 * @post No borrow state is modified.
 * @invariant Immutable borrows never mutate the payload while being checked.
 * @throws Nothing intentionally; violation exceptions are caught and converted
 * to `false`.
 * @note Ownership/thread-safety: reads one sequence-local borrow handle.
 */
bool shared_ref_matches_payload(const sequence_state::shared_ref_type& ref,
                                std::int64_t expected_payload) noexcept {
    try {
        return ref->payload == expected_payload;
    } catch (...) {
        return false;
    }
}

/**
 * @brief Validate that the mutable borrow observes the model payload.
 *
 * @param ref Mutable borrow to inspect.
 * @param expected_payload Payload value expected by the model.
 * @retval true The borrow dereferenced successfully and observed the expected
 * payload.
 * @retval false Dereference threw or returned an unexpected value.
 * @pre `ref` is expected to point at the live owner control block.
 * @post No borrow state is modified.
 * @invariant The check reads through mutable access without changing the
 * payload so it is safe after every generated step.
 * @throws Nothing intentionally; violation exceptions are caught and converted
 * to `false`.
 * @note Ownership/thread-safety: reads one sequence-local borrow handle.
 */
bool mutable_ref_matches_payload(const sequence_state::mutable_ref_type& ref,
                                 std::int64_t expected_payload) noexcept {
    try {
        return ref->payload == expected_payload;
    } catch (...) {
        return false;
    }
}

/**
 * @brief Verify all live immutable refs still read the model payload.
 *
 * @param state Generated sequence state to inspect.
 * @retval true Every live immutable ref reads `state.expected_payload`.
 * @retval false At least one ref is stale, empty, or otherwise invalid.
 * @pre The generated owner control block outlives all refs in `state`.
 * @post No state is modified.
 * @invariant A move of `Owner<T>` must not invalidate existing `Ref<T>`
 * handles, so every ref is checked after every command.
 * @throws Nothing intentionally; violation exceptions are converted to false
 * by `shared_ref_matches_payload`.
 * @note Ownership/thread-safety: reads sequence-local refs only.
 */
bool all_shared_refs_match_payload(const sequence_state& state) noexcept {
    for (const sequence_state::shared_ref_type& ref : state.shared_refs) {
        if (!shared_ref_matches_payload(ref, state.expected_payload)) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Assert the public borrow invariant after one generated step.
 *
 * @param state Generated sequence state to inspect and temporarily probe.
 * @param report Sequence report updated on the first detected failure.
 * @param step One-based command step being checked.
 * @return No value.
 * @pre `state` is a live generated sequence state; borrow vectors must contain
 * only handles created from `state.owner`.
 * @post The first invariant failure, if any, is recorded in `report`. Temporary
 * probe borrows have been destroyed before return.
 * @invariant The model and public API agree on the borrow state after every
 * command: shared borrows coexist only with shared borrows, mutable borrows
 * exclude all other borrows, and no-borrow state admits either borrow kind.
 * @throws Nothing intentionally. Unexpected exceptions from public borrow or
 * dereference operations are caught and recorded as failures.
 * @note Ownership/thread-safety: all probes are synchronous and
 * sequence-local. The expected failing probes rely on THROW policy selected at
 * the top of this translation unit.
 */
void assert_borrow_invariant(sequence_state& state,
                             sequence_report& report,
                             std::size_t step) noexcept {
    if (!report.ok) {
        return;
    }

    if (state.mutable_refs.size() > 1u) {
        fail_sequence(report, step, "model held more than one mutable borrow");
        return;
    }
    if (!state.mutable_refs.empty() && !state.shared_refs.empty()) {
        fail_sequence(report, step, "model held shared and mutable borrows");
        return;
    }
    if (!state.owner) {
        if (has_live_borrow(state)) {
            fail_sequence(report, step, "model held borrows without an owner");
        }
        return;
    }
    if (!state.owner->has_value()) {
        fail_sequence(report, step, "current owner unexpectedly empty");
        return;
    }

    if (!state.mutable_refs.empty()) {
        if (!mutable_ref_matches_payload(state.mutable_refs.front(),
                                         state.expected_payload)) {
            fail_sequence(report, step, "mutable borrow observed wrong payload");
            return;
        }

        /*
         * F2 Ownership case 2 requires every shared/exclusive overlap to report
         * through the configured violation policy. Probing here after each step
         * proves the exclusive token is still held while the model says it is.
         */
        const bool rejected_shared = reports_borrow_exclusivity([&]() {
            sequence_state::shared_ref_type blocked = state.owner->borrow();
            (void)blocked;
        });
        if (!rejected_shared) {
            fail_sequence(report, step,
                          "shared borrow succeeded while mutable borrow live");
        }
        return;
    }

    if (!state.shared_refs.empty()) {
        if (!all_shared_refs_match_payload(state)) {
            fail_sequence(report, step, "shared borrow observed wrong payload");
            return;
        }

        try {
            sequence_state::shared_ref_type extra_shared = state.owner->borrow();
            if (extra_shared->payload != state.expected_payload) {
                fail_sequence(report, step,
                              "additional shared borrow observed wrong payload");
                return;
            }
        } catch (...) {
            fail_sequence(report, step,
                          "additional shared borrow failed with shared refs live");
            return;
        }

        const bool rejected_mutable = reports_borrow_exclusivity([&]() {
            sequence_state::mutable_ref_type blocked = state.owner->borrow_mut();
            (void)blocked;
        });
        if (!rejected_mutable) {
            fail_sequence(report, step,
                          "mutable borrow succeeded while shared refs live");
        }
        return;
    }

    try {
        sequence_state::shared_ref_type shared_probe = state.owner->borrow();
        if (shared_probe->payload != state.expected_payload) {
            fail_sequence(report, step,
                          "shared borrow observed wrong payload with no borrows");
            return;
        }
    } catch (...) {
        fail_sequence(report, step, "shared borrow failed with no borrows live");
        return;
    }

    try {
        sequence_state::mutable_ref_type mutable_probe =
            state.owner->borrow_mut();
        if (mutable_probe->payload != state.expected_payload) {
            fail_sequence(report, step,
                          "mutable borrow observed wrong payload with no borrows");
            return;
        }
        mutable_probe->payload = state.expected_payload;
    } catch (...) {
        fail_sequence(report, step, "mutable borrow failed with no borrows live");
    }
}

/**
 * @brief Replace or construct the owner when the model can safely do so.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to initialize the new owner.
 * @param report Sequence report updated on failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre Replacement is skipped when live borrows exist because destroying an
 * owner with live non-owning borrows violates the Slice 1 API precondition.
 * @post If no borrow was live, `state.owner` owns a newly constructed payload.
 * @invariant Construct commands exercise owner destruction and construction
 * only at valid lifetime boundaries.
 * @throws `std::bad_alloc` if owner allocation fails.
 * @note Ownership/thread-safety: mutates sequence-local owner state only.
 */
void construct_owner(sequence_state& state,
                     std::mt19937_64& rng,
                     sequence_report& report,
                     std::size_t step) {
    (void)report;
    (void)step;
    if (has_live_borrow(state)) {
        return;
    }

    state.expected_payload = draw_payload(rng);
    state.owner = std::make_unique<sequence_state::owner_type>(
        tracked_value(state.expected_payload));
}

/**
 * @brief Drop the generated owner at a valid lifetime boundary.
 *
 * @param state Generated sequence state to update.
 * @return No value.
 * @pre The owner is reset only when no generated borrow is live.
 * @post `state.owner` is null when a live owner was present and no borrow was
 * live.
 * @invariant Owner destruction is never used to create dangling generated
 * borrow handles; borrow drops are modeled separately.
 * @throws Nothing.
 * @note Ownership/thread-safety: mutates sequence-local owner state only.
 */
void drop_owner(sequence_state& state) noexcept {
    if (!has_live_borrow(state)) {
        state.owner.reset();
    }
}

/**
 * @brief Move-construct the current owner into a fresh owner object.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used if an owner must first be constructed.
 * @param report Sequence report updated on move-contract failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre Existing borrows may be live because moving `Owner<T>` preserves the
 * control-block address they reference.
 * @post The current owner remains live and contains the pre-move payload value.
 * @invariant The moved-from owner reports empty and the moved-to owner reports
 * live after the move constructor.
 * @throws `std::bad_alloc` if owner allocation fails.
 * @note Ownership/thread-safety: moves sequence-local owner state only.
 */
void move_construct_owner(sequence_state& state,
                          std::mt19937_64& rng,
                          sequence_report& report,
                          std::size_t step) {
    ensure_owner(state, rng);

    sequence_state::owner_type& source = *state.owner;
    std::unique_ptr<sequence_state::owner_type> moved =
        std::make_unique<sequence_state::owner_type>(std::move(source));

    if (source.has_value()) {
        fail_sequence(report, step, "move-constructed source owner still live");
        return;
    }
    if (!moved->has_value()) {
        fail_sequence(report, step, "move-constructed destination owner empty");
        return;
    }

    state.owner = std::move(moved);
}

/**
 * @brief Move-assign the current owner into a fresh owner object.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to construct the temporary target owner.
 * @param report Sequence report updated on move-contract failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre Existing borrows may point into the source owner; the temporary target
 * has no borrows before assignment.
 * @post The current owner remains live and contains the pre-assignment payload
 * value.
 * @invariant Move assignment empties the source owner and leaves the target
 * owning the original control block, preserving existing borrow handles.
 * @throws `std::bad_alloc` if owner allocation fails.
 * @note Ownership/thread-safety: moves sequence-local owner state only.
 */
void move_assign_owner(sequence_state& state,
                       std::mt19937_64& rng,
                       sequence_report& report,
                       std::size_t step) {
    ensure_owner(state, rng);

    std::unique_ptr<sequence_state::owner_type> target =
        std::make_unique<sequence_state::owner_type>(
            tracked_value(draw_payload(rng)));
    sequence_state::owner_type& source = *state.owner;
    *target = std::move(source);

    if (source.has_value()) {
        fail_sequence(report, step, "move-assigned source owner still live");
        return;
    }
    if (!target->has_value()) {
        fail_sequence(report, step, "move-assigned destination owner empty");
        return;
    }

    state.owner = std::move(target);
}

/**
 * @brief Mutate the payload directly through the owner when no borrow is live.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to generate the new payload.
 * @return No value.
 * @pre Direct owner mutation is skipped while borrow handles are live so the
 * generated model does not bypass the borrow API it is testing.
 * @post When an owner exists and no borrow is live, the owner payload and model
 * payload are updated to the same generated value.
 * @invariant The command keeps public owner dereference coverage separate from
 * `MutRef<T>` coverage.
 * @throws `std::bad_alloc` if the helper must construct a missing owner.
 * @note Ownership/thread-safety: mutates sequence-local payload state only.
 */
void direct_owner_write(sequence_state& state, std::mt19937_64& rng) {
    if (has_live_borrow(state)) {
        return;
    }

    ensure_owner(state, rng);
    state.expected_payload = draw_payload(rng);
    (*state.owner)->payload = state.expected_payload;
}

/**
 * @brief Attempt to add one immutable borrow to the generated model.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used if an owner must first be constructed.
 * @param report Sequence report updated on an unexpected borrow result.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command may run with or without a live mutable borrow.
 * @post If no mutable borrow was live, one `Ref<T>` is appended. If a mutable
 * borrow was live, the attempt must report borrow exclusivity.
 * @invariant Shared borrows may coexist only with other shared borrows.
 * @throws `std::bad_alloc` if owner allocation or vector growth fails.
 * @note Ownership/thread-safety: mutates sequence-local borrow vectors only.
 */
void borrow_shared(sequence_state& state,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) {
    ensure_owner(state, rng);

    if (!state.mutable_refs.empty()) {
        const bool rejected = reports_borrow_exclusivity([&]() {
            sequence_state::shared_ref_type blocked = state.owner->borrow();
            (void)blocked;
        });
        if (!rejected) {
            fail_sequence(report, step,
                          "borrow() did not reject live mutable borrow");
        }
        return;
    }

    try {
        sequence_state::shared_ref_type ref = state.owner->borrow();
        if (ref->payload != state.expected_payload) {
            fail_sequence(report, step,
                          "new shared borrow observed wrong payload");
            return;
        }
        state.shared_refs.push_back(std::move(ref));
    } catch (...) {
        fail_sequence(report, step, "borrow() failed without mutable borrow");
    }
}

/**
 * @brief Copy one existing immutable borrow.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to choose the copied borrow.
 * @param report Sequence report updated on copy failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command is a no-op when no immutable borrow is live.
 * @post One additional immutable borrow is appended when a source exists.
 * @invariant Copying `Ref<T>` increments the implementation shared-borrow
 * count while preserving the model payload.
 * @throws `std::bad_alloc` if vector growth fails.
 * @note Ownership/thread-safety: mutates sequence-local immutable-borrow vector
 * only.
 */
void copy_shared(sequence_state& state,
                 std::mt19937_64& rng,
                 sequence_report& report,
                 std::size_t step) {
    if (state.shared_refs.empty()) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.shared_refs.size());
    try {
        sequence_state::shared_ref_type copied = state.shared_refs[index];
        if (copied->payload != state.expected_payload) {
            fail_sequence(report, step,
                          "copied shared borrow observed wrong payload");
            return;
        }
        state.shared_refs.push_back(std::move(copied));
    } catch (...) {
        fail_sequence(report, step, "copying Ref failed unexpectedly");
    }
}

/**
 * @brief Move one existing immutable borrow within the generated model.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to choose the moved borrow.
 * @param report Sequence report updated on move failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command is a no-op when no immutable borrow is live.
 * @post The number of immutable borrows is unchanged, but one handle has been
 * moved out of and back into the vector.
 * @invariant Moving `Ref<T>` transfers a counted borrow without creating or
 * destroying an implementation count entry.
 * @throws `std::bad_alloc` if vector growth during reinsertion fails.
 * @note Ownership/thread-safety: mutates sequence-local immutable-borrow vector
 * only.
 */
void move_shared(sequence_state& state,
                 std::mt19937_64& rng,
                 sequence_report& report,
                 std::size_t step) {
    if (state.shared_refs.empty()) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.shared_refs.size());
    try {
        sequence_state::shared_ref_type moved =
            std::move(state.shared_refs[index]);
        state.shared_refs.erase(state.shared_refs.begin() +
                                static_cast<std::ptrdiff_t>(index));
        state.shared_refs.push_back(std::move(moved));
    } catch (...) {
        fail_sequence(report, step, "moving Ref failed unexpectedly");
    }
}

/**
 * @brief Drop one immutable borrow from the generated model.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to choose the dropped borrow.
 * @return No value.
 * @pre The command is a no-op when no immutable borrow is live.
 * @post At most one immutable borrow has been destroyed.
 * @invariant Dropping a `Ref<T>` releases one shared count and may enable a
 * later mutable borrow when the last shared ref is dropped.
 * @throws Nothing under the current `Ref<T>` move/destruction contract.
 * @note Ownership/thread-safety: mutates sequence-local immutable-borrow vector
 * only.
 */
void drop_shared(sequence_state& state, std::mt19937_64& rng) {
    if (state.shared_refs.empty()) {
        return;
    }

    const std::size_t index = bounded_random(rng, state.shared_refs.size());
    state.shared_refs.erase(state.shared_refs.begin() +
                            static_cast<std::ptrdiff_t>(index));
}

/**
 * @brief Attempt to add one exclusive mutable borrow to the generated model.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used if an owner must first be constructed.
 * @param report Sequence report updated on an unexpected borrow result.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command may run while no borrow, shared borrows, or a mutable borrow
 * are live.
 * @post If no borrow was live, one `MutRef<T>` is appended. Otherwise the
 * attempt must report borrow exclusivity.
 * @invariant Mutable borrowing succeeds only from the no-borrow state.
 * @throws `std::bad_alloc` if owner allocation or vector growth fails.
 * @note Ownership/thread-safety: mutates sequence-local mutable-borrow vector
 * only.
 */
void borrow_mutable(sequence_state& state,
                    std::mt19937_64& rng,
                    sequence_report& report,
                    std::size_t step) {
    ensure_owner(state, rng);

    if (has_live_borrow(state)) {
        const bool rejected = reports_borrow_exclusivity([&]() {
            sequence_state::mutable_ref_type blocked =
                state.owner->borrow_mut();
            (void)blocked;
        });
        if (!rejected) {
            fail_sequence(report, step,
                          "borrow_mut() did not reject existing borrow");
        }
        return;
    }

    try {
        sequence_state::mutable_ref_type ref = state.owner->borrow_mut();
        if (ref->payload != state.expected_payload) {
            fail_sequence(report, step,
                          "new mutable borrow observed wrong payload");
            return;
        }
        state.mutable_refs.push_back(std::move(ref));
    } catch (...) {
        fail_sequence(report, step, "borrow_mut() failed with no borrows live");
    }
}

/**
 * @brief Move the existing exclusive mutable borrow.
 *
 * @param state Generated sequence state to update.
 * @param report Sequence report updated on move failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command is a no-op when no mutable borrow is live.
 * @post The number of mutable borrows is unchanged, and the moved-to handle
 * still owns the exclusive token.
 * @invariant Moving `MutRef<T>` transfers the exclusive token without creating
 * another mutable borrow.
 * @throws `std::bad_alloc` if vector growth during reinsertion fails.
 * @note Ownership/thread-safety: mutates sequence-local mutable-borrow vector
 * only.
 */
void move_mutable(sequence_state& state,
                  sequence_report& report,
                  std::size_t step) {
    if (state.mutable_refs.empty()) {
        return;
    }

    try {
        sequence_state::mutable_ref_type moved =
            std::move(state.mutable_refs.front());
        state.mutable_refs.clear();
        state.mutable_refs.push_back(std::move(moved));
    } catch (...) {
        fail_sequence(report, step, "moving MutRef failed unexpectedly");
    }
}

/**
 * @brief Mutate the payload through the live exclusive mutable borrow.
 *
 * @param state Generated sequence state to update.
 * @param rng Random engine used to generate the new payload value.
 * @param report Sequence report updated on mutation failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre The command is a no-op when no mutable borrow is live.
 * @post When a mutable borrow is live, both the payload and model value are
 * updated to the same generated value.
 * @invariant Only `MutRef<T>` write commands mutate while a borrow is live.
 * @throws Nothing intentionally; memsafe violations are caught and reported as
 * property failures.
 * @note Ownership/thread-safety: mutates sequence-local payload state only.
 */
void write_mutable(sequence_state& state,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) noexcept {
    if (state.mutable_refs.empty()) {
        return;
    }

    try {
        state.expected_payload = draw_payload(rng);
        state.mutable_refs.front()->payload = state.expected_payload;
    } catch (...) {
        fail_sequence(report, step, "writing through MutRef failed");
    }
}

/**
 * @brief Drop the existing exclusive mutable borrow.
 *
 * @param state Generated sequence state to update.
 * @return No value.
 * @pre The command is a no-op when no mutable borrow is live.
 * @post No mutable borrow remains live.
 * @invariant Dropping `MutRef<T>` releases the exclusive token and may enable a
 * later shared or mutable borrow.
 * @throws Nothing under the current `MutRef<T>` destruction contract.
 * @note Ownership/thread-safety: mutates sequence-local mutable-borrow vector
 * only.
 */
void drop_mutable(sequence_state& state) noexcept {
    state.mutable_refs.clear();
}

/**
 * @brief Apply one generated command to the Owner/Ref/MutRef model.
 *
 * @param state Generated sequence state to update.
 * @param command Command to execute.
 * @param rng Random engine owned by the active sequence.
 * @param report Sequence report updated on command failure.
 * @param step One-based command step being executed.
 * @return No value.
 * @pre `state` satisfies the model invariant before dispatch.
 * @post Either the command has updated `state`, or the first failure has been
 * recorded in `report`.
 * @invariant Commands that would destroy an owner with live borrows are skipped
 * because the Slice 1 public API documents that as a caller precondition rather
 * than a checked violation path.
 * @throws `std::bad_alloc` if owner allocation or vector growth fails.
 * @note Ownership/thread-safety: all state is local to the generated sequence.
 */
void apply_command(sequence_state& state,
                   command_kind command,
                   std::mt19937_64& rng,
                   sequence_report& report,
                   std::size_t step) {
    switch (command) {
        case command_kind::construct_owner:
            construct_owner(state, rng, report, step);
            break;
        case command_kind::drop_owner:
            drop_owner(state);
            break;
        case command_kind::move_construct_owner:
            move_construct_owner(state, rng, report, step);
            break;
        case command_kind::move_assign_owner:
            move_assign_owner(state, rng, report, step);
            break;
        case command_kind::direct_owner_write:
            direct_owner_write(state, rng);
            break;
        case command_kind::borrow_shared:
            borrow_shared(state, rng, report, step);
            break;
        case command_kind::copy_shared:
            copy_shared(state, rng, report, step);
            break;
        case command_kind::move_shared:
            move_shared(state, rng, report, step);
            break;
        case command_kind::drop_shared:
            drop_shared(state, rng);
            break;
        case command_kind::borrow_mutable:
            borrow_mutable(state, rng, report, step);
            break;
        case command_kind::move_mutable:
            move_mutable(state, report, step);
            break;
        case command_kind::write_mutable:
            write_mutable(state, rng, report, step);
            break;
        case command_kind::drop_mutable:
            drop_mutable(state);
            break;
    }
}

/**
 * @brief Execute one deterministic generated Owner/Ref/MutRef sequence.
 *
 * @param rng Random engine seeded by `property_driver.hpp` for this sequence.
 * @param prefix_limit Maximum number of generated commands to execute, or
 * `k_full_sequence_prefix` to execute the entire generated sequence.
 * @return Sequence report containing pass/fail status and replay metadata.
 * @pre `rng` must be the per-sequence engine supplied by
 * `memsafe::test_support::property::run_sequences`, or another engine seeded
 * with the reported failing sequence seed.
 * @post All live generated borrows are dropped before the owner is destroyed.
 * @invariant The borrow invariant is asserted after every generated command
 * through public `Owner<T>`, `Ref<T>`, and `MutRef<T>` operations.
 * @throws `std::bad_alloc` if allocation for owner or borrow storage fails.
 * The property driver catches escaping exceptions and records seed metadata.
 * @note Ownership/thread-safety: all objects and borrows are local to the
 * sequence and single-threaded.
 */
sequence_report run_owner_sequence(std::mt19937_64& rng,
                                   std::size_t prefix_limit) {
    sequence_report report;
    report.generated_steps =
        1u + bounded_random(rng, k_max_commands_per_sequence);
    report.executed_steps = report.generated_steps < prefix_limit
                                ? report.generated_steps
                                : prefix_limit;

    sequence_state state;

    for (std::size_t step = 1u; step <= report.executed_steps; ++step) {
        const command_kind command = draw_command(rng);

        /*
         * CPP_MEMSAFE-0140-TEST requires generated construct/borrow/move/drop
         * sequences. Each command mutates the model first, then the invariant
         * probe checks the public API state produced by that exact prefix.
         */
        apply_command(state, command, rng, report, step);
        assert_borrow_invariant(state, report, step);

        if (!report.ok) {
            break;
        }
    }

    /*
     * Borrow handles are non-owning. Explicit cleanup keeps replay prefixes at
     * valid lifetime boundaries even when the last generated command left a
     * borrow live.
     */
    state.mutable_refs.clear();
    state.shared_refs.clear();
    state.owner.reset();

    return report;
}

/**
 * @brief Replay one generated sequence from a deterministic sequence seed.
 *
 * @param seed Seed reported by `property_driver.hpp` for a specific sequence.
 * @param prefix_limit Command prefix to execute before cleanup.
 * @return Sequence report for the replayed prefix.
 * @pre `seed` must be the per-sequence seed, not the property run base seed.
 * @post No global state is modified.
 * @invariant Replays use the same command generator as the property body.
 * @throws `std::bad_alloc` under the same conditions as `run_owner_sequence`.
 * @note Ownership/thread-safety: all replay state is local to the call.
 */
sequence_report replay_owner_sequence(std::uint64_t seed,
                                      std::size_t prefix_limit) {
    std::mt19937_64 rng(seed);
    return run_owner_sequence(rng, prefix_limit);
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
        if (!replay_owner_sequence(seed, prefix).ok) {
            return prefix;
        }
    }

    return original_steps;
}

/**
 * @brief Execute one named non-stateful Owner/Ref/MutRef property target.
 *
 * @param name Stable property name used for seed derivation and diagnostics.
 * @param salt Stable salt separating the Owner, Ref, and MutRef target runs.
 * @return Property-driver result for the named target.
 * @pre `name != nullptr` and remains valid for the duration of the call.
 * @post At least `property::sequence_threshold(non_stateful)` sequences have
 * executed on a passing run.
 * @invariant Each named target uses the same integrated command sequence model
 * but a distinct deterministic seed stream, satisfying the F2 per-type
 * non-stateful count for Owner, Ref, and MutRef.
 * @throws `std::bad_alloc` if property configuration or sequence execution
 * allocation fails.
 * @note Ownership/thread-safety: all property state is local to this call.
 */
memsafe::test_support::property::run_result run_named_property(
    const char* name,
    std::uint64_t salt) {
    namespace prop = memsafe::test_support::property;

    prop::run_config config =
        prop::make_config(name, prop::workload::non_stateful, salt);

    return prop::run_sequences(
        config,
        [](std::size_t sequence,
           std::mt19937_64& rng,
           prop::shrinking_metadata& shrink) {
            const sequence_report report =
                run_owner_sequence(rng, k_full_sequence_prefix);
            if (report.ok) {
                return true;
            }

            const std::size_t minimal_prefix =
                find_minimal_failing_prefix(shrink.seed,
                                            report.generated_steps);
            prop::record_shrink(shrink,
                                report.generated_steps,
                                minimal_prefix,
                                shrink.seed,
                                sequence,
                                report.reason);
            return false;
        });
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
 * @brief Check one completed property result against threshold requirements.
 *
 * @param result Result returned by `run_named_property`.
 * @return No value.
 * @pre `result` was produced by this translation unit's non-stateful property
 * configuration.
 * @post Harness failures are recorded for property failures or insufficient
 * sequence budgets.
 * @invariant A passing run must execute the property driver's active
 * non-stateful threshold, which is 10 000 normally and at least 100 000 when a
 * supported nightly multiplier macro is defined.
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
 * @brief Run the Owner/Ref/MutRef non-stateful property tests.
 *
 * @retval 0 All generated sequences satisfied the borrow invariants.
 * @retval 1 One or more harness checks failed, with replay seed and shrunk
 * prefix reported for property failures.
 * @pre The executable is run by the F4 property lane, which discovers
 * `testing/tests/property/*.cpp` and defines any lane-specific property macros
 * before including `property_driver.hpp`.
 * @post The process exit code is the test verdict consumed by CTest.
 * @invariant Three named non-stateful property targets execute, one each for
 * Owner, Ref, and MutRef traceability, and each target receives the property
 * driver's full non-stateful sequence threshold.
 * @throws The test does not intentionally throw. Unexpected allocation
 * failures inside the property body are caught by `property_driver.hpp` and
 * surfaced as property failures with seed metadata.
 * @note Ownership/thread-safety: all test state is automatic storage or
 * property-driver local state; no worker threads are started.
 */
int main() {
    const memsafe::test_support::property::run_result owner_result =
        run_named_property("owner.construct-borrow-move-drop", 0x014001u);
    const memsafe::test_support::property::run_result ref_result =
        run_named_property("ref.copy-borrow-drop", 0x014002u);
    const memsafe::test_support::property::run_result mutref_result =
        run_named_property("mutref.exclusive-borrow-move-drop", 0x014003u);

    check_property_result(owner_result);
    check_property_result(ref_result);
    check_property_result(mutref_result);

    RUN_TESTS("test_owner_property");
}
