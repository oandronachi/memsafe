/**
 * @file scope.hpp
 * @brief Scope-bound RAII arena for the memsafe Slice 3 public API.
 *
 * @details
 * Work package: CPP_MEMSAFE-0300-FUNC.
 *
 * Purpose:
 * - Define `memsafe::Scope`, the Slice 3 untyped RAII arena described by the
 *   architecture `Scope` data component and `Create_In_Scope` subprogram.
 * - Provide `Scope::create<T>(args...)`, which constructs an object owned by
 *   the scope and returns the unchecked raw `T*` required by the v1 contract.
 * - Destroy every successfully created object exactly once, in strict reverse
 *   creation order, when the `Scope` object itself is destroyed.
 *
 * Key invariants:
 * - Each successful `create<T>` call records exactly one private destruction
 *   entry before the raw pointer is returned to the caller.
 * - The destruction ledger is walked from newest entry to oldest entry on
 *   scope exit, so dependent objects see the same last-in, first-out lifetime
 *   shape as ordinary nested automatic variables.
 * - Raw pointers returned by `create<T>` are intentionally unchecked. Accessing
 *   one after the owning `Scope` has been destroyed is outside this library's
 *   violation policy in v1 and is observable only by ASan, HWASan, Valgrind, or
 *   similar external tooling.
 * - This header has no dependency on `violation.hpp` and never calls
 *   `MEMSAFE_DETAIL_VIOLATE` or any `MEMSAFE_ON_VIOLATION` path.
 *
 * Ownership and thread-safety:
 * - `Scope` uniquely owns the objects it creates. The caller receives raw,
 *   non-owning pointers whose lifetime is bounded by the `Scope`.
 * - A single `Scope` instance is not internally synchronized; concurrent calls
 *   to `create`, destruction, or `size` on the same instance require external
 *   synchronization. Distinct `Scope` instances are independent.
 *
 * @note This file replaces the Slice 0 stub from CPP_MEMSAFE-0030-FUNC with the
 * complete CPP_MEMSAFE-0300-FUNC implementation artifact.
 */
#ifndef MEMSAFE_SCOPE_HPP
#define MEMSAFE_SCOPE_HPP

#include <memsafe/config.hpp>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace memsafe {

/**
 * @brief Non-copyable RAII arena that owns objects until scope exit.
 *
 * @details
 * `Scope` is an untyped lifetime owner. `create<T>(args...)` allocates storage,
 * constructs a `T`, records the matching destructor and deallocator in a private
 * ledger, and returns a raw pointer for ordinary in-scope use. The raw pointer
 * is not a checked handle; stale access after `Scope` destruction is deliberately
 * left to sanitizer or memcheck lanes as required by the Slice 3 contract.
 *
 * Example:
 * @code
 * memsafe::Scope scope;
 *
 * auto* first = scope.create<std::string>("alpha");
 * auto* second = scope.create<int>(42);
 *
 * (void)first->size();
 * (void)*second;
 * // `second` is destroyed before `first` when `scope` leaves scope.
 * @endcode
 *
 * @pre Construct a `Scope` before using any pointer returned by its `create`
 * member. Do not use returned pointers after the `Scope` has been destroyed.
 * @post Destroying the `Scope` destroys all objects that were successfully
 * created through it and then releases their storage.
 * @invariant The private ledger contains one entry per live object owned by the
 * scope, ordered by successful creation time.
 * @throws `Scope` construction and destruction do not throw. `create<T>` may
 * throw allocation exceptions or exceptions propagated from `T` construction.
 * @note Ownership/thread-safety: `Scope` is the sole owner of created objects;
 * returned `T*` values are non-owning. A single `Scope` instance requires
 * external synchronization for concurrent access.
 */
class Scope final {
public:
    /**
     * @brief Construct an empty scope arena.
     *
     * @return No value; constructors initialize the receiving object.
     * @pre No precondition.
     * @post `size() == 0`.
     * @invariant The destruction ledger is empty immediately after
     * construction.
     * @throws Nothing.
     * @note Ownership/thread-safety: the new scope owns no objects yet and can
     * be handed to one thread or protected by external synchronization before
     * shared use.
     */
    Scope() noexcept = default;

    /**
     * @brief Destroy every object owned by this scope in reverse creation order.
     *
     * @return No value; destructors release the receiving object.
     * @pre Every type accepted by `create<T>` has a non-throwing destructor.
     * This is enforced with a compile-time check in `create<T>`.
     * @post All successfully created objects have had their destructor invoked
     * exactly once and their storage released. The `Scope` object and all raw
     * pointers previously returned by it are no longer valid for use.
     * @invariant Destruction walks the ledger from newest entry to oldest entry.
     * @throws Nothing. If a destructor incorrectly throws despite being
     * declared non-throwing, the C++ runtime terminates as required for a
     * throwing `noexcept` destructor.
     * @note Ownership/thread-safety: destruction requires exclusive access to
     * the `Scope`; concurrent use of returned raw pointers is the caller's
     * responsibility and is not checked by this v1 API.
     */
    ~Scope() noexcept {
        destroy_all();
    }

    /**
     * @brief Copy construction is disabled because a scope has unique ownership.
     *
     * @param other Source scope that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `Scope` is a compile-time error.
     * @post No second owner is created for the same arena entries.
     * @invariant Each created object has exactly one owning `Scope`.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: disabling copies prevents double
     * destruction of raw-pointer-backed arena entries.
     */
    Scope(const Scope& other) = delete;

    /**
     * @brief Copy assignment is disabled because a scope has unique ownership.
     *
     * @param other Source scope that would otherwise replace this scope.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy-assigning a `Scope` is a compile-time error.
     * @post No ownership transfer or duplication occurs.
     * @invariant Each created object remains owned by exactly one `Scope`.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: disabling copy assignment keeps the arena
     * lifetime bound to one concrete `Scope` object.
     */
    Scope& operator=(const Scope& other) = delete;

    /**
     * @brief Move construction is disabled to keep raw-pointer lifetimes tied
     * to the lexical `Scope` object that created them.
     *
     * @param other Source scope that would otherwise transfer ownership.
     * @return No value; this overload is deleted.
     * @pre Not available. Move-constructing a `Scope` is a compile-time error.
     * @post No ownership transfer occurs.
     * @invariant The scope that created an object remains the scope that
     * destroys it.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: the Slice 3 API returns unchecked raw
     * pointers, so keeping `Scope` non-movable avoids making pointer lifetime
     * depend on hidden ownership transfer.
     */
    Scope(Scope&& other) = delete;

    /**
     * @brief Move assignment is disabled to keep arena ownership stable.
     *
     * @param other Source scope that would otherwise transfer ownership.
     * @return No value; this overload is deleted.
     * @pre Not available. Move-assigning a `Scope` is a compile-time error.
     * @post No existing entries are replaced by moved entries.
     * @invariant The destruction ledger for a live scope is never transferred
     * to another `Scope` object.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: stable ownership makes the raw `T*`
     * lifetime model match the v1 sanitizer-only stale-pointer oracle.
     */
    Scope& operator=(Scope&& other) = delete;

    /**
     * @brief Construct a `T` owned by this scope and return its raw pointer.
     *
     * @tparam T Object type to construct. `T` must be a complete, non-array
     * object type with a non-throwing destructor.
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Arguments forwarded to `T`'s constructor.
     * @return Raw pointer to the newly constructed `T`. The pointer is
     * non-owning and remains valid only until this `Scope` is destroyed.
     * @pre `T` is constructible from `Args&&...`, is not an array, and has a
     * non-throwing destructor. The caller must not use the returned pointer
     * after this `Scope` has been destroyed.
     * @post On success, `size()` is increased by one and the object will be
     * destroyed exactly once during scope destruction, after all objects created
     * later by the same scope. On construction or ledger-allocation failure, no
     * new live object is retained by the scope.
     * @invariant A successful call appends exactly one ledger entry for the
     * returned object.
     * @throws `std::bad_alloc` from storage or ledger allocation, or any
     * exception thrown by `T`'s constructor. No memsafe violation path is used
     * for allocation or stale raw-pointer behavior.
     * @note Ownership/thread-safety: ownership stays with the `Scope`; the raw
     * pointer is an unchecked borrow-like convenience for in-scope use. A
     * single `Scope` must be externally synchronized when shared across
     * threads.
     *
     * Example:
     * @code
     * memsafe::Scope scope;
     * auto* value = scope.create<std::pair<int, int>>(1, 2);
     * int sum = value->first + value->second;
     * (void)sum;
     * @endcode
     */
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_object<T>::value,
                      "memsafe::Scope::create<T> requires an object type");
        static_assert(!std::is_array<T>::value,
                      "memsafe::Scope::create<T> does not construct arrays");
        static_assert(std::is_constructible<T, Args&&...>::value,
                      "memsafe::Scope::create<T> requires T(args...) to be constructible");
        static_assert(std::is_nothrow_destructible<T>::value,
                      "memsafe::Scope::create<T> requires a non-throwing destructor");

        void* const storage = allocate_for<T>();
        T* object = nullptr;

        try {
            object = ::new (storage) T(std::forward<Args>(args)...);
        } catch (...) {
            deallocate_for<T>(storage);
            throw;
        }

        try {
            entries_.push_back(entry{object, &destroy_for<T>, &deallocate_for<T>});
        } catch (...) {
            /*
             * If the private ledger cannot grow after `T` construction, unwind
             * the object immediately. This preserves the invariant that every
             * live created object is represented by exactly one ledger entry.
             */
            destroy_for<T>(object);
            deallocate_for<T>(object);
            throw;
        }

        /*
         * F1 Slice 3 and F3 require create() to return an unchecked raw T*.
         * Do not wrap this pointer or report stale uses through violation.hpp;
         * F2 makes post-scope access a sanitizer-only oracle.
         */
        return object;
    }

    /**
     * @brief Return the number of live objects currently owned by the scope.
     *
     * @return Count of successful `create<T>` calls whose objects are still
     * owned by this `Scope`.
     * @pre The `Scope` object is alive.
     * @post The scope and all owned objects are unchanged.
     * @invariant The returned value is the size of the private destruction
     * ledger.
     * @throws Nothing.
     * @note Ownership/thread-safety: this observer performs no synchronization;
     * concurrent mutation of the same `Scope` requires external locking.
     */
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

private:
    /**
     * @brief Private erased destructor callback type for one ledger entry.
     *
     * @param object Pointer to the live object represented by the entry.
     * @return No value.
     * @pre `object` points to a live object of the concrete type captured when
     * the callback was recorded.
     * @post The pointed-to object's destructor has run exactly once.
     * @invariant The callback never owns storage; it only ends the object's
     * lifetime before the paired deallocation callback releases storage.
     * @throws Nothing. `create<T>` accepts only non-throwing destructors.
     * @note Ownership/thread-safety: callbacks are private implementation
     * details of the owning `Scope` and run only while that scope has exclusive
     * destruction access.
     */
    using destroy_fn = void (*)(void*) noexcept;

    /**
     * @brief Private erased deallocation callback type for one ledger entry.
     *
     * @param storage Pointer to the storage allocated for the entry's object.
     * @return No value.
     * @pre `storage` was returned by the matching `allocate_for<T>` helper and
     * the object's lifetime has already ended.
     * @post The storage has been released with the same alignment-aware C++
     * deallocation form used for that concrete `T`.
     * @invariant The callback is paired with the concrete type's destructor
     * callback in the same ledger entry.
     * @throws Nothing.
     * @note Ownership/thread-safety: the owning `Scope` invokes this callback
     * during exclusive destruction or exception rollback.
     */
    using deallocate_fn = void (*)(void*) noexcept;

    /**
     * @brief Type-erased lifetime record for one object owned by a `Scope`.
     *
     * @details
     * Each record holds the object address plus the concrete callbacks needed
     * to end the object's lifetime and release its storage. This small ledger
     * entry is the reason `Scope` can be an untyped arena while still destroying
     * heterogeneous objects correctly.
     *
     * @pre `object`, `destroy`, and `deallocate` are all initialized before an
     * entry is appended to `entries_`.
     * @post Copying an entry copies only erased callbacks and the object
     * address; ownership remains with the containing `Scope`.
     * @invariant A live entry has exactly one object pointer, one destructor
     * callback, and one deallocation callback for the same concrete `T`.
     * @throws Nothing.
     * @note Ownership/thread-safety: entries are private to one `Scope` and are
     * not shared across threads without caller-provided synchronization of the
     * enclosing scope.
     */
    struct entry {
        /// Address of the live object owned by this ledger entry.
        void* object;
        /// Erased function that invokes the concrete object's destructor.
        destroy_fn destroy;
        /// Erased function that releases the concrete object's allocated storage.
        deallocate_fn deallocate;
    };

    /**
     * @brief Allocate correctly aligned raw storage for an object of type `T`.
     *
     * @tparam T Concrete object type whose size and alignment determine the
     * allocation request.
     * @return Pointer to uninitialized storage suitable for exactly one `T`.
     * @pre `T` is a complete object type; `create<T>` enforces that before this
     * helper is instantiated.
     * @post Storage for one `T` has been allocated, but no `T` lifetime has
     * begun yet.
     * @invariant The returned pointer must be released by
     * `deallocate_for<T>`.
     * @throws `std::bad_alloc` or another exception from the global aligned
     * allocation function if storage cannot be obtained.
     * @note Ownership/thread-safety: the caller owns the raw storage until it is
     * either recorded in `entries_` or released during exception rollback.
     */
    template <class T>
    static void* allocate_for() {
        return ::operator new(sizeof(T), std::align_val_t(alignof(T)));
    }

    /**
     * @brief Release storage allocated for a concrete object type `T`.
     *
     * @tparam T Concrete object type whose alignment selects the matching
     * deallocation function.
     * @param storage Pointer returned by `allocate_for<T>`.
     * @return No value.
     * @pre `storage` is non-null storage allocated by `allocate_for<T>`, and
     * any `T` object formerly living there has already been destroyed.
     * @post The storage is released exactly once.
     * @invariant The aligned deallocation form mirrors the allocation form in
     * `allocate_for<T>`.
     * @throws Nothing.
     * @note Ownership/thread-safety: this helper is called only by the owning
     * `Scope` during exception rollback or exclusive destruction.
     */
    template <class T>
    static void deallocate_for(void* storage) noexcept {
        ::operator delete(storage, std::align_val_t(alignof(T)));
    }

    /**
     * @brief Invoke the destructor for the concrete object type `T`.
     *
     * @tparam T Concrete type recorded when the object was created.
     * @param object Pointer to a live `T` object.
     * @return No value.
     * @pre `object` points to a live `T` created by placement new in this
     * scope's storage.
     * @post The `T` lifetime has ended exactly once; storage remains allocated
     * until `deallocate_for<T>` runs.
     * @invariant Destruction and deallocation stay separate so exception
     * rollback and reverse-order scope cleanup can preserve the ledger
     * invariant exactly.
     * @throws Nothing. `create<T>` statically requires `T` to be nothrow
     * destructible.
     * @note Ownership/thread-safety: this helper is private to the owning
     * `Scope` and assumes exclusive access to the object lifetime.
     */
    template <class T>
    static void destroy_for(void* object) noexcept {
        static_cast<T*>(object)->~T();
    }

    /**
     * @brief Destroy and deallocate all ledger entries in reverse creation order.
     *
     * @return No value.
     * @pre The `Scope` destructor has exclusive access to this scope, and every
     * ledger entry is complete and represents a live object.
     * @post Every recorded object has been destroyed and deallocated exactly
     * once, and `entries_` is empty before the vector's own destructor runs.
     * @invariant Traversal is last-in, first-out to satisfy F1 Slice 3 and the
     * CPP_MEMSAFE-0300-FUNC acceptance criterion for reverse destruction order.
     * @throws Nothing. Ledger callbacks are `noexcept`.
     * @note Ownership/thread-safety: this function is the single normal cleanup
     * path for scope-owned objects and must not report stale raw-pointer use
     * through the violation subsystem; F2 leaves that as a sanitizer-only
     * oracle.
     */
    void destroy_all() noexcept {
        /*
         * The architecture and work-package acceptance criteria require reverse
         * destruction order. Indexing from size() down to one avoids unsigned
         * underflow while preserving exact last-in, first-out traversal.
         */
        for (std::size_t index = entries_.size(); index != 0U; --index) {
            const entry current = entries_[index - 1U];
            current.destroy(current.object);
            current.deallocate(current.object);
        }

        entries_.clear();
    }

    /// Private LIFO ledger containing one complete lifetime record per live object.
    std::vector<entry> entries_;
};

} // namespace memsafe

#endif /* MEMSAFE_SCOPE_HPP */
