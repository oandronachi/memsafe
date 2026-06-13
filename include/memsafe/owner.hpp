/**
 * @file owner.hpp
 * @brief Slice 1 owner and borrow primitives for the header-only memsafe library.
 *
 * @details
 * Work package: CPP_MEMSAFE-0100-FUNC, with the private
 * CPP_MEMSAFE-0500-FUNC friend-hook integration used by `Mutex<T>::lock()`.
 *
 * Purpose:
 * - Define `memsafe::Owner<T>`, the move-only RAII owner for one heap-allocated
 *   `T`.
 * - Define `memsafe::Ref<T>`, the copyable immutable borrow returned by
 *   `Owner<T>::borrow()`.
 * - Define `memsafe::MutRef<T>`, the non-copyable exclusive mutable borrow
 *   returned by `Owner<T>::borrow_mut()`.
 * - Provide `memsafe::detail::borrow_ctrl<T>`, the private counter-bearing
 *   control object used by Slice 1 borrow handles.
 * - Preserve `MutRef<T>` as the public Slice 5 mutex lock return type by
 *   allowing trusted `Mutex<T>` construction to attach a private release hook.
 *
 * Key invariants:
 * - `Owner<T>` is non-copyable and has an unconditionally `noexcept(true)` move
 *   constructor and move-assignment operator, matching F1 ownership semantics
 *   and the F3 `Move_Noexcept` property.
 * - `Ref<T>` and `MutRef<T>` are type-level `MEMSAFE_NODISCARD` classes, and
 *   `Owner<T>::borrow()` / `Owner<T>::borrow_mut()` return them by value so
 *   ordinary `-Wunused-result` diagnostics can catch discarded borrows.
 * - With `MEMSAFE_RELEASE_CHECKS != 0`, immutable borrows increment the shared
 *   count, mutable borrows set the exclusive token, and requesting a mutable
 *   borrow while any borrow exists reports `violation_kind::borrow_exclusivity`.
 * - With `MEMSAFE_RELEASE_CHECKS == 0`, counter fields and violation call sites
 *   are not compiled into `borrow_ctrl<T>`, preserving the release no-checks
 *   lane required by F1 and F4.
 * - Borrow-returning functions and borrow-handle dereference paths carry
 *   `MEMSAFE_BORROWS(...)` so the Slice 7 Clang co_await lane can attach
 *   lifetime and coroutine lifetime annotations without changing this API.
 * - The CPP_MEMSAFE-0500-FUNC friend hook does not change public construction:
 *   only `Owner<T>` and `Mutex<T>` can create `MutRef<T>` values.
 *
 * Ownership and thread-safety:
 * - `Owner<T>` uniquely owns the heap allocation. `Ref<T>` and `MutRef<T>` are
 *   non-owning handles into the owning `Owner<T>` control block and must not be
 *   used after the owning `Owner<T>` has been destroyed.
 * - The Slice 1 types are single-threaded. Borrow counters are ordinary
 *   non-atomic fields; cross-thread borrowing is reserved for Slice 4
 *   `SyncOwner<T>`, `SyncRef<T>`, and `SyncMutRef<T>`.
 */
#ifndef MEMSAFE_OWNER_HPP
#define MEMSAFE_OWNER_HPP

#include <memsafe/config.hpp>
#include <memsafe/backend.hpp>
#include <memsafe/violation.hpp>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace memsafe {

/**
 * @brief Move-only owner for a heap-allocated object.
 *
 * @tparam T Object type owned by the `Owner`.
 * @pre `T` is a complete non-reference, non-array object type whenever an
 * `Owner<T>` is constructed.
 * @post Construction creates one heap-allocated `T`; destruction releases it
 * exactly once.
 * @invariant At most one live `Owner<T>` owns a given control block, and all
 * live borrows observe that control block without owning it.
 * @throws Public construction may throw allocation exceptions or exceptions
 * propagated from `T` construction. Move construction and move assignment are
 * unconditionally `noexcept(true)`.
 * @note Ownership/thread-safety: ownership is unique and single-threaded.
 * Borrow handles are non-owning and must not outlive the owner.
 *
 * Example:
 * @code
 * memsafe::Owner<int> value(7);
 * {
 *     auto read = value.borrow();
 *     int copy = *read;
 *     (void)copy;
 * }
 * {
 *     auto write = value.borrow_mut();
 *     *write = 9;
 * }
 * @endcode
 */
template <typename T>
class Owner;

/**
 * @brief Immutable counted borrow of an `Owner<T>` payload.
 *
 * @tparam T Object type borrowed from the owner.
 * @pre Instances are created by `Owner<T>::borrow()` or trusted friends, not by
 * user code directly.
 * @post Copying a `Ref<T>` creates another immutable borrow in debug-check
 * lanes; destroying it releases one immutable borrow.
 * @invariant A live `Ref<T>` never grants mutable access to the payload.
 * @throws Copy construction, copy assignment, and factory construction may
 * report `borrow_exclusivity` under THROW policy if a corrupt or invalid
 * control state already contains a mutable borrow. Move operations and
 * destruction do not throw.
 * @note Ownership/thread-safety: `Ref<T>` is non-owning and single-threaded.
 * It may not be held across `co_await`; `MEMSAFE_BORROWS` supplies the Slice 7
 * backend metadata for compilers that can check that rule.
 *
 * Example:
 * @code
 * memsafe::Owner<int> owner(5);
 * auto a = owner.borrow();
 * auto b = a;
 * int total = *a + *b;
 * (void)total;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD Ref;

/**
 * @brief Exclusive counted mutable borrow of an `Owner<T>` payload.
 *
 * @tparam T Object type borrowed mutably from the owner.
 * @pre Instances are created by `Owner<T>::borrow_mut()` or trusted friends,
 * not by user code directly.
 * @post Construction acquires the exclusive token in debug-check lanes;
 * destruction releases it.
 * @invariant `MutRef<T>` is non-copyable, and a successfully acquired mutable
 * borrow excludes both immutable and other mutable borrows while checks are
 * enabled.
 * @throws Factory construction may report `borrow_exclusivity` under THROW
 * policy. Move operations and destruction do not throw.
 * @note Ownership/thread-safety: `MutRef<T>` is non-owning and single-threaded.
 * It may not be held across `co_await`; `MEMSAFE_BORROWS` supplies the Slice 7
 * backend metadata for compilers that can check that rule.
 *
 * Example:
 * @code
 * memsafe::Owner<int> owner(5);
 * auto borrow = owner.borrow_mut();
 * *borrow = 8;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD MutRef;

/**
 * @brief Forward declaration for Slice 5 `Mutex<T>` integration.
 *
 * @tparam T Guarded object type.
 * @pre This declaration is used only for friendship; Slice 5 supplies the
 * eventual definition.
 * @post No type is defined by this header.
 * @invariant The declaration keeps `MutRef<T>` construction available to the
 * future mutex wrapper without broadening public constructors.
 * @throws Nothing; this is a declaration only.
 * @note Ownership/thread-safety: no storage or synchronization is introduced
 * by this forward declaration.
 */
template <typename T>
class Mutex;

namespace detail {

/**
 * @brief Control block that owns one payload and tracks Slice 1 borrows.
 *
 * @tparam T Object type stored in the control block.
 * @pre `T` is a complete non-reference, non-array object type and is
 * constructible from the arguments passed to the constructor.
 * @post Construction allocates and constructs one `T`; destruction releases it.
 * @invariant When debug checks are enabled, `shared_borrows_` counts live
 * immutable borrows and `mutable_borrowed_` records whether one exclusive borrow
 * is live. When release checks are disabled, those fields and all violation
 * branches are absent.
 * @throws Construction may throw allocation exceptions or `T` construction
 * exceptions. Borrow acquisition may report through the active violation policy
 * when exclusivity is broken.
 * @note Ownership/thread-safety: this is a private single-owner,
 * single-threaded control block. Borrow handles point to it but do not own it.
 */
template <typename T>
class borrow_ctrl final {
public:
    static_assert(!std::is_reference<T>::value,
                  "memsafe::Owner<T> cannot own a reference type");
    static_assert(!std::is_array<T>::value,
                  "memsafe::Owner<T> does not support array types");

    /**
     * @brief Construct the owned payload from forwarded arguments.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return No value; constructors initialize the receiving object.
     * @pre `T` is constructible from `Args&&...`.
     * @post `get()` returns a non-null pointer to the new `T`, and no borrows
     * are live.
     * @invariant The payload is heap-allocated so moving `Owner<T>` moves only
     * a pointer-bearing control object, not the `T` object itself.
     * @throws `std::bad_alloc` or any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the control block owns the payload and is
     * intended for one owning `Owner<T>` at a time.
     */
    template <typename... Args>
    explicit borrow_ctrl(Args&&... args)
        : value_(std::make_unique<T>(std::forward<Args>(args)...)) {}

    /**
     * @brief Copy construction is disabled for unique ownership.
     *
     * @param other Source control block that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a control block is a compile-time error.
     * @post No duplicate owner or duplicate borrow counters are created.
     * @invariant A payload has one control block and one owning `Owner<T>`.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents double deletion
     * and split borrow counters.
     */
    borrow_ctrl(const borrow_ctrl& other) = delete;

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
     * two owners to one borrow ledger.
     */
    borrow_ctrl& operator=(const borrow_ctrl& other) = delete;

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
     * non-owning `Ref<T>` and `MutRef<T>` handles simple and predictable.
     */
    borrow_ctrl(borrow_ctrl&& other) = delete;

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
     * dangling borrow-control pointers after `Owner<T>` moves.
     */
    borrow_ctrl& operator=(borrow_ctrl&& other) = delete;

    /**
     * @brief Destroy the payload and borrow ledger.
     *
     * @return No value; destructors release the receiving object.
     * @pre No `Ref<T>` or `MutRef<T>` that points at this control block may be
     * used after destruction begins.
     * @post The owned `T` has been destroyed exactly once.
     * @invariant Destruction is the sole normal release path for the owned
     * heap allocation.
     * @throws No exception may escape this destructor. If `T` violates the
     * ordinary C++ expectation that destructors do not throw during cleanup, the
     * runtime termination rules for destructors apply.
     * @note Ownership/thread-safety: destruction requires exclusive access to
     * the owning `Owner<T>` and absence of live borrow use.
     */
    ~borrow_ctrl() = default;

    /**
     * @brief Return whether the control block currently owns a payload.
     *
     * @return True when `get()` would return a non-null pointer.
     * @pre The control block is alive.
     * @post No state is modified.
     * @invariant A constructed control block owns exactly one payload until it
     * is destroyed.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: observer only; no synchronization is
     * performed.
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
     * @post No state is modified.
     * @invariant Normal live control blocks return the same stable payload
     * address for their lifetime.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is non-owning and must be used
     * according to the active borrow state.
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
     * @post No state is modified.
     * @invariant Normal live control blocks return the same stable payload
     * address for their lifetime.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is non-owning and must be used
     * according to the active borrow state.
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
     * observer and no counter fields are present.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: single-threaded observer for tests and
     * internal checks.
     */
    bool has_borrow() const noexcept {
#if MEMSAFE_RELEASE_CHECKS
        return shared_borrows_ != 0U || mutable_borrowed_;
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
     * @post With debug checks enabled, `shared_borrows_` is incremented only
     * when no mutable borrow is live. With release checks disabled, no state is
     * modified and the function returns true.
     * @invariant Shared borrows may coexist with other shared borrows but not
     * with a mutable borrow in debug-check lanes.
     * @throws `memsafe::violation` when THROW policy is active and a mutable
     * borrow is already live; installed handlers may throw.
     * @note Ownership/thread-safety: this is non-atomic and requires
     * single-threaded use of one `Owner<T>`.
     */
    bool acquire_shared() {
#if MEMSAFE_RELEASE_CHECKS
        if (mutable_borrowed_) {
            /*
             * F1 Slice 1 and F2 Ownership case 2 require the same
             * borrow_exclusivity violation for every shared/exclusive overlap.
             * If HANDLER policy returns, do not record a borrow that would make
             * the debug ledger internally inconsistent.
             */
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::borrow_exclusivity,
                "cannot create immutable borrow while a mutable borrow exists");
            return false;
        }
        ++shared_borrows_;
#endif
        return true;
    }

    /**
     * @brief Release one immutable borrow.
     *
     * @return Nothing.
     * @pre The control block is alive and, in debug-check lanes, the caller
     * previously acquired one immutable borrow.
     * @post With debug checks enabled, the shared-borrow count is decremented
     * when it is nonzero. With release checks disabled, no state is modified.
     * @invariant Correct `Ref<T>` lifetimes balance every successful
     * `acquire_shared()` call with one release.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: non-atomic release for single-threaded
     * borrow handles.
     */
    void release_shared() noexcept {
#if MEMSAFE_RELEASE_CHECKS
        if (shared_borrows_ != 0U) {
            --shared_borrows_;
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
     * @post With debug checks enabled, `mutable_borrowed_` is set only when no
     * shared or mutable borrow is live. With release checks disabled, no state
     * is modified and the function returns true.
     * @invariant A successful mutable borrow excludes every other borrow while
     * debug checks are enabled.
     * @throws `memsafe::violation` when THROW policy is active and any borrow
     * is already live; installed handlers may throw.
     * @note Ownership/thread-safety: this is non-atomic and requires
     * single-threaded use of one `Owner<T>`.
     */
    bool acquire_mut() {
#if MEMSAFE_RELEASE_CHECKS
        if (has_borrow()) {
            /*
             * This is the package's central acceptance criterion: mutable
             * borrowing while any borrow exists must report borrow_exclusivity
             * in Debug_Checks, and the whole branch is removed in release.
             */
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::borrow_exclusivity,
                "cannot create mutable borrow while another borrow exists");
            return false;
        }
        mutable_borrowed_ = true;
#endif
        return true;
    }

    /**
     * @brief Release the exclusive mutable borrow token.
     *
     * @return Nothing.
     * @pre The control block is alive and, in debug-check lanes, the caller
     * previously acquired the mutable borrow token.
     * @post With debug checks enabled, the mutable token is cleared. With
     * release checks disabled, no state is modified.
     * @invariant Correct `MutRef<T>` lifetimes balance every successful
     * `acquire_mut()` call with one release.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: non-atomic release for single-threaded
     * borrow handles.
     */
    void release_mut() noexcept {
#if MEMSAFE_RELEASE_CHECKS
        mutable_borrowed_ = false;
#endif
    }

private:
    /// Owned heap allocation that backs `Owner<T>` dereference and borrows.
    std::unique_ptr<T> value_;

#if MEMSAFE_RELEASE_CHECKS
    /// Count of live immutable `Ref<T>` handles in debug-check lanes.
    std::size_t shared_borrows_ = 0U;
    /// True while one live `MutRef<T>` owns the exclusive borrow token.
    bool mutable_borrowed_ = false;
#endif
};

/**
 * @brief Acquire a shared borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to acquire from, or null for an already-empty
 * borrow handle.
 * @return `ctrl` when acquisition succeeds; otherwise null.
 * @pre If non-null, `ctrl` points to a live `borrow_ctrl<T>`.
 * @post A successful debug-check acquisition increments the shared count.
 * @invariant Null inputs stay null and do not touch counters.
 * @throws `memsafe::violation` or an installed-handler exception if acquisition
 * reports a violation.
 * @note Ownership/thread-safety: this helper does not own the control block and
 * performs no synchronization.
 */
template <typename T>
inline borrow_ctrl<T>* retain_shared(borrow_ctrl<T>* ctrl) {
    if (ctrl != nullptr && ctrl->acquire_shared()) {
        return ctrl;
    }
    return nullptr;
}

/**
 * @brief Release a shared borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to release from, or null.
 * @return Nothing.
 * @pre If non-null, `ctrl` points to a live `borrow_ctrl<T>` and the caller
 * owns one shared borrow acquired from it.
 * @post The shared count is decremented in debug-check lanes.
 * @invariant Null inputs are no-ops.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper does not own the control block and
 * performs no synchronization.
 */
template <typename T>
inline void release_shared(borrow_ctrl<T>* ctrl) noexcept {
    if (ctrl != nullptr) {
        ctrl->release_shared();
    }
}

/**
 * @brief Acquire an exclusive mutable borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to acquire from, or null for an already-empty
 * borrow handle.
 * @return `ctrl` when acquisition succeeds; otherwise null.
 * @pre If non-null, `ctrl` points to a live `borrow_ctrl<T>`.
 * @post A successful debug-check acquisition sets the mutable token.
 * @invariant Null inputs stay null and do not touch counters.
 * @throws `memsafe::violation` or an installed-handler exception if acquisition
 * reports a violation.
 * @note Ownership/thread-safety: this helper does not own the control block and
 * performs no synchronization.
 */
template <typename T>
inline borrow_ctrl<T>* retain_mut(borrow_ctrl<T>* ctrl) {
    if (ctrl != nullptr && ctrl->acquire_mut()) {
        return ctrl;
    }
    return nullptr;
}

/**
 * @brief Release an exclusive mutable borrow for a nullable control pointer.
 *
 * @tparam T Object type stored in the control block.
 * @param ctrl Control pointer to release from, or null.
 * @return Nothing.
 * @pre If non-null, `ctrl` points to a live `borrow_ctrl<T>` and the caller
 * owns the mutable token acquired from it.
 * @post The mutable token is cleared in debug-check lanes.
 * @invariant Null inputs are no-ops.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper does not own the control block and
 * performs no synchronization.
 */
template <typename T>
inline void release_mut(borrow_ctrl<T>* ctrl) noexcept {
    if (ctrl != nullptr) {
        ctrl->release_mut();
    }
}

} // namespace detail

/**
 * @brief Immutable counted borrow implementation for `Owner<T>`.
 *
 * @tparam T Object type borrowed immutably.
 * @pre Construct through `Owner<T>::borrow()` or a trusted friend so the debug
 * borrow counter is acquired consistently.
 * @post Destruction or reassignment releases one shared borrow when checks are
 * enabled.
 * @invariant `Ref<T>` is copyable, non-owning, type-level nodiscard, and never
 * exposes mutable access to the payload.
 * @throws Copy and factory construction may report through the configured
 * violation policy; move and destruction operations are `noexcept`.
 * @note Ownership/thread-safety: single-threaded non-owning borrow handle. The
 * `MEMSAFE_BORROWS` annotations on construction and dereference carry the Slice
 * 7 co_await/lifetime contract.
 */
template <typename T>
class MEMSAFE_NODISCARD Ref final {
public:
    /**
     * @brief Copy an immutable borrow and increment the shared count.
     *
     * @param other Borrow handle to copy.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is either empty or points to a live owner control block.
     * @post This object refers to the same payload as `other` when acquisition
     * succeeds. In debug-check lanes the shared-borrow count is incremented.
     * @invariant Copying preserves immutable-only access.
     * @throws `memsafe::violation` under THROW policy if the source control
     * block cannot accept a shared borrow; installed handlers may throw.
     * @note Ownership/thread-safety: the copied handle remains non-owning and
     * single-threaded.
     */
    Ref(const Ref& other) : ctrl_(detail::retain_shared(other.ctrl_)) {}

    /**
     * @brief Move an immutable borrow without changing the shared count.
     *
     * @param other Borrow handle whose recorded borrow is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `Ref<T>`.
     * @post This object owns `other`'s borrow record and `other` is empty.
     * @invariant Moving does not create or destroy a borrow-count entry.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the borrow remains non-owning and tied to
     * the same single-threaded owner control block.
     */
    Ref(Ref&& other) noexcept : ctrl_(other.ctrl_) {
        other.ctrl_ = nullptr;
    }

    /**
     * @brief Copy-assign an immutable borrow.
     *
     * @param other Borrow handle to copy from.
     * @return Reference to this borrow handle.
     * @pre `other` is either empty or points to a live owner control block.
     * @post This object refers to the same payload as `other` when acquisition
     * succeeds; the previously held borrow, if any, is released.
     * @invariant The new borrow is acquired before the old one is released so a
     * throwing violation policy leaves this object unchanged.
     * @throws `memsafe::violation` under THROW policy if the new source control
     * block cannot accept a shared borrow; installed handlers may throw.
     * @note Ownership/thread-safety: assignment does not transfer ownership of
     * the payload or synchronize between threads.
     */
    Ref& operator=(const Ref& other) {
        if (this != &other) {
            detail::borrow_ctrl<T>* const next = detail::retain_shared(other.ctrl_);
            detail::release_shared(ctrl_);
            ctrl_ = next;
        }
        return *this;
    }

    /**
     * @brief Move-assign an immutable borrow.
     *
     * @param other Borrow handle whose recorded borrow is transferred.
     * @return Reference to this borrow handle.
     * @pre `other` is a live `Ref<T>`.
     * @post This object owns `other`'s previous borrow record, the previously
     * held borrow is released, and `other` is empty.
     * @invariant Moving does not create an additional shared-count entry.
     * @throws Nothing; this assignment operator is `noexcept`.
     * @note Ownership/thread-safety: assignment remains single-threaded and
     * non-owning.
     */
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            detail::release_shared(ctrl_);
            ctrl_ = other.ctrl_;
            other.ctrl_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Release this immutable borrow.
     *
     * @return No value; destructors release the receiving object.
     * @pre The owning `Owner<T>` control block is still alive when this handle
     * is non-empty.
     * @post The shared-borrow count is decremented in debug-check lanes.
     * @invariant Each successful acquisition is released exactly once by
     * destruction or assignment.
     * @throws Nothing; this destructor is `noexcept`.
     * @note Ownership/thread-safety: destruction releases a non-owning,
     * single-threaded borrow record.
     */
    ~Ref() noexcept {
        detail::release_shared(ctrl_);
    }

    /**
     * @brief Dereference the immutable borrow.
     *
     * @return Const reference to the borrowed payload.
     * @pre This handle is non-empty and its owning `Owner<T>` is still alive.
     * @post No borrow state is modified.
     * @invariant The returned reference does not permit mutation through this
     * `Ref<T>`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the reference is non-owning and bounded by
     * both this `Ref<T>` and the owning `Owner<T>`.
     */
    const T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the immutable borrowed payload.
     *
     * @return Const pointer to the borrowed payload.
     * @pre This handle is non-empty and its owning `Owner<T>` is still alive.
     * @post No borrow state is modified.
     * @invariant The returned pointer does not permit mutation through this
     * `Ref<T>`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the pointer is non-owning and bounded by
     * both this `Ref<T>` and the owning `Owner<T>`.
     */
    const T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

private:
    friend class Owner<T>;
    template <typename>
    friend class Mutex;

    /**
     * @brief Construct a borrow from an owner control block.
     *
     * @param ctrl Owner control block that supplies the payload and counter.
     * @return No value; constructors initialize the receiving object.
     * @pre `ctrl` is alive and belongs to a live `Owner<T>` or trusted future
     * wrapper.
     * @post On successful acquisition, this handle points at `ctrl` and the
     * shared-borrow count is incremented in debug-check lanes.
     * @invariant The parameter carries `MEMSAFE_BORROWS(ctrl)` to connect the
     * returned borrow lifetime to the owner control source for backend lanes.
     * @throws `memsafe::violation` under THROW policy if a mutable borrow is
     * already live; installed handlers may throw.
     * @note Ownership/thread-safety: the constructed handle does not own the
     * control block.
     */
    explicit Ref(detail::borrow_ctrl<T>& ctrl MEMSAFE_BORROWS(ctrl))
        : ctrl_(detail::retain_shared(&ctrl)) {}

    /**
     * @brief Return the borrowed payload pointer or report a null borrow.
     *
     * @return Const pointer to the borrowed payload.
     * @pre The handle is non-empty and the owner control block is alive.
     * @post No borrow state is modified.
     * @invariant Debug-check null detection reports `violation_kind::null_access`;
     * release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the handle is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning.
     */
    const T* require_value() const {
        const T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty Ref");
        }
#endif
        return value;
    }

    /// Non-owning pointer to the owner control block that records this borrow.
    detail::borrow_ctrl<T>* ctrl_ = nullptr;
};

/**
 * @brief Exclusive mutable counted borrow implementation for `Owner<T>`.
 *
 * @tparam T Object type borrowed mutably.
 * @pre Construct through `Owner<T>::borrow_mut()` or a trusted friend so the
 * debug exclusive token is acquired consistently.
 * @post Destruction or reassignment releases the exclusive token when checks
 * are enabled.
 * @invariant `MutRef<T>` is non-copyable, movable, non-owning, type-level
 * nodiscard, and exposes the only counted mutable access path. Slice 5
 * `Mutex<T>` may attach a private friend-only release hook so the same public
 * `MutRef<T>` type can clear the borrow token and unlock a mutex together.
 * @throws Factory construction may report through the configured violation
 * policy; move and destruction operations are `noexcept`.
 * @note Ownership/thread-safety: single-threaded non-owning borrow handle. The
 * `MEMSAFE_BORROWS` annotations on construction and dereference carry the Slice
 * 7 co_await/lifetime contract.
 */
template <typename T>
class MEMSAFE_NODISCARD MutRef final {
public:
    /**
     * @brief Copy construction is disabled for exclusive borrows.
     *
     * @param other Source mutable borrow that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `MutRef<T>` is a compile-time error.
     * @post No second exclusive handle is created.
     * @invariant There can be at most one live mutable borrow per owner control
     * block in debug-check lanes.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies preserves exclusive
     * access semantics.
     */
    MutRef(const MutRef& other) = delete;

    /**
     * @brief Copy assignment is disabled for exclusive borrows.
     *
     * @param other Source mutable borrow that would otherwise be assigned.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No duplicate exclusive handle is created.
     * @invariant The mutable borrow token cannot be copied.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copy assignment preserves
     * exclusive access semantics.
     */
    MutRef& operator=(const MutRef& other) = delete;

    /**
     * @brief Move an exclusive mutable borrow without changing the borrow token.
     *
     * @param other Borrow handle whose exclusive token is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `MutRef<T>`.
     * @post This object owns `other`'s borrow record and any friend-provided
     * release hook; `other` is empty.
     * @invariant Moving does not create another exclusive token.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the borrow remains non-owning and tied to
     * the same single-threaded owner control block.
     */
    MutRef(MutRef&& other) noexcept
        : ctrl_(other.ctrl_),
          release_hook_(other.release_hook_),
          release_state_(other.release_state_) {
        other.ctrl_ = nullptr;
        other.release_hook_ = nullptr;
        other.release_state_ = nullptr;
    }

    /**
     * @brief Move-assign an exclusive mutable borrow.
     *
     * @param other Borrow handle whose exclusive token is transferred.
     * @return Reference to this borrow handle.
     * @pre `other` is a live `MutRef<T>`.
     * @post This object owns `other`'s previous borrow record and any
     * friend-provided release hook, the previously held borrow is released,
     * and `other` is empty.
     * @invariant Moving does not create another exclusive token.
     * @throws Nothing; this assignment operator is `noexcept`.
     * @note Ownership/thread-safety: assignment remains single-threaded and
     * non-owning.
     */
    MutRef& operator=(MutRef&& other) noexcept {
        if (this != &other) {
            release();
            ctrl_ = other.ctrl_;
            release_hook_ = other.release_hook_;
            release_state_ = other.release_state_;
            other.ctrl_ = nullptr;
            other.release_hook_ = nullptr;
            other.release_state_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Release this exclusive mutable borrow.
     *
     * @return No value; destructors release the receiving object.
     * @pre The owning `Owner<T>` control block is still alive when this handle
     * is non-empty.
     * @post The mutable borrow token is cleared in debug-check lanes. When a
     * trusted friend such as `Mutex<T>` installed a release hook, that hook has
     * also run before this object becomes empty.
     * @invariant Each successful mutable acquisition is released exactly once by
     * destruction or assignment.
     * @throws Nothing; this destructor is `noexcept`.
     * @note Ownership/thread-safety: destruction releases a non-owning,
     * single-threaded borrow record.
     */
    ~MutRef() noexcept {
        release();
    }

    /**
     * @brief Dereference the exclusive mutable borrow.
     *
     * @return Mutable reference to the borrowed payload.
     * @pre This handle is non-empty and its owning `Owner<T>` is still alive.
     * @post No borrow state is modified.
     * @invariant Mutable access is available only through a `MutRef<T>` that
     * successfully acquired the exclusive token in debug-check lanes.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the reference is non-owning and bounded by
     * both this `MutRef<T>` and the owning `Owner<T>`.
     */
    T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the exclusive mutable borrowed payload.
     *
     * @return Mutable pointer to the borrowed payload.
     * @pre This handle is non-empty and its owning `Owner<T>` is still alive.
     * @post No borrow state is modified.
     * @invariant Mutable access is available only through a `MutRef<T>` that
     * successfully acquired the exclusive token in debug-check lanes.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty borrow; installed handlers may throw. With release checks off,
     * no null check is emitted.
     * @note Ownership/thread-safety: the pointer is non-owning and bounded by
     * both this `MutRef<T>` and the owning `Owner<T>`.
     */
    T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

private:
    friend class Owner<T>;
    template <typename>
    friend class Mutex;

    /// Friend-only cleanup callback used by Slice 5 mutex integration.
    using release_hook_type =
        void (*)(detail::borrow_ctrl<T>* ctrl, void* state) noexcept;

    /**
     * @brief Construct a mutable borrow from an owner control block.
     *
     * @param ctrl Owner control block that supplies the payload and counter.
     * @return No value; constructors initialize the receiving object.
     * @pre `ctrl` is alive and belongs to a live `Owner<T>` or trusted future
     * wrapper.
     * @post On successful acquisition, this handle points at `ctrl` and the
     * mutable token is set in debug-check lanes.
     * @invariant The parameter carries `MEMSAFE_BORROWS(ctrl)` to connect the
     * returned borrow lifetime to the owner control source for backend lanes.
     * @throws `memsafe::violation` under THROW policy if any borrow is already
     * live; installed handlers may throw.
     * @note Ownership/thread-safety: the constructed handle does not own the
     * control block.
     */
    explicit MutRef(detail::borrow_ctrl<T>& ctrl MEMSAFE_BORROWS(ctrl))
        : ctrl_(detail::retain_mut(&ctrl)) {}

    /**
     * @brief Construct a mutable borrow with a friend-provided release hook.
     *
     * @param ctrl Owner-compatible control block that supplies the payload and
     * borrow counter.
     * @param release_hook Cleanup callback that releases the borrow token and
     * any enclosing resource, or null to use ordinary owner release.
     * @param release_state Opaque non-owning state passed to `release_hook`.
     * @return No value; constructors initialize the receiving object.
     * @pre `ctrl` and `release_state` are alive for the returned borrow
     * lifetime when `release_hook` is non-null. The caller has already acquired
     * any external resource that the hook will release.
     * @post On successful acquisition, this handle points at `ctrl` and owns
     * exactly one cleanup obligation. If a returning violation handler rejects
     * the borrow, the hook runs immediately and the constructed handle is empty.
     * @invariant The hook path is private to trusted friends so public
     * `MutRef<T>` remains the same exclusive mutable borrow type required by
     * F1 while Slice 5 `Mutex<T>` can bind unlock to destruction.
     * @throws `memsafe::violation` under THROW policy if the borrow token
     * cannot be acquired; the release hook is run before the exception is
     * rethrown. Installed handlers may throw.
     * @note Ownership/thread-safety: the hook is non-owning and must perform
     * only `noexcept` cleanup. `Mutex<T>` uses it while still holding the
     * standard mutex so the non-atomic Slice 1 borrow counter is not raced.
     */
    MutRef(detail::borrow_ctrl<T>& ctrl MEMSAFE_BORROWS(ctrl),
           release_hook_type release_hook,
           void* release_state)
        : ctrl_(nullptr),
          release_hook_(release_hook),
          release_state_(release_state) {
        try {
            ctrl_ = detail::retain_mut(&ctrl);
        } catch (...) {
            if (release_hook_ != nullptr) {
                release_hook_(&ctrl, release_state_);
                release_hook_ = nullptr;
                release_state_ = nullptr;
            }
            throw;
        }

        if (ctrl_ == nullptr && release_hook_ != nullptr) {
            release_hook_(&ctrl, release_state_);
            release_hook_ = nullptr;
            release_state_ = nullptr;
        }
    }

    /**
     * @brief Return the borrowed payload pointer or report a null borrow.
     *
     * @return Mutable pointer to the borrowed payload.
     * @pre The handle is non-empty and the owner control block is alive.
     * @post No borrow state is modified.
     * @invariant Debug-check null detection reports `violation_kind::null_access`;
     * release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the handle is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning.
     */
    T* require_value() const {
        T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty MutRef");
        }
#endif
        return value;
    }

    /**
     * @brief Release the current mutable borrow and any attached resource.
     *
     * @return Nothing.
     * @pre If `ctrl_` is non-null, it points at a live control block and this
     * object owns the corresponding mutable borrow token.
     * @post The borrow token is released, any friend release hook has run, and
     * this object is empty.
     * @invariant Cleanup is centralized so destruction and move assignment
     * cannot diverge for plain owner borrows versus mutex-backed borrows.
     * @throws Nothing; release hooks are required to be `noexcept`.
     * @note Ownership/thread-safety: ordinary owner borrows remain
     * single-threaded. Mutex-backed borrows release while their associated
     * mutex is still held.
     */
    void release() noexcept {
        if (ctrl_ != nullptr) {
            if (release_hook_ != nullptr) {
                release_hook_(ctrl_, release_state_);
            } else {
                detail::release_mut(ctrl_);
            }
            ctrl_ = nullptr;
        }
        release_hook_ = nullptr;
        release_state_ = nullptr;
    }

    /// Non-owning pointer to the owner control block that records this borrow.
    detail::borrow_ctrl<T>* ctrl_ = nullptr;
    /// Optional trusted-friend cleanup that replaces ordinary token release.
    release_hook_type release_hook_ = nullptr;
    /// Opaque non-owning state passed to `release_hook_`.
    void* release_state_ = nullptr;
};

/**
 * @brief Move-only RAII owner implementation for one heap-allocated `T`.
 *
 * @tparam T Object type owned by the wrapper.
 * @pre `T` is constructible from the constructor arguments and is not a
 * reference or array type.
 * @post Construction owns one `T`; destruction releases it; moves transfer the
 * control block and leave the source empty.
 * @invariant `Owner<T>` is non-copyable and unconditionally nothrow-movable so
 * standard containers reallocate by move.
 * @throws Construction may allocate or propagate `T` construction exceptions;
 * move construction and move assignment throw nothing.
 * @note Ownership/thread-safety: unique single-threaded owner. `borrow()` and
 * `borrow_mut()` return by-value nodiscard borrow handles annotated with
 * `MEMSAFE_BORROWS(*this)`.
 */
template <typename T>
class Owner final {
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
     * `Owner<T>`.
     * @throws `std::bad_alloc` or any exception propagated by `T` construction.
     * @note Ownership/thread-safety: the new owner has unique ownership and is
     * single-threaded.
     *
     * Example:
     * @code
     * memsafe::Owner<std::pair<int, int>> owner(1, 2);
     * int first = owner->first;
     * (void)first;
     * @endcode
     */
    template <typename... Args,
              typename Enable = typename std::enable_if<
                  std::is_constructible<T, Args&&...>::value>::type>
    explicit Owner(Args&&... args)
        : ctrl_(std::make_unique<detail::borrow_ctrl<T>>(std::forward<Args>(args)...)) {}

    /**
     * @brief Move ownership from another owner.
     *
     * @param other Source owner whose control block is transferred.
     * @return No value; constructors initialize the receiving object.
     * @pre `other` is a live `Owner<T>`.
     * @post This owner contains `other`'s former control block and `other` is
     * empty.
     * @invariant Moving transfers ownership of the control block without
     * relocating it, so existing borrow handles still point to the same control
     * address.
     * @throws Nothing; this constructor is unconditionally `noexcept(true)`.
     * @note Ownership/thread-safety: ownership moves to the receiving object;
     * borrows remain non-owning and single-threaded.
     */
    Owner(Owner&& other) noexcept(true) = default;

    /**
     * @brief Move-assign ownership from another owner.
     *
     * @param other Source owner whose control block is transferred.
     * @return Reference to this owner.
     * @pre `other` is a live `Owner<T>`. Any borrow handles into this owner's
     * current payload must have ended before assignment.
     * @post This owner contains `other`'s former control block and `other` is
     * empty.
     * @invariant Move assignment is unconditionally `noexcept(true)` so
     * `std::vector<Owner<T>>` can reallocate by moving rather than copying.
     * @throws Nothing; this assignment operator is unconditionally
     * `noexcept(true)`.
     * @note Ownership/thread-safety: ownership moves to the receiving object;
     * borrows remain non-owning and single-threaded.
     */
    Owner& operator=(Owner&& other) noexcept(true) = default;

    /**
     * @brief Copy construction is disabled for unique ownership.
     *
     * @param other Source owner that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying an `Owner<T>` is a compile-time error.
     * @post No duplicate owner is created for the same payload.
     * @invariant A payload has exactly one owning `Owner<T>`.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents double deletion
     * and split borrow counters.
     */
    Owner(const Owner& other) = delete;

    /**
     * @brief Copy assignment is disabled for unique ownership.
     *
     * @param other Source owner that would otherwise replace this owner.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No payload ownership is duplicated.
     * @invariant A payload remains owned by exactly one `Owner<T>`.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copy assignment preserves unique
     * ownership.
     */
    Owner& operator=(const Owner& other) = delete;

    /**
     * @brief Destroy the owned payload.
     *
     * @return No value; destructors release the receiving object.
     * @pre No `Ref<T>` or `MutRef<T>` into this owner may be used after
     * destruction begins.
     * @post The heap-allocated `T`, if present, has been destroyed exactly once.
     * @invariant The control block is released by the unique pointer.
     * @throws No exception may escape this destructor. If `T` violates the
     * ordinary C++ expectation that destructors do not throw during cleanup, the
     * runtime termination rules for destructors apply.
     * @note Ownership/thread-safety: destruction requires exclusive access to
     * the owner and does not synchronize with borrow handles.
     */
    ~Owner() = default;

    /**
     * @brief Return whether this owner currently contains a payload.
     *
     * @retval true This owner has a control block with a payload.
     * @retval false This owner has been moved from or otherwise has no payload.
     * @pre The owner object is alive.
     * @post No state is modified.
     * @invariant Moved-from owners report false.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: observer only; no synchronization is
     * performed.
     */
    bool has_value() const noexcept {
        return ctrl_ != nullptr && ctrl_->has_value();
    }

    /**
     * @brief Dereference the owned payload mutably.
     *
     * @return Mutable reference to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a `Ref<T>` or `MutRef<T>`;
     * callers should prefer explicit borrows when they need counted access.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned reference is tied to this owner
     * and requires single-threaded access.
     */
    T& operator*() MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Dereference the owned payload immutably.
     *
     * @return Const reference to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a `Ref<T>`; counted
     * immutable access is available through `borrow()`.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned reference is tied to this owner
     * and requires single-threaded access.
     */
    const T& operator*() const MEMSAFE_BORROWS(*this) {
        return *require_value();
    }

    /**
     * @brief Access a member of the owned payload mutably.
     *
     * @return Mutable pointer to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a borrow-handle object.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned pointer is tied to this owner and
     * requires single-threaded access.
     */
    T* operator->() MEMSAFE_BORROWS(*this) {
        return require_value();
    }

    /**
     * @brief Access a member of the owned payload immutably.
     *
     * @return Const pointer to the owned payload.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Direct owner access does not create a borrow-handle object.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty moved-from owner; installed handlers may throw. With release
     * checks off, no null check is emitted.
     * @note Ownership/thread-safety: returned pointer is tied to this owner and
     * requires single-threaded access.
     */
    const T* operator->() const MEMSAFE_BORROWS(*this) {
        return require_value();
    }

    /**
     * @brief Create an immutable counted borrow.
     *
     * @return `Ref<T>` by value so type-level `MEMSAFE_NODISCARD` diagnostics
     * fire when the borrow is discarded.
     * @pre `has_value()` is true and no mutable borrow is live in debug-check
     * lanes.
     * @post On success, the shared-borrow count is incremented until the
     * returned `Ref<T>` is destroyed, moved away, or assigned over.
     * @invariant Multiple immutable borrows may coexist, and the
     * `MEMSAFE_BORROWS(*this)` annotation ties the returned borrow to this
     * owner for backend lifetime analysis.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner or an active mutable borrow; installed handlers may throw.
     * @note Ownership/thread-safety: the returned borrow is non-owning and
     * single-threaded.
     */
    MEMSAFE_NODISCARD Ref<T> borrow() const MEMSAFE_BORROWS(*this) {
        return Ref<T>(require_control());
    }

    /**
     * @brief Create an exclusive mutable counted borrow.
     *
     * @return `MutRef<T>` by value so type-level `MEMSAFE_NODISCARD`
     * diagnostics fire when the borrow is discarded.
     * @pre `has_value()` is true and no immutable or mutable borrow is live in
     * debug-check lanes.
     * @post On success, the mutable borrow token is held until the returned
     * `MutRef<T>` is destroyed, moved away, or assigned over.
     * @invariant With debug checks enabled, requesting this borrow while any
     * borrow exists reports `violation_kind::borrow_exclusivity`; the
     * `MEMSAFE_BORROWS(*this)` annotation ties the returned borrow to this
     * owner for backend lifetime analysis.
     * @throws `memsafe::violation` under THROW policy when debug checks detect
     * an empty owner or any active borrow; installed handlers may throw. With
     * release checks off, the exclusivity branch is not compiled.
     * @note Ownership/thread-safety: the returned borrow is non-owning and
     * single-threaded.
     */
    MEMSAFE_NODISCARD MutRef<T> borrow_mut() MEMSAFE_BORROWS(*this) {
        return MutRef<T>(require_control());
    }

private:
    /**
     * @brief Return the control block or report a null owner.
     *
     * @return Reference to the owner's control block.
     * @pre `has_value()` is true.
     * @post No borrow counters are modified.
     * @invariant Debug-check null detection reports `violation_kind::null_access`;
     * release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned reference is internal and
     * non-owning.
     */
    detail::borrow_ctrl<T>& require_control() const {
#if MEMSAFE_RELEASE_CHECKS
        if (ctrl_ == nullptr || !ctrl_->has_value()) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot borrow from an empty Owner");
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
     * @invariant Debug-check null detection reports `violation_kind::null_access`;
     * release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning and tied to
     * this owner.
     */
    T* require_value() {
        T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty Owner");
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
     * @invariant Debug-check null detection reports `violation_kind::null_access`;
     * release no-check lanes omit the branch.
     * @throws `memsafe::violation` under THROW policy when the owner is empty;
     * installed handlers may throw.
     * @note Ownership/thread-safety: returned pointer is non-owning and tied to
     * this owner.
     */
    const T* require_value() const {
        const T* const value = ctrl_ != nullptr ? ctrl_->get() : nullptr;
#if MEMSAFE_RELEASE_CHECKS
        if (value == nullptr) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::null_access,
                "cannot dereference an empty Owner");
        }
#endif
        return value;
    }

    /// Unique owner of the payload control block and its debug borrow ledger.
    std::unique_ptr<detail::borrow_ctrl<T>> ctrl_;
};

} // namespace memsafe

#endif /* MEMSAFE_OWNER_HPP */
