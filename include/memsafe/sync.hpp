/**
 * @file sync.hpp
 * @brief Slice 4 sync borrows plus Slice 5 Arc and Mutex primitives.
 *
 * @details
 * Work package: CPP_MEMSAFE-0500-FUNC, extending the finalized
 * CPP_MEMSAFE-0400-FUNC sync-borrow header.
 *
 * Purpose:
 * - Define `memsafe::SyncOwner<T>`, the move-only RAII owner for one
 *   heap-allocated `T` with atomic borrow accounting.
 * - Define `memsafe::SyncRef<T>`, the copyable immutable borrow returned by
 *   `SyncOwner<T>::borrow()`.
 * - Define `memsafe::SyncMutRef<T>`, the move-only exclusive mutable borrow
 *   returned by `SyncOwner<T>::borrow_mut()`.
 * - Provide `memsafe::detail::sync_borrow_ctrl<T>`, the private control object
 *   that stores the payload and its Slice 4 atomic borrow state.
 * - Define `memsafe::Arc<T>`, an atomically reference-counted shared owner for
 *   cross-thread ownership sharing.
 * - Define `memsafe::Mutex<T>`, the Slice 5 mutual-exclusion wrapper whose
 *   `lock()` method returns the project-wide `memsafe::MutRef<T>` type for
 *   serialized mutable access.
 *
 * Key invariants:
 * - `SyncOwner<T>` is non-copyable and has unconditionally `noexcept(true)`
 *   move construction and move assignment, matching F1 ownership semantics and
 *   the F3 `Move_Noexcept` property for Slice 4.
 * - `SyncRef<T>` and `SyncMutRef<T>` are type-level `MEMSAFE_NODISCARD`
 *   classes, and `SyncOwner<T>::borrow()` / `SyncOwner<T>::borrow_mut()` return
 *   them by value so ordinary unused-result diagnostics can catch discarded
 *   borrows.
 * - With `MEMSAFE_RELEASE_CHECKS != 0`, all borrow state lives in one
 *   `std::atomic<std::uint32_t>` word. `0` means no borrow is live, values
 *   below the mutable token count immutable borrows, and the mutable token means
 *   one exclusive mutable borrow is live.
 * - The single atomic state word is the synchronization point required by the
 *   AADL Slice 4 `Atomic_Refcount` property. It prevents the two-counter race
 *   where a shared borrow and a mutable borrow could each observe the other as
 *   absent before publishing their own state.
 * - With `MEMSAFE_RELEASE_CHECKS == 0`, counter fields and violation call sites
 *   are not compiled into the control block, preserving the release no-checks
 *   lane established by the earlier ownership packages.
 * - Borrow-returning functions and borrow-handle dereference paths carry
 *   `MEMSAFE_BORROWS(...)` so the Slice 7 Clang co_await lane can attach
 *   lifetime and coroutine lifetime annotations without changing this API.
 * - `Arc<T>` stores its strong count in one `std::atomic<std::uint64_t>`.
 *   Copying an `Arc<T>` increments that count atomically, destroying or
 *   assigning away an `Arc<T>` decrements it atomically, and the payload control
 *   block is deleted exactly once by the last release.
 * - `Mutex<T>::lock()` returns a type-level and function-level
 *   `MEMSAFE_NODISCARD` `MutRef<T>`. A private friend-only `MutRef<T>` release
 *   hook clears the mutex borrow ledger and unlocks the underlying
 *   `std::mutex` when the returned borrow is destroyed or assigned over.
 * - `Mutex<T>` maintains a debug-check borrow-state word in addition to the
 *   `std::mutex`. The mutex is the synchronization mechanism; the state word is
 *   the documented Slice 5 borrow ledger that records one live mutex-backed
 *   `MutRef<T>` and is cleared before unlock.
 *
 * Ownership and thread-safety:
 * - `SyncOwner<T>` uniquely owns the heap allocation. `SyncRef<T>` and
 *   `SyncMutRef<T>` are non-owning handles into the owner control block and
 *   must not be used after the owning `SyncOwner<T>` has been destroyed.
 * - Borrow acquisition, release, and observation are atomic and may be called
 *   from multiple threads while the owner remains alive and is not being moved.
 * - The atomic borrow ledger prevents overlapping counted mutable/shared
 *   borrows. It does not make arbitrary operations on `T` internally
 *   synchronized; callers still must obey the ordinary C++ data-race rules for
 *   the payload and should access it through the counted borrow handles when
 *   sharing a `SyncOwner<T>` across threads.
 * - Distinct `Arc<T>` handles may be copied and destroyed concurrently because
 *   their shared control block uses atomic reference counting. Access to the
 *   payload itself follows the ordinary C++ rules; use `Arc<Mutex<T>>` when
 *   mutable shared state must be serialized.
 * - `Mutex<T>` serializes all payload access performed through live lock
 *   handles. Destroying a mutex while a handle is live is outside the API
 *   contract, just as destroying `std::mutex` while locked is outside the
 *   standard mutex contract.
 */
#ifndef MEMSAFE_SYNC_HPP
#define MEMSAFE_SYNC_HPP

#include <memsafe/config.hpp>
#include <memsafe/backend.hpp>
#include <memsafe/owner.hpp>
#include <memsafe/violation.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace memsafe {

/**
 * @brief Move-only owner for a heap-allocated object with atomic borrow state.
 *
 * @tparam T Object type owned by the `SyncOwner`.
 * @pre `T` is a complete non-reference, non-array object type whenever a
 * `SyncOwner<T>` is constructed.
 * @post Construction creates one heap-allocated `T`; destruction releases it
 * exactly once after all borrow handles have ended.
 * @invariant At most one live `SyncOwner<T>` owns a given control block, and
 * all live sync borrow handles observe that stable control-block address.
 * @throws Public construction may throw allocation exceptions or exceptions
 * propagated from `T` construction. Move construction and move assignment are
 * unconditionally `noexcept(true)`.
 * @note Ownership/thread-safety: ownership is unique. Borrow accounting is
 * atomic and can be used across threads while the owner remains alive and is
 * not concurrently moved or destroyed.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> value(7);
 * auto read = value.borrow();
 * int copy = *read;
 * (void)copy;
 * {
 *     auto write = value.borrow_mut();
 *     *write = 9;
 * }
 * @endcode
 */
template <typename T>
class SyncOwner;

/**
 * @brief Immutable counted borrow of a `SyncOwner<T>` payload.
 *
 * @tparam T Object type borrowed from the sync owner.
 * @pre Instances are created by `SyncOwner<T>::borrow()` or trusted friends,
 * not by user code directly.
 * @post Copying a `SyncRef<T>` creates another immutable borrow in
 * debug-check lanes; destroying it atomically releases one immutable borrow.
 * @invariant A live `SyncRef<T>` never grants mutable access to the payload.
 * @throws Copy construction, copy assignment, and factory construction may
 * report `borrow_exclusivity` under THROW policy if an exclusive mutable borrow
 * is already live. Move operations and destruction do not throw.
 * @note Ownership/thread-safety: `SyncRef<T>` is non-owning. It may be copied,
 * moved, and destroyed on different threads from the creating thread while the
 * owning `SyncOwner<T>` remains alive.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> owner(5);
 * auto a = owner.borrow();
 * auto b = a;
 * int total = *a + *b;
 * (void)total;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD SyncRef;

/**
 * @brief Exclusive counted mutable borrow of a `SyncOwner<T>` payload.
 *
 * @tparam T Object type borrowed mutably from the sync owner.
 * @pre Instances are created by `SyncOwner<T>::borrow_mut()` or trusted
 * friends, not by user code directly.
 * @post Construction atomically acquires the exclusive token in debug-check
 * lanes; destruction releases it.
 * @invariant `SyncMutRef<T>` is non-copyable, and a successfully acquired
 * mutable borrow excludes both immutable and other mutable borrows while checks
 * are enabled.
 * @throws Factory construction may report `borrow_exclusivity` under THROW
 * policy. Move operations and destruction do not throw.
 * @note Ownership/thread-safety: `SyncMutRef<T>` is non-owning and movable
 * across threads. The owning `SyncOwner<T>` must outlive the handle, and the
 * caller must avoid accessing the same `T` outside the exclusive borrow.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> owner(5);
 * auto borrow = owner.borrow_mut();
 * *borrow = 8;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD SyncMutRef;

/**
 * @brief Atomically reference-counted shared owner for one heap-allocated
 * payload.
 *
 * @tparam T Object type stored in the shared `Arc` control block.
 * @pre `T` is a complete non-reference, non-array object type whenever an
 * owning `Arc<T>` is constructed.
 * @post Construction from payload arguments creates one shared control block
 * with strong count 1; copying increments the count; destruction decrements the
 * count and deletes the payload at count 0.
 * @invariant All non-empty `Arc<T>` handles that compare equal by `get()` share
 * one atomic strong count and one payload address.
 * @throws Payload construction may throw allocation or `T` construction
 * exceptions. Copying can report `capacity_exhausted` under THROW policy only
 * if the strong count is exhausted. Move operations and destruction do not
 * throw.
 * @note Ownership/thread-safety: distinct `Arc<T>` handles may be copied,
 * assigned, and destroyed concurrently. The payload itself is not internally
 * synchronized; wrap mutable shared state in `Mutex<T>`.
 *
 * Example:
 * @code
 * memsafe::Arc<int> first(7);
 * memsafe::Arc<int> second = first;
 * int copy = *second;
 * (void)copy;
 * @endcode
 */
template <typename T>
class Arc;

/**
 * @brief Mutual-exclusion wrapper that serializes mutable access to a payload.
 *
 * @tparam T Object type stored inside the mutex wrapper.
 * @pre `T` is a complete non-reference, non-array object type whenever a
 * `Mutex<T>` is constructed.
 * @post Construction creates one guarded payload. `lock()` returns an
 * exclusive `MutRef<T>` that unlocks on destruction.
 * @invariant At most one live mutex-backed `MutRef<T>` owns the underlying
 * `std::mutex` at a time, and debug-check builds mirror that state in a
 * one-token borrow ledger.
 * @throws Construction may throw exceptions from `T` construction. `lock()` may
 * throw `std::system_error` or a configured memsafe violation exception. Copy
 * and move operations are disabled.
 * @note Ownership/thread-safety: the mutex owns its payload directly and is
 * thread-safe for concurrent `lock()` calls while the `Mutex<T>` object remains
 * alive.
 *
 * Example:
 * @code
 * memsafe::Mutex<int> counter(0);
 * auto borrow = counter.lock();
 * *borrow += 1;
 * @endcode
 */
template <typename T>
class Mutex;

namespace detail {

/**
 * @brief Control block that owns one payload and tracks Slice 4 borrows.
 *
 * @tparam T Object type stored in the control block.
 * @pre `T` is a complete non-reference, non-array object type and is
 * constructible from the arguments passed to the constructor.
 * @post Construction allocates and constructs one `T`; destruction releases it.
 * @invariant When debug checks are enabled, `borrow_state_` is the single
 * atomic `Atomic_Refcount` ledger: `0` means unborrowed, `mutable_token_` means
 * exclusively borrowed, and values in between count shared borrows.
 * @throws Construction may throw allocation exceptions or `T` construction
 * exceptions. Borrow acquisition may report through the active violation policy
 * when exclusivity is broken.
 * @note Ownership/thread-safety: this is a private single-owner control block.
 * Borrow handles point to it but do not own it; atomic operations make the
 * borrow ledger safe for cross-thread acquire/release.
 */
template <typename T>
class sync_borrow_ctrl final {
public:
    static_assert(!std::is_reference<T>::value,
                  "memsafe::SyncOwner<T> cannot own a reference type");
    static_assert(!std::is_array<T>::value,
                  "memsafe::SyncOwner<T> does not support array types");

    /**
     * @brief Construct the owned payload from forwarded arguments.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is constructible from `Args&&...`.
     * @post `get()` returns a non-null pointer to the new `T`, and no borrows
     * are live.
     * @invariant The payload is heap-allocated so moving `SyncOwner<T>` moves
     * only the owning control-block pointer, not the `T` object itself.
     * @throws `std::bad_alloc` or any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the control block owns the payload and is
     * intended for one owning `SyncOwner<T>` at a time.
     */
    template <typename... Args>
    explicit sync_borrow_ctrl(Args&&... args)
        : value_(std::make_unique<T>(std::forward<Args>(args)...)) {}

    /**
     * @brief Copy construction is disabled for unique ownership.
     *
     * @param other Source control block that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a control block is a compile-time error.
     * @post No duplicate owner or duplicate borrow counter is created.
     * @invariant A payload has one control block and one owning owner.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents double deletion
     * and split atomic borrow ledgers.
     */
    sync_borrow_ctrl(const sync_borrow_ctrl& other) = delete;

    /**
     * @brief Copy assignment is disabled for unique ownership.
     *
     * @param other Source control block that would otherwise replace this one.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No payload ownership is duplicated.
     * @invariant Borrow counters remain attached to exactly one payload.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copy assignment avoids aliasing
     * two owners to one atomic borrow ledger.
     */
    sync_borrow_ctrl& operator=(const sync_borrow_ctrl& other) = delete;

    /**
     * @brief Move construction is disabled because owners move the pointer to
     * the control block.
     *
     * @param other Source control block that would otherwise be moved.
     * @return No value; this overload is deleted.
     * @pre Not available. Move construction is a compile-time error.
     * @post Existing borrow handles never observe a relocated control block.
     * @invariant The address of a control block is stable for its lifetime.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: stable control-block addresses keep
     * non-owning sync borrow handles valid while the owner object itself moves.
     */
    sync_borrow_ctrl(sync_borrow_ctrl&& other) = delete;

    /**
     * @brief Move assignment is disabled because owners transfer the owning
     * pointer instead.
     *
     * @param other Source control block that would otherwise replace this one.
     * @return No value; this overload is deleted.
     * @pre Not available. Move assignment is a compile-time error.
     * @post No control-block address changes under live borrows.
     * @invariant A control block is never relocated after construction.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: stable control-block addresses avoid
     * dangling borrow-control pointers after `SyncOwner<T>` moves.
     */
    sync_borrow_ctrl& operator=(sync_borrow_ctrl&& other) = delete;

    /**
     * @brief Destroy the payload and borrow ledger.
     *
     * @return No value; destructors release the receiving object.
     * @pre No `SyncRef<T>` or `SyncMutRef<T>` that points at this control block
     * may be used after destruction begins.
     * @post The owned `T` has been destroyed exactly once.
     * @invariant Destruction is the sole normal release path for the owned heap
     * allocation.
     * @throws No exception may escape this destructor. If `T` violates the
     * ordinary C++ expectation that destructors do not throw during cleanup, the
     * runtime termination rules for destructors apply.
     * @note Ownership/thread-safety: destruction requires the caller to have
     * externally joined or otherwise ended cross-thread borrow use.
     */
    ~sync_borrow_ctrl() = default;

    /**
     * @brief Return whether the control block currently owns a payload.
     *
     * @retval true `get()` returns a non-null pointer.
     * @retval false Construction did not complete or the control block is not
     * usable.
     * @pre The control block is alive.
     * @post No state is modified.
     * @invariant A constructed control block owns exactly one payload until it
     * is destroyed.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: observer only. Reading the owning
     * `std::unique_ptr` is safe while the control block is alive and not being
     * destroyed.
     */
    bool has_value() const noexcept {
        return static_cast<bool>(value_);
    }

    /**
     * @brief Return a mutable pointer to the owned payload.
     *
     * @return Pointer to the owned `T`, or null only if construction did not
     * complete and the control block is not usable.
     * @pre The control block is alive.
     * @post No borrow state is modified.
     * @invariant Normal live control blocks return the same stable payload
     * address for their lifetime.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is non-owning and must be used
     * according to the active borrow state and ordinary C++ data-race rules.
     */
    T* get() noexcept {
        return value_.get();
    }

    /**
     * @brief Return a const pointer to the owned payload.
     *
     * @return Pointer to the owned `T`, or null only if construction did not
     * complete and the control block is not usable.
     * @pre The control block is alive.
     * @post No borrow state is modified.
     * @invariant Normal live control blocks return the same stable payload
     * address for their lifetime.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is non-owning and must be used
     * according to the active borrow state and ordinary C++ data-race rules.
     */
    const T* get() const noexcept {
        return value_.get();
    }

    /**
     * @brief Report whether any debug-check borrow is currently live.
     *
     * @retval true At least one immutable borrow or one mutable borrow is live.
     * @retval false No borrow is live, or release checks are disabled.
     * @pre The control block is alive.
     * @post No state is modified.
     * @invariant In release no-check lanes this function is a constant false
     * observer and no counter field is present.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic observer for tests and internal
     * checks.
     */
    bool has_borrow() const noexcept {
#if MEMSAFE_RELEASE_CHECKS
        return borrow_state_.load(std::memory_order_acquire) != no_borrow_;
#else
        return false;
#endif
    }

    /**
     * @brief Try to acquire one immutable borrow.
     *
     * @retval true The immutable borrow was acquired.
     * @retval false A violation handler returned after a failed acquisition, so
     * no borrow was recorded.
     * @pre The control block is alive.
     * @post With debug checks enabled, the atomic shared count is incremented
     * only when the exclusive mutable token is absent. With release checks
     * disabled, no state is modified and the function returns true.
     * @invariant Shared borrows may coexist with other shared borrows but not
     * with a mutable borrow in debug-check lanes.
     * @throws `memsafe::violation` when THROW policy is active and a mutable
     * borrow is already live or the shared counter is exhausted; installed
     * handlers may throw.
     * @note Ownership/thread-safety: the compare-exchange loop is the Slice 4
     * atomic refcount acquire path and may be called concurrently.
     */
    bool acquire_shared() {
#if MEMSAFE_RELEASE_CHECKS
        std::uint32_t observed = borrow_state_.load(std::memory_order_acquire);
        for (;;) {
            if (observed == mutable_token_) {
                /*
                 * F1 Slice 4 requires the same exclusivity rule as Owner<T>,
                 * but the decision and publication must be one atomic step so
                 * a racing SyncMutRef cannot overlap a SyncRef.
                 */
                MEMSAFE_DETAIL_VIOLATE(
                    ::memsafe::violation_kind::borrow_exclusivity,
                    "cannot create immutable sync borrow while a mutable sync borrow exists");
                return false;
            }
            if (observed >= max_shared_borrows_) {
                /*
                 * The high bit is reserved as the exclusive token, per the
                 * AADL Slice 4 Atomic_Refcount model. Refusing to wrap keeps
                 * the state machine in its documented domain.
                 */
                MEMSAFE_DETAIL_VIOLATE(
                    ::memsafe::violation_kind::capacity_exhausted,
                    "too many immutable sync borrows");
                return false;
            }
            const std::uint32_t desired = observed + 1U;
            if (borrow_state_.compare_exchange_weak(observed,
                                                    desired,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                return true;
            }
        }
#else
        return true;
#endif
    }

    /**
     * @brief Release one immutable borrow.
     *
     * @return Nothing.
     * @pre The control block is alive and, in debug-check lanes, the caller
     * previously acquired one immutable borrow.
     * @post With debug checks enabled, the shared-borrow count is atomically
     * decremented when it is nonzero. With release checks disabled, no state is
     * modified.
     * @invariant Correct `SyncRef<T>` lifetimes balance every successful
     * `acquire_shared()` call with one release.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic release path for sync borrow
     * handles. Invalid extra releases are treated as no-ops to keep the
     * destructor path non-throwing.
     */
    void release_shared() noexcept {
#if MEMSAFE_RELEASE_CHECKS
        std::uint32_t observed = borrow_state_.load(std::memory_order_acquire);
        while (observed != no_borrow_ && observed != mutable_token_) {
            const std::uint32_t desired = observed - 1U;
            if (borrow_state_.compare_exchange_weak(observed,
                                                    desired,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                return;
            }
        }
#endif
    }

    /**
     * @brief Try to acquire the exclusive mutable borrow token.
     *
     * @retval true The mutable borrow was acquired.
     * @retval false A violation handler returned after a failed acquisition, so
     * no borrow was recorded.
     * @pre The control block is alive.
     * @post With debug checks enabled, the atomic state changes from `0` to the
     * exclusive mutable token only when no borrow is live. With release checks
     * disabled, no state is modified and the function returns true.
     * @invariant A successful mutable borrow excludes every other borrow while
     * debug checks are enabled.
     * @throws `memsafe::violation` when THROW policy is active and any borrow
     * is already live; installed handlers may throw.
     * @note Ownership/thread-safety: the single compare-exchange is the Slice 4
     * atomic exclusive-token acquire path and may race safely with shared
     * acquisition attempts.
     */
    bool acquire_mut() {
#if MEMSAFE_RELEASE_CHECKS
        std::uint32_t expected = no_borrow_;
        if (borrow_state_.compare_exchange_strong(expected,
                                                  mutable_token_,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return true;
        }
        /*
         * Expected now contains the observed nonzero state. Reporting one
         * borrow_exclusivity violation for every failed exclusive acquisition
         * matches F1 Slice 4 and the F2 cross-thread SyncOwner case.
         */
        MEMSAFE_DETAIL_VIOLATE(
            ::memsafe::violation_kind::borrow_exclusivity,
            "cannot create mutable sync borrow while another sync borrow exists");
        return false;
#else
        return true;
#endif
    }

    /**
     * @brief Release the exclusive mutable borrow token.
     *
     * @return Nothing.
     * @pre The control block is alive and, in debug-check lanes, the caller
     * previously acquired the mutable borrow token.
     * @post With debug checks enabled, the mutable token is atomically cleared.
     * With release checks disabled, no state is modified.
     * @invariant Correct `SyncMutRef<T>` lifetimes balance every successful
     * `acquire_mut()` call with one release.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic release path for sync mutable
     * handles. Invalid releases are ignored to keep destruction non-throwing.
     */
    void release_mut() noexcept {
#if MEMSAFE_RELEASE_CHECKS
        std::uint32_t expected = mutable_token_;
        (void)borrow_state_.compare_exchange_strong(expected,
                                                    no_borrow_,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
#endif
    }

private:
    /// Owned heap allocation that backs `SyncOwner<T>` dereference and borrows.
    std::unique_ptr<T> value_;

#if MEMSAFE_RELEASE_CHECKS
    /// State value meaning no counted sync borrow is live.
    static constexpr std::uint32_t no_borrow_ = 0U;
    /// State value reserving the high bit for the exclusive mutable token.
    static constexpr std::uint32_t mutable_token_ = 0x80000000U;
    /// Highest representable immutable-borrow count before the token bit.
    static constexpr std::uint32_t max_shared_borrows_ = mutable_token_ - 1U;
    /// Atomic Slice 4 borrow ledger for shared counts and the exclusive token.
    std::atomic<std::uint32_t> borrow_state_{no_borrow_};
#endif
};

/**
 * @brief Acquire a shared sync borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to acquire from, or null for an already-empty
 * borrow handle.
 * @return `ctrl` when acquisition succeeds; otherwise null.
 * @pre If non-null, `ctrl` points to a live `sync_borrow_ctrl<T>`.
 * @post A successful debug-check acquisition increments the atomic shared
 * count.
 * @invariant Null inputs stay null and do not touch counters.
 * @throws `memsafe::violation` or an installed-handler exception if acquisition
 * reports a violation.
 * @note Ownership/thread-safety: this helper does not own the control block;
 * it delegates to the atomic control-block acquire operation.
 */
template <typename T>
inline sync_borrow_ctrl<T>* sync_retain_shared(sync_borrow_ctrl<T>* ctrl) {
    if (ctrl != nullptr && ctrl->acquire_shared()) {
        return ctrl;
    }
    return nullptr;
}

/**
 * @brief Release a shared sync borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to release from, or null.
 * @return Nothing.
 * @pre If non-null, `ctrl` points to a live `sync_borrow_ctrl<T>` and the
 * caller owns one shared borrow acquired from it.
 * @post The shared count is atomically decremented in debug-check lanes.
 * @invariant Null inputs are no-ops.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper does not own the control block;
 * it delegates to the atomic control-block release operation.
 */
template <typename T>
inline void sync_release_shared(sync_borrow_ctrl<T>* ctrl) noexcept {
    if (ctrl != nullptr) {
        ctrl->release_shared();
    }
}

/**
 * @brief Acquire an exclusive mutable sync borrow for a nullable control
 * pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to acquire from, or null for an already-empty
 * borrow handle.
 * @return `ctrl` when acquisition succeeds; otherwise null.
 * @pre If non-null, `ctrl` points to a live `sync_borrow_ctrl<T>`.
 * @post A successful debug-check acquisition sets the atomic mutable token.
 * @invariant Null inputs stay null and do not touch counters.
 * @throws `memsafe::violation` or an installed-handler exception if acquisition
 * reports a violation.
 * @note Ownership/thread-safety: this helper does not own the control block;
 * it delegates to the atomic exclusive-token acquire operation.
 */
template <typename T>
inline sync_borrow_ctrl<T>* sync_retain_mut(sync_borrow_ctrl<T>* ctrl) {
    if (ctrl != nullptr && ctrl->acquire_mut()) {
        return ctrl;
    }
    return nullptr;
}

/**
 * @brief Release an exclusive mutable sync borrow for a nullable control
 * pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to release from, or null.
 * @return Nothing.
 * @pre If non-null, `ctrl` points to a live `sync_borrow_ctrl<T>` and the
 * caller owns the mutable token acquired from it.
 * @post The mutable token is atomically cleared in debug-check lanes.
 * @invariant Null inputs are no-ops.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper does not own the control block;
 * it delegates to the atomic control-block release operation.
 */
template <typename T>
inline void sync_release_mut(sync_borrow_ctrl<T>* ctrl) noexcept {
    if (ctrl != nullptr) {
        ctrl->release_mut();
    }
}

/**
 * @brief Shared control block for `Arc<T>` payload and atomic strong count.
 *
 * @tparam T Object type stored in the `Arc` control block.
 * @pre `T` is a complete non-reference, non-array object type and is
 * constructible from the forwarded constructor arguments.
 * @post Construction initializes the payload and starts the strong count at
 * one. The control block deletes itself when the last strong reference is
 * released.
 * @invariant `strong_count_` is always positive while the control block is
 * reachable from any non-empty `Arc<T>`. Count zero is represented by the
 * control block having been deleted, not by an observable zero value.
 * @throws Construction may throw allocation or `T` construction exceptions.
 * Retain may report `capacity_exhausted` under the configured violation policy
 * if the count reaches `std::numeric_limits<std::uint64_t>::max()`.
 * @note Ownership/thread-safety: `Arc<T>` handles share this block. Atomic
 * retain/release operations synchronize ownership reclamation; payload access
 * remains governed by ordinary C++ data-race rules.
 */
template <typename T>
class arc_ctrl final {
public:
    static_assert(!std::is_reference<T>::value,
                  "memsafe::Arc<T> cannot own a reference type");
    static_assert(!std::is_array<T>::value,
                  "memsafe::Arc<T> does not support array types");

    /**
     * @brief Construct the shared payload from forwarded arguments.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is constructible from `Args&&...`.
     * @post `get()` returns the new payload address and `strong_count()` is 1.
     * @invariant The strong count begins at one so the creating `Arc<T>` owns
     * exactly one release obligation.
     * @throws Any exception thrown by `T` construction.
     * @note Ownership/thread-safety: no sharing exists until the constructor
     * returns and an `Arc<T>` publishes the control pointer.
     */
    template <typename... Args>
    explicit arc_ctrl(Args&&... args)
        : value_(std::forward<Args>(args)...), strong_count_(1U) {}

    /**
     * @brief Copy construction is disabled for shared control blocks.
     *
     * @param other Source block that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a control block is a compile-time error.
     * @post No duplicate payload or split strong count is created.
     * @invariant Every shared payload has exactly one strong-count word.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: `Arc<T>` copies retain this block instead
     * of copying the block itself.
     */
    arc_ctrl(const arc_ctrl& other) = delete;

    /**
     * @brief Copy assignment is disabled for shared control blocks.
     *
     * @param other Source block that would otherwise replace this block.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post The payload and strong count remain unique to this control block.
     * @invariant Refcount state cannot be split across two blocks.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: ownership transfer is represented only by
     * atomic retain/release on the original block.
     */
    arc_ctrl& operator=(const arc_ctrl& other) = delete;

    /**
     * @brief Move construction is disabled because `Arc<T>` moves the pointer.
     *
     * @param other Source block that would otherwise be moved.
     * @return No value; this overload is deleted.
     * @pre Not available. Move construction is a compile-time error.
     * @post Existing `Arc<T>` handles never observe a relocated control block.
     * @invariant The control-block address is stable until the final release.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: stable addresses allow atomic retain and
     * release from distinct handles without relocating shared state.
     */
    arc_ctrl(arc_ctrl&& other) = delete;

    /**
     * @brief Move assignment is disabled because `Arc<T>` moves the pointer.
     *
     * @param other Source block that would otherwise replace this block.
     * @return No value; this overload is deleted.
     * @pre Not available. Move assignment is a compile-time error.
     * @post No control-block address changes under live `Arc<T>` handles.
     * @invariant The atomic count remains attached to one stable payload.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: stable control-block identity is the basis
     * for Slice 5 atomic-reference-count sharing.
     */
    arc_ctrl& operator=(arc_ctrl&& other) = delete;

    /**
     * @brief Destroy the shared payload and strong-count word.
     *
     * @return No value; destructors release the receiving object.
     * @pre The strong count has reached one and the final release is deleting
     * this control block.
     * @post The stored `T` has been destroyed exactly once.
     * @invariant Only `release_strong()` on the final owner deletes the block.
     * @throws No exception may escape this destructor. If `T`'s destructor
     * throws during cleanup, normal C++ destructor termination rules apply.
     * @note Ownership/thread-safety: deletion is sequenced after the final
     * atomic decrement observes that no other strong owners remain.
     */
    ~arc_ctrl() = default;

    /**
     * @brief Atomically retain one additional strong reference.
     *
     * @retval true The strong count was incremented and the caller owns one
     * additional release obligation.
     * @retval false A violation handler returned after count exhaustion, so no
     * new strong reference was recorded.
     * @pre This control block is alive and reachable from an existing
     * non-empty `Arc<T>`.
     * @post On success, `strong_count()` is one larger than before the retain.
     * @invariant The count never wraps; the maximum unsigned value is rejected
     * before publication.
     * @throws `memsafe::violation` under THROW policy when the strong count is
     * exhausted; installed handlers may throw.
     * @note Ownership/thread-safety: the compare-exchange loop is the Slice 5
     * `Atomic_Refcount` acquire path for copying `Arc<T>` handles.
     */
    bool retain_strong() {
        std::uint64_t observed = strong_count_.load(std::memory_order_acquire);
        for (;;) {
            if (observed == std::numeric_limits<std::uint64_t>::max()) {
                /*
                 * F3 models Arc with an unsigned 64-bit atomic strong count.
                 * Refusing to wrap preserves the AADL Atomic_Refcount domain
                 * and prevents a future premature delete.
                 */
                MEMSAFE_DETAIL_VIOLATE(
                    ::memsafe::violation_kind::capacity_exhausted,
                    "too many Arc strong references");
                return false;
            }

            const std::uint64_t desired = observed + 1U;
            if (strong_count_.compare_exchange_weak(observed,
                                                    desired,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    /**
     * @brief Atomically release one strong reference and delete at count zero.
     *
     * @return Nothing.
     * @pre The caller owns one strong release obligation for this control
     * block.
     * @post If the released reference was the last one, `delete this` has
     * destroyed the payload. Otherwise the strong count is one smaller.
     * @invariant Exactly one caller observes the transition from one to zero
     * and therefore exactly one caller deletes the payload.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: release uses acquire-release ordering and
     * an acquire fence before deletion so payload writes sequenced before other
     * releases are visible to the final deleter.
     */
    void release_strong() noexcept {
        const std::uint64_t previous =
            strong_count_.fetch_sub(1U, std::memory_order_acq_rel);
        if (previous == 1U) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete this;
        }
    }

    /**
     * @brief Return the current strong-reference count.
     *
     * @return Atomic snapshot of the number of live `Arc<T>` handles sharing
     * this control block.
     * @pre The control block is alive.
     * @post No state is modified.
     * @invariant The returned value is an observation only and may be stale
     * immediately when other threads copy or drop distinct handles.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic observer for diagnostics and tests.
     */
    std::uint64_t strong_count() const noexcept {
        return strong_count_.load(std::memory_order_acquire);
    }

    /**
     * @brief Return a mutable pointer to the shared payload.
     *
     * @return Pointer to the stored `T`.
     * @pre The control block is alive.
     * @post No reference-count state is modified.
     * @invariant The payload address is stable for the control block lifetime.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is non-owning. It carries no
     * payload-level synchronization; use `Mutex<T>` for shared mutation.
     */
    T* get() noexcept {
        return &value_;
    }

private:
    /// Payload owned by the shared control block.
    T value_;
    /// Atomic strong count shared by all `Arc<T>` handles.
    std::atomic<std::uint64_t> strong_count_;
};

/**
 * @brief Retain an `Arc<T>` control block through a nullable pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control block to retain, or null for an empty source `Arc<T>`.
 * @return `ctrl` when the retain succeeds; otherwise null.
 * @pre If non-null, `ctrl` points to a live `arc_ctrl<T>` with a positive
 * strong count.
 * @post A successful retain increments the strong count exactly once.
 * @invariant Null inputs stay null and do not touch counters.
 * @throws `memsafe::violation` or an installed-handler exception if retain
 * reports count exhaustion.
 * @note Ownership/thread-safety: helper for public `Arc<T>` copy operations.
 */
template <typename T>
inline arc_ctrl<T>* arc_retain(arc_ctrl<T>* ctrl) {
    if (ctrl != nullptr && ctrl->retain_strong()) {
        return ctrl;
    }
    return nullptr;
}

/**
 * @brief Release an `Arc<T>` control block through a nullable pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control block to release, or null for an empty `Arc<T>`.
 * @return Nothing.
 * @pre If non-null, the caller owns one strong release obligation for `ctrl`.
 * @post A non-null control block has had its strong count decremented, and may
 * have been deleted if this was the last release.
 * @invariant Null inputs are no-ops, matching moved-from and default `Arc<T>`
 * destruction.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: helper for public `Arc<T>` destruction and
 * assignment operations.
 */
template <typename T>
inline void arc_release(arc_ctrl<T>* ctrl) noexcept {
    if (ctrl != nullptr) {
        ctrl->release_strong();
    }
}

} // namespace detail

/**
 * @brief Immutable counted borrow implementation for `SyncOwner<T>`.
 *
 * @tparam T Object type borrowed immutably.
 * @pre Construct through `SyncOwner<T>::borrow()` or a trusted friend so the
 * atomic borrow counter is acquired consistently.
 * @post Destruction or reassignment atomically releases one shared borrow when
 * checks are enabled.
 * @invariant `SyncRef<T>` is copyable, non-owning, type-level nodiscard, and
 * never exposes mutable access to the payload.
 * @throws Copy and factory construction may report through the configured
 * violation policy; move and destruction operations are `noexcept`.
 * @note Ownership/thread-safety: non-owning borrow handle. The atomic counter
 * allows copies, moves, and destruction on different threads while the owner
 * remains alive.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> owner(3);
 * auto first = owner.borrow();
 * auto second = first;
 * int sum = *first + *second;
 * (void)sum;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD SyncRef final {
public:
    /**
     * @brief Copy an immutable sync borrow and increment the shared count.
     *
     * @param other Borrow handle to copy.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is either empty or points to a live sync owner control
     * block.
     * @post This object refers to the same payload as `other` when acquisition
     * succeeds. In debug-check lanes the atomic shared-borrow count is
     * incremented.
     * @invariant Copying preserves immutable-only access.
     * @throws `memsafe::violation` under THROW policy if the source control
     * block cannot accept a shared borrow; installed handlers may throw.
     * @note Ownership/thread-safety: the copied handle remains non-owning and
     * uses the same atomic borrow ledger.
     */
    SyncRef(const SyncRef& other)
        : ctrl_(detail::sync_retain_shared(other.ctrl_)) {}

    /**
     * @brief Move an immutable sync borrow without changing the shared count.
     *
     * @param other Borrow handle whose recorded borrow is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `SyncRef<T>`.
     * @post This object owns `other`'s borrow record and `other` is empty.
     * @invariant Moving does not create or destroy a borrow-count entry.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the borrow remains non-owning and tied to
     * the same atomic owner control block.
     */
    SyncRef(SyncRef&& other) noexcept : ctrl_(other.ctrl_) {
        other.ctrl_ = nullptr;
    }

    /**
     * @brief Copy-assign an immutable sync borrow.
     *
     * @param other Borrow handle to copy from.
     * @return Reference to this borrow handle.
     * @pre `other` is either empty or points to a live sync owner control
     * block.
     * @post This object refers to the same payload as `other` when acquisition
     * succeeds; the previously held borrow, if any, is released.
     * @invariant The new borrow is acquired before the old one is released so a
     * throwing violation policy leaves this object unchanged.
     * @throws `memsafe::violation` under THROW policy if the new source control
     * block cannot accept a shared borrow; installed handlers may throw.
     * @note Ownership/thread-safety: assignment updates atomic borrow counters
     * and transfers no payload ownership.
     */
    SyncRef& operator=(const SyncRef& other) {
        if (this != &other) {
            detail::sync_borrow_ctrl<T>* const next =
                detail::sync_retain_shared(other.ctrl_);
            detail::sync_release_shared(ctrl_);
            ctrl_ = next;
        }
        return *this;
    }

    /**
     * @brief Move-assign an immutable sync borrow.
     *
     * @param other Borrow handle whose recorded borrow is transferred.
     * @return Reference to this borrow handle.
     * @pre `other` is a live `SyncRef<T>`.
     * @post This object owns `other`'s previous borrow record, the previously
     * held borrow is released, and `other` is empty.
     * @invariant Moving does not create an additional shared-count entry.
     * @throws Nothing; this assignment operator is `noexcept`.
     * @note Ownership/thread-safety: assignment releases any prior atomic
     * borrow record and then transfers the source record.
     */
    SyncRef& operator=(SyncRef&& other) noexcept {
        if (this != &other) {
            detail::sync_release_shared(ctrl_);
            ctrl_ = other.ctrl_;
            other.ctrl_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Release this immutable sync borrow.
     *
     * @return No value; destructors release the receiving object.
     * @pre The owning `SyncOwner<T>` control block is still alive when this
     * handle is non-empty.
     * @post The shared-borrow count is decremented in debug-check lanes.
     * @invariant Each successful acquisition is released exactly once by
     * destruction or assignment.
     * @throws Nothing; this destructor is `noexcept`.
     * @note Ownership/thread-safety: destruction is an atomic borrow-ledger
     * release and may occur on a different thread from acquisition.
     */
    ~SyncRef() noexcept {
        detail::sync_release_shared(ctrl_);
    }

    /**
     * @brief Dereference the immutable sync borrow.
     *
     * @return Const reference to the borrowed payload.
     * @pre This handle is non-empty and its owning `SyncOwner<T>` is still
     * alive.
     * @post No borrow state is modified.
     * @invariant The returned reference does not permit mutation through this
     * `SyncRef<T>`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the reference is non-owning and bounded by
     * both this `SyncRef<T>` and the owning `SyncOwner<T>`. Concurrent reads are
     * valid only when `T` itself is safely read concurrently.
     */
    const T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the immutable borrowed payload.
     *
     * @return Const pointer to the borrowed payload.
     * @pre This handle is non-empty and its owning `SyncOwner<T>` is still
     * alive.
     * @post No borrow state is modified.
     * @invariant The returned pointer does not permit mutation through this
     * `SyncRef<T>`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the pointer is non-owning and bounded by
     * both this `SyncRef<T>` and the owning `SyncOwner<T>`.
     */
    const T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

private:
    friend class SyncOwner<T>;

    /**
     * @brief Construct a sync borrow from an owner control block.
     *
     * @param ctrl Sync owner control block that supplies the payload and atomic
     * counter.
     * @return No value; constructors initialize the receiving object.
     * @pre `ctrl` is alive and belongs to a live `SyncOwner<T>` or trusted
     * future wrapper.
     * @post On successful acquisition, this handle points at `ctrl` and the
     * shared-borrow count is atomically incremented in debug-check lanes.
     * @invariant The parameter carries `MEMSAFE_BORROWS(ctrl)` to connect the
     * returned borrow lifetime to the owner control source for backend lanes.
     * @throws `memsafe::violation` under THROW policy if a mutable borrow is
     * already live; installed handlers may throw.
     * @note Ownership/thread-safety: the constructed handle does not own the
     * control block and may be moved to another thread after construction.
     */
    explicit SyncRef(detail::sync_borrow_ctrl<T>& ctrl MEMSAFE_BORROWS(ctrl))
        : ctrl_(detail::sync_retain_shared(&ctrl)) {}

    /**
     * @brief Return the borrowed payload pointer or report a null borrow.
     *
     * @return Const pointer to the borrowed payload.
     * @pre The handle is non-empty and the owner control block is alive.
     * @post No borrow state is modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the handle is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning. The
     * borrow ledger has already been acquired by this handle.
     */
    const T* require_value() const {
        const T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty SyncRef");
        }
#endif
        return value;
    }

    /// Non-owning pointer to the sync owner control block for this borrow.
    detail::sync_borrow_ctrl<T>* ctrl_ = nullptr;
};

/**
 * @brief Exclusive mutable counted borrow implementation for `SyncOwner<T>`.
 *
 * @tparam T Object type borrowed mutably.
 * @pre Construct through `SyncOwner<T>::borrow_mut()` or a trusted friend so
 * the atomic exclusive token is acquired consistently.
 * @post Destruction or reassignment atomically releases the exclusive token
 * when checks are enabled.
 * @invariant `SyncMutRef<T>` is non-copyable, movable, non-owning, type-level
 * nodiscard, and exposes the only counted mutable access path.
 * @throws Factory construction may report through the configured violation
 * policy; move and destruction operations are `noexcept`.
 * @note Ownership/thread-safety: non-owning borrow handle. Moving between
 * threads is supported while the owning `SyncOwner<T>` remains alive.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> owner(3);
 * auto write = owner.borrow_mut();
 * *write += 1;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD SyncMutRef final {
public:
    /**
     * @brief Copy construction is disabled for exclusive sync borrows.
     *
     * @param other Source mutable borrow that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `SyncMutRef<T>` is a compile-time error.
     * @post No second exclusive handle is created.
     * @invariant There can be at most one live mutable borrow per sync owner
     * control block in debug-check lanes.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies preserves exclusive
     * access semantics across threads.
     */
    SyncMutRef(const SyncMutRef& other) = delete;

    /**
     * @brief Copy assignment is disabled for exclusive sync borrows.
     *
     * @param other Source mutable borrow that would otherwise be assigned.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No duplicate exclusive handle is created.
     * @invariant The mutable borrow token cannot be copied.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copy assignment preserves
     * exclusive access semantics across threads.
     */
    SyncMutRef& operator=(const SyncMutRef& other) = delete;

    /**
     * @brief Move an exclusive mutable sync borrow without changing the token.
     *
     * @param other Borrow handle whose exclusive token is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `SyncMutRef<T>`.
     * @post This object owns `other`'s borrow record and `other` is empty.
     * @invariant Moving does not create another exclusive token.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the borrow remains non-owning and tied to
     * the same atomic owner control block.
     */
    SyncMutRef(SyncMutRef&& other) noexcept : ctrl_(other.ctrl_) {
        other.ctrl_ = nullptr;
    }

    /**
     * @brief Move-assign an exclusive mutable sync borrow.
     *
     * @param other Borrow handle whose exclusive token is transferred.
     * @return Reference to this borrow handle.
     * @pre `other` is a live `SyncMutRef<T>`.
     * @post This object owns `other`'s previous borrow record, the previously
     * held borrow is released, and `other` is empty.
     * @invariant Moving does not create another exclusive token.
     * @throws Nothing; this assignment operator is `noexcept`.
     * @note Ownership/thread-safety: assignment atomically releases any prior
     * token and then transfers the source token.
     */
    SyncMutRef& operator=(SyncMutRef&& other) noexcept {
        if (this != &other) {
            detail::sync_release_mut(ctrl_);
            ctrl_ = other.ctrl_;
            other.ctrl_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Release this exclusive mutable sync borrow.
     *
     * @return No value; destructors release the receiving object.
     * @pre The owning `SyncOwner<T>` control block is still alive when this
     * handle is non-empty.
     * @post The mutable borrow token is atomically cleared in debug-check
     * lanes.
     * @invariant Each successful mutable acquisition is released exactly once
     * by destruction or assignment.
     * @throws Nothing; this destructor is `noexcept`.
     * @note Ownership/thread-safety: destruction is an atomic borrow-ledger
     * release and may occur on a different thread from acquisition.
     */
    ~SyncMutRef() noexcept {
        detail::sync_release_mut(ctrl_);
    }

    /**
     * @brief Dereference the exclusive mutable sync borrow.
     *
     * @return Mutable reference to the borrowed payload.
     * @pre This handle is non-empty and its owning `SyncOwner<T>` is still
     * alive.
     * @post No borrow state is modified.
     * @invariant Mutable access is available only through a `SyncMutRef<T>` that
     * successfully acquired the exclusive token in debug-check lanes.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the reference is non-owning and bounded by
     * both this `SyncMutRef<T>` and the owning `SyncOwner<T>`.
     */
    T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the exclusive mutable borrowed payload.
     *
     * @return Mutable pointer to the borrowed payload.
     * @pre This handle is non-empty and its owning `SyncOwner<T>` is still
     * alive.
     * @post No borrow state is modified.
     * @invariant Mutable access is available only through a `SyncMutRef<T>` that
     * successfully acquired the exclusive token in debug-check lanes.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the pointer is non-owning and bounded by
     * both this `SyncMutRef<T>` and the owning `SyncOwner<T>`.
     */
    T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

private:
    friend class SyncOwner<T>;

    /**
     * @brief Construct a mutable sync borrow from an owner control block.
     *
     * @param ctrl Sync owner control block that supplies the payload and atomic
     * counter.
     * @return No value; constructors initialize the receiving object.
     * @pre `ctrl` is alive and belongs to a live `SyncOwner<T>` or trusted
     * future wrapper.
     * @post On successful acquisition, this handle points at `ctrl` and the
     * mutable token is set atomically in debug-check lanes.
     * @invariant The parameter carries `MEMSAFE_BORROWS(ctrl)` to connect the
     * returned borrow lifetime to the owner control source for backend lanes.
     * @throws `memsafe::violation` under THROW policy if any borrow is already
     * live; installed handlers may throw.
     * @note Ownership/thread-safety: the constructed handle does not own the
     * control block and may be moved to another thread after construction.
     */
    explicit SyncMutRef(detail::sync_borrow_ctrl<T>& ctrl MEMSAFE_BORROWS(ctrl))
        : ctrl_(detail::sync_retain_mut(&ctrl)) {}

    /**
     * @brief Return the borrowed payload pointer or report a null borrow.
     *
     * @return Mutable pointer to the borrowed payload.
     * @pre The handle is non-empty and the owner control block is alive.
     * @post No borrow state is modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the handle is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning. The
     * exclusive token has already been acquired by this handle.
     */
    T* require_value() const {
        T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty SyncMutRef");
        }
#endif
        return value;
    }

    /// Non-owning pointer to the sync owner control block for this borrow.
    detail::sync_borrow_ctrl<T>* ctrl_ = nullptr;
};

/**
 * @brief Move-only RAII owner implementation for one heap-allocated `T` with
 * atomic borrow accounting.
 *
 * @tparam T Object type owned by the wrapper.
 * @pre `T` is constructible from the constructor arguments and is not a
 * reference or array type.
 * @post Construction owns one `T`; destruction releases it; moves transfer the
 * control block and leave the source empty.
 * @invariant `SyncOwner<T>` is non-copyable and unconditionally nothrow-movable
 * so standard containers reallocate by move. The control block address remains
 * stable so existing borrow handles survive moves of the owner object.
 * @throws Construction may allocate or propagate `T` construction exceptions;
 * move construction and move assignment throw nothing.
 * @note Ownership/thread-safety: unique owner with atomic borrow accounting.
 * Concurrent calls to `borrow()` and `borrow_mut()` are supported while no
 * thread concurrently moves or destroys the owner.
 *
 * Example:
 * @code
 * memsafe::SyncOwner<int> value(1);
 * auto read = value.borrow();
 * int current = *read;
 * (void)current;
 * @endcode
 */
template <typename T>
class SyncOwner final {
public:
    /**
     * @brief Construct an owner by forwarding arguments to `T`.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @tparam Enable SFINAE guard that participates only when `T` is
     * constructible from `Args&&...`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is a non-reference, non-array object type constructible from
     * `Args&&...`.
     * @post `has_value()` is true and no borrows are live.
     * @invariant The payload is heap-allocated and controlled by exactly one
     * `SyncOwner<T>` with one atomic borrow ledger.
     * @throws `std::bad_alloc` or any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the new owner has unique ownership. After
     * construction, its borrow APIs can be called from multiple threads while
     * the owner remains alive and unmoved.
     *
     * Example:
     * @code
     * memsafe::SyncOwner<std::pair<int, int>> owner(1, 2);
     * int first = owner->first;
     * (void)first;
     * @endcode
     */
    template <typename... Args,
              typename Enable = typename std::enable_if<
                  std::is_constructible<T, Args&&...>::value>::type>
    explicit SyncOwner(Args&&... args)
        : ctrl_(std::make_unique<detail::sync_borrow_ctrl<T>>(
              std::forward<Args>(args)...)) {}

    /**
     * @brief Move ownership from another sync owner.
     *
     * @param other Source owner whose control block is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `SyncOwner<T>` and is not being accessed
     * concurrently.
     * @post This owner contains `other`'s former control block and `other` is
     * empty.
     * @invariant Moving transfers ownership of the control block without
     * relocating it, so existing borrow handles still point to the same control
     * address.
     * @throws Nothing; this constructor is unconditionally `noexcept(true)`.
     * @note Ownership/thread-safety: ownership moves to the receiving object;
     * the move operation itself must not race with borrow acquisition,
     * destruction, or direct owner access.
     */
    SyncOwner(SyncOwner&& other) noexcept(true) = default;

    /**
     * @brief Move-assign ownership from another sync owner.
     *
     * @param other Source owner whose control block is transferred.
     * @return Reference to this owner.
     * @pre `other` is a live `SyncOwner<T>` and is not being accessed
     * concurrently. Any borrow handles into this owner's current payload must
     * have ended before assignment.
     * @post This owner contains `other`'s former control block and `other` is
     * empty.
     * @invariant Move assignment is unconditionally `noexcept(true)` so
     * `std::vector<SyncOwner<T>>` can reallocate by moving rather than copying.
     * @throws Nothing; this assignment operator is unconditionally
     * `noexcept(true)`.
     * @note Ownership/thread-safety: ownership moves to the receiving object;
     * the move operation itself must not race with borrow acquisition,
     * destruction, or direct owner access.
     */
    SyncOwner& operator=(SyncOwner&& other) noexcept(true) = default;

    /**
     * @brief Copy construction is disabled for unique ownership.
     *
     * @param other Source owner that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `SyncOwner<T>` is a compile-time error.
     * @post No duplicate owner is created for the same payload.
     * @invariant A payload has exactly one owning `SyncOwner<T>`.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents double deletion
     * and split atomic borrow counters.
     */
    SyncOwner(const SyncOwner& other) = delete;

    /**
     * @brief Copy assignment is disabled for unique ownership.
     *
     * @param other Source owner that would otherwise replace this owner.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No payload ownership is duplicated.
     * @invariant A payload remains owned by exactly one `SyncOwner<T>`.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copy assignment preserves unique
     * ownership and a single atomic borrow ledger.
     */
    SyncOwner& operator=(const SyncOwner& other) = delete;

    /**
     * @brief Destroy the owned payload.
     *
     * @return No value; destructors release the receiving object.
     * @pre No `SyncRef<T>` or `SyncMutRef<T>` into this owner may be used after
     * destruction begins.
     * @post The heap-allocated `T`, if present, has been destroyed exactly once.
     * @invariant The control block is released by the unique pointer.
     * @throws No exception may escape this destructor. If `T` violates the
     * ordinary C++ expectation that destructors do not throw during cleanup, the
     * runtime termination rules for destructors apply.
     * @note Ownership/thread-safety: destruction requires exclusive access to
     * the owner object and externally guaranteed absence of live borrow use.
     */
    ~SyncOwner() = default;

    /**
     * @brief Return whether this owner currently contains a payload.
     *
     * @retval true This owner has a control block with a payload.
     * @retval false This owner has been moved from or otherwise has no payload.
     * @pre The owner object is alive.
     * @post No state is modified.
     * @invariant Moved-from owners report false.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: observer only. Concurrent calls are safe
     * while no thread moves or destroys this owner.
     */
    bool has_value() const noexcept {
        return ctrl_ != nullptr && ctrl_->has_value();
    }

    /**
     * @brief Dereference the owned payload mutably.
     *
     * @return Mutable reference to the owned payload.
     * @pre `has_value()` is true, and the caller has external synchronization
     * when sharing the owner across threads.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a `SyncRef<T>` or
     * `SyncMutRef<T>`; callers should prefer explicit borrows for counted
     * cross-thread access.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned reference is tied to this owner
     * and is not itself synchronized by the atomic borrow ledger.
     */
    T& operator*() MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Dereference the owned payload immutably.
     *
     * @return Const reference to the owned payload.
     * @pre `has_value()` is true, and the caller has external synchronization
     * when sharing the owner across threads.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a `SyncRef<T>`; counted
     * immutable access is available through `borrow()`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned reference is tied to this owner
     * and is not itself synchronized by the atomic borrow ledger.
     */
    const T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the owned payload mutably.
     *
     * @return Mutable pointer to the owned payload.
     * @pre `has_value()` is true, and the caller has external synchronization
     * when sharing the owner across threads.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a borrow-handle object.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned pointer is tied to this owner and
     * is not itself synchronized by the atomic borrow ledger.
     */
    T* operator->() MEMSAFE_BORROWS(*this) {
        return require_value();
    }

    /**
     * @brief Access a member of the owned payload immutably.
     *
     * @return Const pointer to the owned payload.
     * @pre `has_value()` is true, and the caller has external synchronization
     * when sharing the owner across threads.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a borrow-handle object.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned pointer is tied to this owner and
     * is not itself synchronized by the atomic borrow ledger.
     */
    const T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

    /**
     * @brief Create an immutable counted sync borrow.
     *
     * @return `SyncRef<T>` by value so type-level `MEMSAFE_NODISCARD`
     * diagnostics fire when the borrow is discarded.
     * @pre `has_value()` is true and no mutable borrow is live in debug-check
     * lanes.
     * @post On success, the atomic shared-borrow count is incremented until the
     * returned `SyncRef<T>` is destroyed, moved away, or assigned over.
     * @invariant Multiple immutable sync borrows may coexist, and the
     * `MEMSAFE_BORROWS(*this)` annotation ties the returned borrow to this owner
     * for backend lifetime analysis.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner, an active mutable borrow, or shared-count exhaustion;
     * installed handlers may throw.
     * @note Ownership/thread-safety: the returned borrow is non-owning and may
     * be copied or destroyed on any thread while the owner remains alive.
     *
     * Example:
     * @code
     * memsafe::SyncOwner<int> owner(11);
     * auto read = owner.borrow();
     * int copy = *read;
     * (void)copy;
     * @endcode
     */
    MEMSAFE_NODISCARD SyncRef<T> borrow() const MEMSAFE_BORROWS(*this) {
        return SyncRef<T>(require_control());
    }

    /**
     * @brief Create an exclusive mutable counted sync borrow.
     *
     * @return `SyncMutRef<T>` by value so type-level `MEMSAFE_NODISCARD`
     * diagnostics fire when the borrow is discarded.
     * @pre `has_value()` is true and no immutable or mutable borrow is live in
     * debug-check lanes.
     * @post On success, the mutable borrow token is held until the returned
     * `SyncMutRef<T>` is destroyed, moved away, or assigned over.
     * @invariant With debug checks enabled, requesting this borrow while any
     * borrow exists reports `violation_kind::borrow_exclusivity`; the
     * `MEMSAFE_BORROWS(*this)` annotation ties the returned borrow to this owner
     * for backend lifetime analysis.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner or any active borrow; installed handlers may throw. With
     * release checks off, the exclusivity branch is not compiled.
     * @note Ownership/thread-safety: the returned borrow is non-owning and may
     * be moved or destroyed on any thread while the owner remains alive.
     *
     * Example:
     * @code
     * memsafe::SyncOwner<int> owner(11);
     * auto write = owner.borrow_mut();
     * *write = 12;
     * @endcode
     */
    MEMSAFE_NODISCARD SyncMutRef<T> borrow_mut() MEMSAFE_BORROWS(*this) {
        return SyncMutRef<T>(require_control());
    }

private:
    /**
     * @brief Return the control block or report a null owner.
     *
     * @return Reference to the owner's control block.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned reference is internal and
     * non-owning. The owner must not be moved or destroyed concurrently.
     */
    detail::sync_borrow_ctrl<T>& require_control() const {
#if MEMSAFE_RELEASE_CHECKS
        if (ctrl_ == nullptr || !ctrl_->has_value()) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot borrow from an empty SyncOwner");
        }
#endif
        return *ctrl_;
    }

    /**
     * @brief Return the mutable payload pointer or report a null owner.
     *
     * @return Mutable pointer to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning and tied to
     * this owner. Direct owner access is not synchronized by the borrow ledger.
     */
    T* require_value() {
        T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty SyncOwner");
        }
#endif
        return value;
    }

    /**
     * @brief Return the const payload pointer or report a null owner.
     *
     * @return Const pointer to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning and tied to
     * this owner. Direct owner access is not synchronized by the borrow ledger.
     */
    const T* require_value() const {
        const T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty SyncOwner");
        }
#endif
        return value;
    }

    /// Unique owner of the payload control block and its atomic borrow ledger.
    std::unique_ptr<detail::sync_borrow_ctrl<T>> ctrl_;
};

/**
 * @brief Atomically reference-counted shared owner implementation.
 *
 * @tparam T Object type owned by the shared control block.
 * @pre `T` is constructible from the constructor arguments and is not a
 * reference or array type.
 * @post Constructing from payload arguments creates strong count 1; copying
 * increments the atomic strong count; destruction decrements it and deletes the
 * payload when the count reaches zero.
 * @invariant All non-empty copies of the same `Arc<T>` share one stable
 * control-block address and one atomic `std::uint64_t` strong count.
 * @throws Construction may allocate or propagate `T` construction exceptions.
 * Copying may report `capacity_exhausted` under the configured violation
 * policy if the strong count is exhausted. Move operations and destruction do
 * not throw.
 * @note Ownership/thread-safety: distinct `Arc<T>` handle objects may be
 * copied and destroyed concurrently. Mutable payload access is not serialized;
 * use `Arc<Mutex<T>>` when shared mutation is required.
 *
 * Example:
 * @code
 * memsafe::Arc<memsafe::Mutex<int>> shared(0);
 * auto clone = shared;
 * {
 *     auto locked = clone->lock();
 *     ++*locked;
 * }
 * @endcode
 */
template <typename T>
class Arc final {
public:
    /**
     * @brief Construct an empty `Arc<T>`.
     *
     * @return No value; constructors initialize the receiving object.
     * @pre No payload is required.
     * @post `has_value()` is false and `strong_count()` is zero.
     * @invariant Empty `Arc<T>` handles have no release obligation.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: empty handles can be moved, assigned, and
     * destroyed without touching shared state.
     */
    Arc() noexcept = default;

    /**
     * @brief Construct a shared payload by forwarding arguments to `T`.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @tparam Enable SFINAE guard that participates only when `T` is
     * constructible from `Args&&...`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is a non-reference, non-array object type constructible from
     * `Args&&...`.
     * @post `has_value()` is true and `strong_count()` is 1.
     * @invariant The payload and atomic strong count live in one stable control
     * block until the last `Arc<T>` releases it.
     * @throws `std::bad_alloc` or any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the new control block is not shared until
     * the constructed `Arc<T>` is copied or moved to another thread.
     *
     * Example:
     * @code
     * memsafe::Arc<int> value(42);
     * int copy = *value;
     * (void)copy;
     * @endcode
     */
    template <typename... Args,
              typename Enable = typename std::enable_if<
                  std::is_constructible<T, Args&&...>::value>::type>
    explicit Arc(Args&&... args)
        : ctrl_(new detail::arc_ctrl<T>(std::forward<Args>(args)...)) {}

    /**
     * @brief Copy a shared owner and retain one strong reference.
     *
     * @param other Source owner whose control block is retained.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is either empty or points to a live `Arc<T>` control block.
     * Concurrent destruction of the same `other` handle object is not allowed;
     * concurrent operations on distinct handles sharing its control block are
     * allowed.
     * @post This owner shares `other`'s payload when retain succeeds; the
     * strong count is one larger.
     * @invariant Copying never duplicates the payload, only the atomic strong
     * ownership record.
     * @throws `memsafe::violation` under THROW policy if the strong count is
     * exhausted; installed handlers may throw.
     * @note Ownership/thread-safety: the retain operation is atomic and is safe
     * when copying from a live handle while other distinct handles are released.
     */
    Arc(const Arc& other) : ctrl_(detail::arc_retain(other.ctrl_)) {}

    /**
     * @brief Move a shared owner without touching the strong count.
     *
     * @param other Source owner whose control pointer is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `Arc<T>` handle and is not being accessed
     * concurrently.
     * @post This owner contains `other`'s former control pointer and `other` is
     * empty.
     * @invariant Moving transfers an existing release obligation without
     * incrementing or decrementing the atomic count.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: move the handle object only with ordinary
     * external synchronization for that object.
     */
    Arc(Arc&& other) noexcept : ctrl_(other.ctrl_) {
        other.ctrl_ = nullptr;
    }

    /**
     * @brief Copy-assign a shared owner and update strong counts atomically.
     *
     * @param other Source owner whose control block is retained.
     * @return Reference to this owner.
     * @pre `other` is either empty or points to a live control block. The same
     * handle object must not be assigned concurrently by another thread.
     * @post This owner shares `other`'s payload; the previous control block, if
     * any, has been released.
     * @invariant The new control block is retained before the old one is
     * released so self-assignment and aliasing cannot prematurely delete the
     * payload.
     * @throws `memsafe::violation` under THROW policy if the retain of
     * `other` exhausts the strong count; installed handlers may throw.
     * @note Ownership/thread-safety: retain/release are atomic for distinct
     * shared control blocks. Assignment of the receiving handle object itself
     * is not internally synchronized.
     */
    Arc& operator=(const Arc& other) {
        if (this != &other) {
            detail::arc_ctrl<T>* const retained =
                detail::arc_retain(other.ctrl_);
            detail::arc_release(ctrl_);
            ctrl_ = retained;
        }
        return *this;
    }

    /**
     * @brief Move-assign a shared owner without changing the incoming count.
     *
     * @param other Source owner whose control pointer is transferred.
     * @return Reference to this owner.
     * @pre `other` is a live `Arc<T>` handle and neither handle object is being
     * accessed concurrently.
     * @post This owner contains `other`'s former control pointer, `other` is
     * empty, and this owner's previous control block has been released.
     * @invariant The incoming release obligation is transferred exactly once.
     * @throws Nothing; this assignment operator is `noexcept`.
     * @note Ownership/thread-safety: move assignment is handle-object mutation;
     * use external synchronization for the participating handles.
     */
    Arc& operator=(Arc&& other) noexcept {
        if (this != &other) {
            detail::arc_release(ctrl_);
            ctrl_ = other.ctrl_;
            other.ctrl_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Release one strong reference.
     *
     * @return No value; destructors release the receiving object.
     * @pre No thread may concurrently mutate this handle object. Other distinct
     * handles sharing the same control block may be copied or destroyed.
     * @post The atomic strong count has been decremented; if this was the last
     * strong owner, the payload has been destroyed exactly once.
     * @invariant The final release is the only path that deletes the shared
     * control block.
     * @throws Nothing; this destructor is `noexcept`. If `T`'s destructor
     * throws, normal C++ termination rules for destructors apply.
     * @note Ownership/thread-safety: release is atomic and may race with
     * retain/release on distinct `Arc<T>` handles.
     */
    ~Arc() noexcept {
        detail::arc_release(ctrl_);
    }

    /**
     * @brief Return whether this shared owner contains a payload.
     *
     * @retval true This handle points to a live shared control block.
     * @retval false This handle is empty or moved from.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant Empty handles have no strong release obligation.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: observer for this handle object. Concurrent
     * mutation of the same handle still requires external synchronization.
     */
    bool has_value() const noexcept {
        return ctrl_ != nullptr;
    }

    /**
     * @brief Test whether this shared owner contains a payload.
     *
     * @retval true This handle points to a live shared control block.
     * @retval false This handle is empty or moved from.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant Equivalent to `has_value()`.
     * @throws Nothing; this conversion is `noexcept`.
     * @note Ownership/thread-safety: observer only; it does not retain the
     * control block.
     */
    explicit operator bool() const noexcept {
        return has_value();
    }

    /**
     * @brief Return an atomic snapshot of the strong count.
     *
     * @return Number of live `Arc<T>` handles sharing this control block, or
     * zero for an empty handle.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant The value is a snapshot and may become stale immediately when
     * other threads operate on distinct handles.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic observer intended for tests and
     * diagnostics, not for lifetime decisions.
     */
    std::uint64_t strong_count() const noexcept {
        return ctrl_ != nullptr ? ctrl_->strong_count() : 0U;
    }

    /**
     * @brief Return the shared-owner use count using `shared_ptr` naming.
     *
     * @return Same value as `strong_count()`.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant This is an alias for `strong_count()` so tests can use the
     * familiar `use_count` vocabulary without changing ownership semantics.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic snapshot only.
     */
    std::uint64_t use_count() const noexcept {
        return strong_count();
    }

    /**
     * @brief Return the shared payload pointer.
     *
     * @return Pointer to the payload, or null when this handle is empty.
     * @pre The handle object is alive.
     * @post No reference-count state is modified.
     * @invariant The pointer remains valid while at least one `Arc<T>` owning
     * the same control block remains alive.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: returned pointer is non-owning and does
     * not synchronize access to `T`.
     */
    T* get() const noexcept {
        return ctrl_ != nullptr ? ctrl_->get() : nullptr;
    }

    /**
     * @brief Dereference the shared payload.
     *
     * @return Mutable reference to the shared payload. For immutable shared
     * payloads, instantiate `Arc<const T>`.
     * @pre `has_value()` is true, and the caller has appropriate payload-level
     * synchronization when `T` is mutable and shared across threads.
     * @post No reference-count state is modified.
     * @invariant `Arc<T>` models shared ownership, not exclusive payload access;
     * constness of the handle does not make the payload immutable.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner; installed handlers may throw. With release checks off, no
     * null check is emitted.
     * @note Ownership/thread-safety: the reference is bounded by the lifetime
     * of a non-empty shared control block and is not itself synchronized.
     */
    T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the shared payload.
     *
     * @return Mutable pointer to the shared payload. For immutable shared
     * payloads, instantiate `Arc<const T>`.
     * @pre `has_value()` is true, and mutable payload access is synchronized by
     * the caller when needed.
     * @post No reference-count state is modified.
     * @invariant `Arc<T>` exposes shared ownership only; payload data-race
     * freedom is a separate caller obligation.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner; installed handlers may throw. With release checks off, no
     * null check is emitted.
     * @note Ownership/thread-safety: the pointer is non-owning and remains
     * valid while some `Arc<T>` keeps the control block alive.
     */
    T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

    /**
     * @brief Release this handle's current strong reference.
     *
     * @return Nothing.
     * @pre The handle object is alive and is not being concurrently mutated.
     * @post This handle is empty. The previous control block, if any, has been
     * released and may have been destroyed at count zero.
     * @invariant Reset transfers no ownership elsewhere; it only consumes this
     * handle's current release obligation.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: release is atomic for the shared control
     * block, but mutation of this handle object itself requires external
     * synchronization.
     */
    void reset() noexcept {
        detail::arc_ctrl<T>* const old = ctrl_;
        ctrl_ = nullptr;
        detail::arc_release(old);
    }

private:
    /**
     * @brief Return the payload pointer or report an empty shared owner.
     *
     * @return Mutable pointer to the shared payload.
     * @pre `has_value()` is true.
     * @post No reference-count state is modified.
     * @invariant Debug-check null detection reports
     * `violation_kind::null_access`; release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the handle is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning and does
     * not synchronize payload access.
     */
    T* require_value() const {
        T* const value = get();
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty Arc");
        }
#endif
        return value;
    }

    /// Shared control block retained by this handle, or null when empty.
    detail::arc_ctrl<T>* ctrl_ = nullptr;
};

/**
 * @brief Mutual-exclusion wrapper implementation for one payload.
 *
 * @tparam T Object type guarded by the mutex.
 * @pre `T` is constructible from the constructor arguments and is not a
 * reference or array type.
 * @post Construction stores one payload. Each successful `lock()` call returns
 * a move-only `MutRef<T>` that unlocks on destruction.
 * @invariant The underlying `std::mutex` serializes all access through the
 * returned `MutRef<T>`; debug-check builds mirror the live borrow with a
 * one-token atomic borrow ledger and the owner-compatible borrow control block
 * records the `MutRef<T>` token.
 * @throws Construction may propagate `T` construction exceptions. `lock()` may
 * throw `std::system_error` from the standard mutex or report a memsafe
 * violation if the debug borrow ledger is corrupt.
 * @note Ownership/thread-safety: concurrent `lock()` calls are safe while the
 * `Mutex<T>` remains alive. Destroying or moving the mutex while a returned
 * `MutRef<T>` is live is outside the contract; copying and moving are
 * disabled.
 *
 * Example:
 * @code
 * memsafe::Mutex<int> counter(0);
 * {
 *     auto locked = counter.lock();
 *     ++*locked;
 * }
 * @endcode
 */
template <typename T>
class Mutex final {
public:
    static_assert(!std::is_reference<T>::value,
                  "memsafe::Mutex<T> cannot guard a reference type");
    static_assert(!std::is_array<T>::value,
                  "memsafe::Mutex<T> does not support array types");

    /**
     * @brief Construct the guarded payload from forwarded arguments.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @tparam Enable SFINAE guard that participates only when `T` is
     * constructible from `Args&&...`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is a non-reference, non-array object type constructible from
     * `Args&&...`.
     * @post The payload is initialized and no mutex-backed `MutRef<T>` is
     * live.
     * @invariant The payload address is stable for the lifetime of the mutex
     * wrapper.
     * @throws Any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the constructed mutex may be shared across
     * threads only after construction completes.
     */
    template <typename... Args,
              typename Enable = typename std::enable_if<
                  std::is_constructible<T, Args&&...>::value>::type>
    explicit Mutex(Args&&... args) : ctrl_(std::forward<Args>(args)...) {}

    /**
     * @brief Copy construction is disabled for mutex ownership.
     *
     * @param other Source mutex that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `Mutex<T>` is a compile-time error.
     * @post No duplicate `std::mutex` or duplicated guarded payload is created.
     * @invariant A guarded payload has exactly one mutex wrapper.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents two mutexes from
     * pretending to guard the same logical state.
     */
    Mutex(const Mutex& other) = delete;

    /**
     * @brief Copy assignment is disabled for mutex ownership.
     *
     * @param other Source mutex that would otherwise replace this mutex.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post The mutex and payload remain uniquely owned by this wrapper.
     * @invariant Lock state cannot be duplicated or overwritten by assignment.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: `std::mutex` itself is non-copyable, and
     * the wrapper follows that ownership model.
     */
    Mutex& operator=(const Mutex& other) = delete;

    /**
     * @brief Move construction is disabled to preserve mutex address stability.
     *
     * @param other Source mutex that would otherwise be moved.
     * @return No value; this overload is deleted.
     * @pre Not available. Move construction is a compile-time error.
     * @post Existing mutex-backed `MutRef<T>` objects cannot observe a
     * relocated mutex.
     * @invariant The mutex object address is stable for all live mutex-backed
     * borrows.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: moving a locked mutex is not a portable
     * operation, so the wrapper is intentionally non-movable.
     */
    Mutex(Mutex&& other) = delete;

    /**
     * @brief Move assignment is disabled to preserve mutex address stability.
     *
     * @param other Source mutex that would otherwise replace this mutex.
     * @return No value; this overload is deleted.
     * @pre Not available. Move assignment is a compile-time error.
     * @post No mutex state is relocated or overwritten.
     * @invariant Mutex-backed borrows always refer to the same mutex address
     * they locked.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: stable mutex identity is required for
     * reliable unlock in the `MutRef<T>` release hook.
     */
    Mutex& operator=(Mutex&& other) = delete;

    /**
     * @brief Destroy the guarded payload and mutex.
     *
     * @return No value; destructors release the receiving object.
     * @pre No mutex-backed `MutRef<T>` for this mutex is live and no thread is
     * blocked in or about to call `lock()` on this object.
     * @post The guarded payload has been destroyed exactly once.
     * @invariant Destruction is valid only after all lock handles have ended.
     * @throws No exception may escape this destructor. If `T`'s destructor
     * throws during cleanup, normal C++ destructor termination rules apply.
     * @note Ownership/thread-safety: callers must externally join threads and
     * end returned borrows before destroying the mutex.
     */
    ~Mutex() = default;

    /**
     * @brief Lock the mutex and return an exclusive mutable reference.
     *
     * @return `MutRef<T>` by value so both function-level and type-level
     * `MEMSAFE_NODISCARD` diagnostics can catch a discarded lock.
     * @pre The mutex object is alive.
     * @post On success, the returned `MutRef<T>` keeps the standard mutex
     * locked and the debug borrow ledger records one exclusive mutable borrow
     * until the borrow is destroyed, moved away, or assigned over.
     * @invariant At most one live `MutRef<T>` returned from this mutex can
     * access the payload through this API at a time.
     * @throws `std::system_error` from `std::mutex::lock`; `memsafe::violation`
     * under THROW policy if debug checks detect a corrupt live-borrow ledger;
     * installed handlers may throw.
     * @note Ownership/thread-safety: concurrent callers block in the standard
     * mutex and therefore serialize increments or other mutable operations.
     *
     * Example:
     * @code
     * memsafe::Mutex<int> counter(0);
     * auto value = counter.lock();
     * ++*value;
     * @endcode
     */
    MEMSAFE_NODISCARD MutRef<T> lock() MEMSAFE_BORROWS(*this);

    /**
     * @brief Report whether a debug-check exclusive mutex borrow is live.
     *
     * @retval true A mutex-backed `MutRef<T>` has recorded the exclusive
     * borrow token.
     * @retval false No mutex-backed borrow is recorded, or release checks are
     * disabled.
     * @pre The mutex object is alive.
     * @post No state is modified.
     * @invariant In release no-check lanes this function is a constant false
     * observer and no borrow-state field is emitted.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: atomic observer for diagnostics and tests.
     */
    bool has_borrow() const noexcept {
#if MEMSAFE_RELEASE_CHECKS
        return borrow_state_.load(std::memory_order_acquire) != 0U;
#else
        return false;
#endif
    }

private:
    /**
     * @brief Record that a `MutRef<T>` has acquired the mutex.
     *
     * @retval true The exclusive borrow token was recorded.
     * @pre The caller currently owns `mutex_`.
     * @post With debug checks enabled, the borrow state is one. With release
     * checks disabled, no state is modified.
     * @invariant The standard mutex should make any pre-existing token
     * impossible; a failure reports ledger corruption rather than spinning and
     * then restores a coherent one-token state if a handler returns.
     * @throws `memsafe::violation` under THROW policy if the token is already
     * recorded; installed handlers may throw.
     * @note Ownership/thread-safety: called only after `std::mutex::lock`
     * succeeds, so payload access is already serialized.
     */
    bool record_lock_borrow() {
#if MEMSAFE_RELEASE_CHECKS
        std::uint32_t expected = 0U;
        if (borrow_state_.compare_exchange_strong(expected,
                                                  1U,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return true;
        }
        /*
         * F1 Slice 5 describes Mutex::lock() as yielding one exclusive mutable
         * borrow. Seeing the token already set after std::mutex acquisition
         * means the debug ledger no longer mirrors the lock state.
         */
        MEMSAFE_DETAIL_VIOLATE(
            ::memsafe::violation_kind::borrow_exclusivity,
            "mutex borrow ledger already contains a live MutRef");
        borrow_state_.store(1U, std::memory_order_release);
        return true;
#else
        return true;
#endif
    }

    /**
     * @brief Clear the debug-check exclusive mutex borrow token.
     *
     * @return Nothing.
     * @pre The caller owns the mutex lock and previously recorded a lock
     * borrow for the returned `MutRef<T>`.
     * @post With debug checks enabled, `has_borrow()` is false before the
     * standard mutex is unlocked. With release checks disabled, no state is
     * modified.
     * @invariant Clearing before unlock prevents a racing next locker from
     * observing the previous borrow's token.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: called by the `MutRef<T>` release hook
     * while the mutex is still held.
     */
    void clear_lock_borrow() noexcept {
#if MEMSAFE_RELEASE_CHECKS
        borrow_state_.store(0U, std::memory_order_release);
#endif
    }

    /**
     * @brief Release a mutex-backed `MutRef<T>` and unlock its mutex.
     *
     * @param ctrl Borrow-control block whose mutable token belongs to the
     * returned `MutRef<T>`.
     * @param state Opaque pointer to the owning `Mutex<T>`.
     * @return Nothing.
     * @pre `state` points to the live mutex that created the borrow, `ctrl`
     * points to `state->ctrl_`, and the current thread owns `state->mutex_`.
     * @post The owner-compatible mutable borrow token is released, the mutex
     * borrow ledger is cleared, and the standard mutex is unlocked.
     * @invariant F1 Concurrency Types requires `Mutex<T>::lock()` to yield the
     * public `MutRef<T>` type; the private hook binds that required type to
     * standard mutex RAII without exposing a second guard type.
     * @throws Nothing; this callback is used from `MutRef<T>` destruction and
     * move assignment.
     * @note Ownership/thread-safety: cleanup runs before unlock so the next
     * locking thread observes an empty borrow ledger after acquiring the mutex.
     */
    static void release_mutex_borrow(detail::borrow_ctrl<T>* ctrl,
                                     void* state) noexcept {
        Mutex<T>* const owner = static_cast<Mutex<T>*>(state);
        detail::release_mut(ctrl);
        owner->clear_lock_borrow();
        owner->mutex_.unlock();
    }

    /// Standard mutex that serializes access to `ctrl_`'s payload.
    std::mutex mutex_;
    /// Owner-compatible payload and `MutRef<T>` borrow ledger protected by `mutex_`.
    detail::borrow_ctrl<T> ctrl_;
#if MEMSAFE_RELEASE_CHECKS
    /// Debug-check mirror of the single exclusive mutex-backed borrow.
    std::atomic<std::uint32_t> borrow_state_{0U};
#endif
};

/**
 * @brief Lock a mutex and return its exclusive mutable reference.
 *
 * @tparam T Object type guarded by the mutex.
 * @return `MutRef<T>` that keeps the mutex locked until destruction or
 * reassignment.
 * @pre The mutex object is alive.
 * @post The returned `MutRef<T>` serializes mutable access to the payload and
 * owns a private release hook that clears the borrow ledgers before unlocking.
 * @invariant The standard mutex is acquired before either borrow ledger is
 * recorded, and it remains held until the returned `MutRef<T>` releases.
 * @throws `std::system_error` from `std::mutex::lock`; `memsafe::violation`
 * under THROW policy if the debug ledger is inconsistent; installed handlers
 * may throw.
 * @note Ownership/thread-safety: concurrent callers serialize through the
 * standard mutex. This exact `MutRef<T>` return type satisfies F1
 * Concurrency Types and CPP_MEMSAFE-0500-FUNC acceptance.
 */
template <typename T>
inline MutRef<T> Mutex<T>::lock() MEMSAFE_BORROWS(*this) {
    mutex_.lock();
    try {
        /*
         * F1 Slice 5 requires the public result to be MutRef<T>, not a
         * separate guard. The lock is therefore bound to MutRef's private
         * trusted-friend release hook after the mutex ledger is recorded.
         */
        record_lock_borrow();
    } catch (...) {
        mutex_.unlock();
        throw;
    }
    return MutRef<T>(ctrl_, &Mutex<T>::release_mutex_borrow, this);
}

} // namespace memsafe

#endif /* MEMSAFE_SYNC_HPP */
