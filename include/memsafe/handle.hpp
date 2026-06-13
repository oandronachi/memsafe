/**
 * @file handle.hpp
 * @brief Slice 2 slot-generation, handle, and bounded slot-map primitives.
 *
 * @details
 * Work packages: CPP_MEMSAFE-0200-FUNC baseline plus
 * CPP_MEMSAFE-0205-FUNC SlotMap allocator additions.
 *
 * Purpose:
 * - Define `memsafe::Slot<T>` as payload storage plus a per-instance
 *   `std::atomic<std::uint32_t>` generation counter.
 * - Define the atomic slot generation load and bump primitives consumed by the
 *   owning `SlotMap<T>` implementation when allocating and freeing slots.
 * - Define `memsafe::Handle<T>` as a type-level `MEMSAFE_NODISCARD` value that
 *   carries only a slot index and the slot generation remembered at allocation
 *   time.
 * - Define `memsafe::SlotMap<T>` as a fixed-capacity, header-only allocator
 *   whose allocation and deallocation paths pop and push slot indices through a
 *   Treiber-stack free list with one packed head-index plus ABA counter.
 *
 * Key invariants:
 * - Slot generations start at a nonzero value; generation zero is reserved for
 *   invalid/default handles.
 * - `Slot<T>::bump_generation()` updates the generation with an atomic
 *   read-modify-write loop and never publishes the reserved zero generation.
 * - A default `Handle<T>` is intrinsically invalid and cannot compare equal to a
 *   handle for any real allocated slot, because real slots use a non-sentinel
 *   index and a nonzero generation.
 * - `Handle<T>` stores no pointer, owner, allocator, freelist node, capacity, or
 *   synchronization state. It is a trivially copyable two-word value.
 * - `SlotMap<T>` introduces no global mutex and no global generation counter:
 *   every generation comparison and bump is performed against the selected
 *   slot's own `std::atomic<std::uint32_t>`.
 * - `SlotMap<T>` free-list publication uses one packed
 *   `std::atomic<std::uint64_t>` head. The low 32 bits carry the head index and
 *   the high 32 bits carry an ABA counter incremented by the shared CAS helper
 *   used by both pop and push.
 * - Exhausted capacity reports `violation_kind::capacity_exhausted`. Stale
 *   handle access reports `violation_kind::use_after_free` through the active
 *   `MEMSAFE_ON_VIOLATION` policy.
 *
 * Ownership and thread-safety:
 * - `Slot<T>` owns only inline storage for one potential `T` object and its
 *   atomic generation counter. It never allocates dynamic storage and never
 *   records whether the payload is live; the owning `SlotMap<T>` manages object
 *   lifetime and free-list membership.
 * - `Handle<T>` owns no storage outside its two scalar fields and is safe to
 *   copy between threads as an ordinary value. It does not keep the pointed-to
 *   object alive.
 * - `SlotMap<T>` owns the inline slot array and constructs payload objects in
 *   place. Allocation/deallocation of slot indices is lock-free; concurrent
 *   access to the contained `T` objects remains the caller's responsibility.
 *
 * Traceability:
 * - F1 Slice 2, "Handle and Slot Types": `Slot<T>` and `Handle<T>`.
 * - F2 Generational Handles cases 1 through 4: allocation, deallocation, reuse,
 *   bounded capacity, stale-handle detection, and lock-free contention are
 *   enforced by the owning `SlotMap<T>` context.
 * - F3 Slice 2 data `Slot`/`Handle`/`SlotMap`: `Atomic_Generation`,
 *   `Generation_Checked`, `Bounded_Capacity`, `Treiber_Stack`, and
 *   `Slot_Capacity`.
 */
#ifndef MEMSAFE_HANDLE_HPP
#define MEMSAFE_HANDLE_HPP

#include <memsafe/backend.hpp>
#include <memsafe/config.hpp>
#include <memsafe/violation.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace memsafe {

namespace detail {

static constexpr std::size_t slotmap_default_capacity = 64U;

} // namespace detail

/**
 * @brief Fixed-capacity slot-map allocator declared before `Slot<T>` for
 * friendship with slot generation internals.
 *
 * @tparam T Object type stored in slots owned by the map.
 * @tparam Capacity Compile-time storage bound. The default is 64, matching the
 * F3 `Slot_Capacity` model value.
 * @pre `T` must satisfy the object-type requirements documented on the full
 * `SlotMap<T, Capacity>` definition below.
 * @post This declaration introduces no storage until the full class template is
 * instantiated.
 * @invariant The default template argument keeps `SlotMap<T>` spelling
 * available while allowing tests to instantiate smaller bounded maps.
 * @throws Nothing; this is a declaration only.
 * @note Ownership/thread-safety: no runtime ownership or synchronization is
 * introduced by the forward declaration.
 */
template <typename T, std::size_t Capacity = detail::slotmap_default_capacity>
class SlotMap;

/**
 * @brief Inline slot storage with a per-slot atomic generation counter.
 *
 * @tparam T Object type stored in this slot. `T` must be a complete object type
 * when `Slot<T>` is instantiated; reference, function, `void`, and array types
 * are rejected because a slot owns exactly one object-sized storage region.
 *
 * @pre The owning container constructs at most one live `T` object in the slot
 * at a time by calling `construct()`.
 * @post A newly constructed slot has generation `initial_generation`, which is
 * nonzero.
 * @invariant The generation counter is per-instance and atomic; no global
 * generation counter or shared mutex is introduced by this type.
 * @throws The default constructor and generation operations do not throw.
 * `construct()` may throw any exception thrown by `T`'s constructor; `destroy()`
 * may throw only if `T`'s destructor is throwing.
 * @note Ownership/thread-safety: the generation counter can be read and bumped
 * concurrently through its atomic operations. Payload construction, destruction,
 * and pointer access require external synchronization and are owned by
 * `SlotMap<T>`, not by this storage primitive.
 *
 * Example:
 * @code
 * memsafe::Slot<int> slot;
 * int* value = slot.construct(7);
 * const auto issued_generation = slot.generation();
 * slot.destroy();
 * const auto next_generation = slot.bump_generation();
 * (void)value;
 * (void)issued_generation;
 * (void)next_generation;
 * @endcode
 */
template <typename T>
class Slot {
    static_assert(std::is_object<T>::value,
                  "Slot<T> requires a complete non-array object type");
    static_assert(!std::is_array<T>::value,
                  "Slot<T> stores exactly one object and does not support arrays");

public:
    /**
     * @brief Unsigned generation value stored atomically in each slot.
     *
     * @return Type alias for the per-slot generation counter representation.
     * @pre Use this alias for slot and handle generation values only; zero is
     * reserved by this header.
     * @post No runtime state is modified.
     * @invariant The alias is `std::uint32_t` in v1 to match the F3
     * `Atomic_Generation` data contract.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage and introduce no
     * synchronization.
     */
    using generation_type = std::uint32_t;

    /**
     * @brief Reserved generation value used only by invalid/default handles.
     *
     * @pre Do not publish this value as the stable generation of a live slot;
     * F1 Slice 2 reserves generation zero for invalid handles.
     * @post No runtime state is modified; this constant only names the
     * reserved sentinel.
     * @invariant The sentinel remains zero so a default `Handle<T>` cannot
     * match any slot generation produced by `Slot<T>`.
     * @throws Nothing; this is a compile-time constant.
     * @note Ownership/thread-safety: the constant owns no storage and has no
     * synchronization behavior. `bump_generation()` explicitly skips this
     * value on wraparound so generation-checked lookup never treats the
     * default handle generation as live.
     */
    static constexpr generation_type invalid_generation = 0u;

    /**
     * @brief First generation assigned to a newly constructed slot.
     *
     * @pre Slot construction uses this value before any payload lifetime
     * begins.
     * @post A default-constructed `Slot<T>` reports this nonzero generation
     * until `bump_generation()` successfully advances it.
     * @invariant The value differs from `invalid_generation`, satisfying the
     * CPP_MEMSAFE-0200-FUNC requirement that live slot generations start at a
     * nonzero value.
     * @throws Nothing; this is a compile-time constant.
     * @note Ownership/thread-safety: the constant is shared immutable metadata.
     * It does not allocate, track liveness, or participate in the 0205
     * allocator/free-list responsibilities.
     */
    static constexpr generation_type initial_generation = 1u;

    /**
     * @brief Construct an empty slot with a nonzero initial generation.
     *
     * @return A slot containing no live `T` object and generation
     * `initial_generation`.
     * @pre `T` satisfies the `Slot<T>` type requirements.
     * @post `generation() == initial_generation` and no payload lifetime has
     * begun.
     * @invariant Construction does not allocate and does not register with any
     * allocator or free list.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the constructed slot owns raw inline
     * storage and an atomic counter. No shared state outside the slot is touched.
     */
    Slot() noexcept : generation_(initial_generation) {}

    /**
     * @brief Disable copying because a slot owns unique raw payload storage.
     *
     * @param other Source slot; unused because copying is intentionally
     * ill-formed.
     * @return No value; the function is deleted.
     * @pre Do not copy slots. Move or copy `Handle<T>` values instead.
     * @post Any attempted copy construction is rejected at compile time.
     * @invariant The atomic generation and possible live payload cannot be
     * duplicated into a second owner.
     * @throws Not applicable; the function is deleted.
     * @note Ownership/thread-safety: deleting copies prevents two slot objects
     * from believing they own the same payload lifetime.
     */
    Slot(const Slot& other) = delete;

    /**
     * @brief Disable copy assignment because a slot owns unique raw storage.
     *
     * @param other Source slot; unused because assignment is intentionally
     * ill-formed.
     * @return No value; the function is deleted.
     * @pre Do not assign slots. `SlotMap<T>` code must manage slot arrays
     * in place.
     * @post Any attempted copy assignment is rejected at compile time.
     * @invariant The per-slot generation remains tied to exactly one storage
     * region.
     * @throws Not applicable; the function is deleted.
     * @note Ownership/thread-safety: deleting assignment avoids racing or
     * duplicating live payload ownership.
     */
    Slot& operator=(const Slot& other) = delete;

    /**
     * @brief Disable moving because atomic generation state is address-stable.
     *
     * @param other Source slot; unused because moving is intentionally
     * ill-formed.
     * @return No value; the function is deleted.
     * @pre Do not move slots after an owning slot-map array has been created.
     * @post Any attempted move construction is rejected at compile time.
     * @invariant Handles remember slot indices into an owning array; moving
     * slots independently would obscure that storage identity.
     * @throws Not applicable; the function is deleted.
     * @note Ownership/thread-safety: deleting moves avoids transferring a raw
     * payload lifetime without the owning container's coordination.
     */
    Slot(Slot&& other) = delete;

    /**
     * @brief Disable move assignment for the same reason move construction is
     * disabled.
     *
     * @param other Source slot; unused because assignment is intentionally
     * ill-formed.
     * @return No value; the function is deleted.
     * @pre Do not move-assign slots.
     * @post Any attempted move assignment is rejected at compile time.
     * @invariant The storage region, generation counter, and owning slot-map
     * index remain stable for the lifetime of the slot object.
     * @throws Not applicable; the function is deleted.
     * @note Ownership/thread-safety: deleting move assignment avoids replacing a
     * slot while other threads may hold handles naming its index.
     */
    Slot& operator=(Slot&& other) = delete;

    /**
     * @brief Destroy the slot wrapper.
     *
     * @return No value; destructors release the receiving object.
     * @pre No live `T` object remains in the raw storage. The owning
     * `SlotMap<T>` must call `destroy()` for any live payload before the slot
     * wrapper is destroyed.
     * @post The atomic generation counter and raw storage cease to exist.
     * @invariant The destructor performs no payload destruction because this
     * work package deliberately avoids owning allocation/liveness state.
     * @throws Nothing directly.
     * @note Ownership/thread-safety: slot teardown must be externally
     * synchronized with any payload access, exactly as owning `SlotMap<T>` array
     * teardown will require.
     */
    ~Slot() = default;

    /**
     * @brief Load the current generation with acquire ordering.
     *
     * @return The current nonzero generation value for this slot.
     * @pre The slot object is alive.
     * @post No state is modified.
     * @invariant The returned value is loaded from the per-instance atomic
     * counter; no global generation or shared mutex participates.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: concurrent calls are atomic. The acquire
     * load pairs with `bump_generation()`'s release update so
     * generation-checked `SlotMap<T>` lookup can observe generation changes
     * consistently.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * const auto generation = slot.generation();
     * @endcode
     */
    generation_type generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    /**
     * @brief Atomically advance the slot generation and return the new value.
     *
     * @return The next nonzero generation after the successful atomic update.
     * @pre The slot object is alive. Callers should invoke this after ending the
     * payload lifetime for a freed slot and before reissuing a handle for that
     * slot.
     * @post Future `generation()` calls observe either the old value or the new
     * nonzero value according to normal atomic visibility rules; zero is never
     * published as the stable generation.
     * @invariant The update is per-slot and atomic, satisfying F3
     * `Atomic_Generation`; generation zero remains reserved so a default handle
     * cannot become valid after wraparound.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this function synchronizes only the
     * generation counter. Payload lifetime and free-list publication remain the
     * responsibility of CPP_MEMSAFE-0205-FUNC.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * const auto old_generation = slot.generation();
     * const auto new_generation = slot.bump_generation();
     * (void)(new_generation != old_generation);
     * @endcode
     */
    generation_type bump_generation() noexcept {
        generation_type observed = generation_.load(std::memory_order_acquire);
        for (;;) {
            const generation_type desired = next_generation_after(observed);

            if (generation_.compare_exchange_weak(observed,
                                                  desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return desired;
            }
        }
    }

    /**
     * @brief Construct a payload object in this slot's inline storage.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Constructor arguments forwarded exactly once to `T`.
     * @return Pointer to the newly constructed payload object.
     * @pre No live `T` object currently occupies this slot's storage.
     * @post A `T` object is live at `value_ptr()`. The generation is unchanged;
     * allocation code remembers the current generation when it mints a handle.
     * @invariant This helper begins an object lifetime only; it performs no
     * dynamic allocation, free-list mutation, capacity accounting, or generation
     * check.
     * @throws Propagates any exception thrown by `T`'s constructor.
     * @note Ownership/thread-safety: the slot owns the inline storage, while the
     * caller owns the decision that the storage is available. Concurrent
     * construction or access requires external synchronization.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * int* value = slot.construct(42);
     * slot.destroy();
     * (void)value;
     * @endcode
     */
    template <typename... Args>
    T* construct(Args&&... args) noexcept(
        std::is_nothrow_constructible<T, Args&&...>::value) {
        /*
         * CPP_MEMSAFE-0200-FUNC owns storage primitives only. Placement new is
         * used here so this header supplies payload storage without adding any
         * allocator or capacity policy, which is deferred to 0205.
         */
        return ::new (storage_address()) T(std::forward<Args>(args)...);
    }

    /**
     * @brief End the lifetime of the payload object currently in this slot.
     *
     * @return Nothing.
     * @pre A live `T` object occupies this slot's storage.
     * @post The payload lifetime has ended. The generation is unchanged until
     * the owning container calls `bump_generation()`.
     * @invariant Destroying the payload and bumping the generation are separate
     * operations so the CPP_MEMSAFE-0205-FUNC `SlotMap<T>` deallocation path can
     * order destruction, generation invalidation, and free-list publication
     * explicitly.
     * @throws Propagates any exception thrown by `T`'s destructor when it is not
     * `noexcept`.
     * @note Ownership/thread-safety: the caller must ensure no concurrent
     * payload access is active when the lifetime ends.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * slot.construct(42);
     * slot.destroy();
     * slot.bump_generation();
     * @endcode
     */
    void destroy() noexcept(std::is_nothrow_destructible<T>::value) {
        value_ptr()->~T();
    }

    /**
     * @brief Return a pointer to the payload storage interpreted as `T`.
     *
     * @return Pointer to the live payload object.
     * @pre A live `T` object has been constructed in this slot.
     * @post No state is modified.
     * @invariant Pointer access performs no generation check; checked lookup is
     * owned by CPP_MEMSAFE-0205-FUNC where a `Handle<T>` and owning slot-map
     * context are both available.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned pointer is borrowed and does
     * not extend the payload lifetime. Concurrent access requires external
     * synchronization by the owning container.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * slot.construct(1);
     * *slot.value_ptr() = 2;
     * slot.destroy();
     * @endcode
     */
    T* value_ptr() noexcept {
        return std::launder(static_cast<T*>(storage_address()));
    }

    /**
     * @brief Return a const pointer to the payload storage interpreted as `T`.
     *
     * @return Const pointer to the live payload object.
     * @pre A live `T` object has been constructed in this slot.
     * @post No state is modified.
     * @invariant Pointer access performs no generation check; checked lookup is
     * owned by CPP_MEMSAFE-0205-FUNC.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned pointer is borrowed and does
     * not extend the payload lifetime. Concurrent access requires external
     * synchronization by the owning container.
     *
     * Example:
     * @code
     * memsafe::Slot<int> slot;
     * slot.construct(5);
     * const memsafe::Slot<int>& slot_ref = slot;
     * const int* value = slot_ref.value_ptr();
     * (void)value;
     * slot.destroy();
     * @endcode
     */
    const T* value_ptr() const noexcept {
        return std::launder(static_cast<const T*>(storage_address()));
    }

private:
    template <typename, std::size_t>
    friend class SlotMap;

    /**
     * @brief Compute the next publishable nonzero generation value.
     *
     * @param current Current slot generation observed by the caller.
     * @return The generation immediately after `current`, except that wrap to
     * `invalid_generation` is skipped and returns `initial_generation`.
     * @pre `current` is a value read from this slot's per-slot generation
     * counter, or a trusted test value used to exercise wraparound behavior.
     * @post No state is modified; the returned value is safe to publish as a
     * non-default slot generation.
     * @invariant F1 Handle and Slot Types reserve generation zero for invalid
     * handles, so this helper never returns `invalid_generation`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the helper is pure arithmetic and owns no
     * storage. Atomic publication is performed by `bump_generation()` or
     * `try_bump_generation_from()`, not by this helper.
     *
     * Example:
     * @code
     * auto next = memsafe::Slot<int>::initial_generation + 1u;
     * (void)next;
     * @endcode
     */
    static generation_type next_generation_after(generation_type current) noexcept {
        generation_type desired =
            static_cast<generation_type>(current + generation_type{1});
        if (desired == invalid_generation) {
            /*
             * F1 Handle and Slot Types reserve generation zero for default
             * handles. Wrapping directly to the initial nonzero generation
             * prevents a live or freshly freed slot from publishing the default
             * handle generation.
             */
            desired = initial_generation;
        }
        return desired;
    }

    /**
     * @brief Advance this slot's generation only if it still matches a handle.
     *
     * @param expected Generation remembered by the handle being deallocated.
     * @param updated Output reference receiving the newly published generation
     * on success or the currently observed generation on failure.
     * @retval true The per-slot generation equaled `expected` and was advanced
     * atomically to the next nonzero generation.
     * @retval false `expected` was invalid or another operation had already
     * advanced the generation; `updated` contains the observed generation.
     * @pre The caller owns the deallocation decision for this slot and passes
     * the handle generation it is validating.
     * @post On success, stale handles carrying `expected` are rejected by later
     * generation-checked lookup. On failure, no generation is changed here.
     * @invariant The compare-and-swap operates only on this slot's atomic
     * generation counter; CPP_MEMSAFE-0205-FUNC forbids a global generation
     * counter or global mutex for deallocation validation.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this helper retires the generation, but it
     * does not destroy the payload or publish the slot to the free list. The
     * owning `SlotMap<T>` sequences those actions around this per-slot CAS.
     *
     * Example:
     * @code
     * std::uint32_t observed = 0;
     * // SlotMap<T>::deallocate() calls this after validating the handle index.
     * (void)observed;
     * @endcode
     */
    bool try_bump_generation_from(generation_type expected,
                                  generation_type& updated) noexcept {
        if (expected == invalid_generation) {
            updated = generation_.load(std::memory_order_acquire);
            return false;
        }

        generation_type observed = expected;
        const generation_type desired = next_generation_after(expected);
        if (generation_.compare_exchange_strong(observed,
                                                desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            updated = desired;
            return true;
        }

        updated = observed;
        return false;
    }

    /**
     * @brief Return the mutable address of the aligned raw payload storage.
     *
     * @return Pointer to the first byte of this slot's storage buffer.
     * @pre The caller is a trusted slot member or owning `SlotMap<T>` path that
     * will only interpret the storage as `T` while a `T` lifetime is active.
     * @post No state is modified.
     * @invariant The returned address is the only storage address used for
     * placement construction, destruction, and `std::launder`-based access.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is borrowed raw storage owned
     * by this slot. Concurrent lifetime changes require synchronization by the
     * owning container.
     */
    void* storage_address() noexcept {
        return static_cast<void*>(&storage_[0]);
    }

    /**
     * @brief Return the const address of the aligned raw payload storage.
     *
     * @return Const pointer to the first byte of this slot's storage buffer.
     * @pre The caller is a trusted slot member or owning `SlotMap<T>` path that
     * will only interpret the storage as `T` while a `T` lifetime is active.
     * @post No state is modified.
     * @invariant The address equals the mutable `storage_address()` result for
     * the same slot object.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the pointer is borrowed raw storage owned
     * by this slot. Const access does not extend payload lifetime or add
     * synchronization.
     */
    const void* storage_address() const noexcept {
        return static_cast<const void*>(&storage_[0]);
    }

    /// Per-slot generation counter used by `SlotMap<T>` handle validation.
    std::atomic<generation_type> generation_;
    /// Inline storage for at most one live `T` payload.
    alignas(T) unsigned char storage_[sizeof(T)];
};

/**
 * @brief Non-stateful generational handle value for a `SlotMap<T>` slot.
 *
 * @tparam T Phantom element type associated with the owning
 * `SlotMap<T>`. The type parameter prevents accidental comparison or lookup
 * across different element types but is not stored in the handle object.
 *
 * @pre Construct handles directly only for tests or trusted infrastructure.
 * Production handles are minted by `SlotMap<T>::allocate()` after it
 * selects an index and reads the slot generation.
 * @post A default-constructed handle is invalid. A component-constructed handle
 * records exactly the supplied index and generation.
 * @invariant The object contains only `index_type index_` and
 * `generation_type generation_`; it has no pointer, allocator, ownership, or
 * synchronization state.
 * @throws Construction, copying, comparison, and observers do not throw.
 * @note Ownership/thread-safety: handles are ordinary trivially copyable values.
 * Copying a handle does not keep any slot alive and does not synchronize with
 * the owning slot map.
 *
 * Example:
 * @code
 * memsafe::Handle<int> empty;
 * memsafe::Handle<int> issued(0u, memsafe::Slot<int>::initial_generation);
 * const bool usable_without_map = issued.is_valid();
 * (void)empty;
 * (void)usable_without_map;
 * @endcode
 */
template <typename T>
class MEMSAFE_NODISCARD Handle final {
public:
    /**
     * @brief Unsigned index type naming a slot in an owning slot map.
     *
     * @return Type alias for slot indices stored by handles.
     * @pre Use `invalid_index` only as the default-handle sentinel; real
     * `SlotMap<T>` capacities must not allocate that index.
     * @post No runtime state is modified.
     * @invariant The alias is `std::uint32_t` in v1 so the handle remains a
     * compact two-word value.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage and introduce no
     * synchronization.
     */
    using index_type = std::uint32_t;

    /**
     * @brief Unsigned generation type remembered from a slot.
     *
     * @return Type alias for the generation component stored by handles.
     * @pre Zero is reserved for invalid/default handles.
     * @post No runtime state is modified.
     * @invariant The alias matches `Slot<T>::generation_type` without requiring
     * `Slot<T>` to be instantiated for incomplete `T` handle declarations.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage and introduce no
     * synchronization.
     */
    using generation_type = std::uint32_t;

    /**
     * @brief Sentinel slot index stored by default-invalid handles.
     *
     * @pre `SlotMap<T>` allocation code must not issue this index for a
     * real slot.
     * @post No runtime state is modified; default construction copies this
     * value into the handle's index component.
     * @invariant The sentinel is the maximum `index_type` value, leaving normal
     * zero-based slot indices available while ensuring a default handle cannot
     * compare equal to an allocated-style handle.
     * @throws Nothing; this is a compile-time constant.
     * @note Ownership/thread-safety: the constant owns no storage and performs
     * no synchronization. Capacity and free-list management that avoid this
     * index belong to CPP_MEMSAFE-0205-FUNC.
     */
    static constexpr index_type invalid_index =
        (std::numeric_limits<index_type>::max)();

    /**
     * @brief Sentinel remembered generation stored by default-invalid handles.
     *
     * @pre Use this value only for invalid/default handles or explicit
     * non-stateful tests of invalid component combinations.
     * @post No runtime state is modified; default construction copies this
     * value into the handle's remembered-generation component.
     * @invariant The sentinel is zero and therefore differs from
     * `Slot<T>::initial_generation` and every generation published by
     * `Slot<T>::bump_generation()`.
     * @throws Nothing; this is a compile-time constant.
     * @note Ownership/thread-safety: the constant is immutable metadata. It
     * does not prove or disprove slot-map membership; generation-checked lookup
     * is deferred to the owning `SlotMap<T>` context in CPP_MEMSAFE-0205-FUNC.
     */
    static constexpr generation_type invalid_generation = 0u;

    /**
     * @brief Construct the never-valid default handle.
     *
     * @return A handle with `index() == invalid_index` and
     * `generation() == invalid_generation`.
     * @pre None.
     * @post `is_valid()` returns false.
     * @invariant The default handle cannot equal a real allocated slot handle
     * because real slot handles use non-sentinel indices and nonzero
     * generations.
     * @throws Nothing; this constructor is `constexpr noexcept`.
     * @note Ownership/thread-safety: the handle owns only two scalar fields and
     * does not touch shared state.
     *
     * Example:
     * @code
     * static_assert(!memsafe::Handle<int>{}.is_valid());
     * @endcode
     */
    constexpr Handle() noexcept = default;

    /**
     * @brief Construct a handle from an index and remembered generation.
     *
     * @param index Slot index selected by the owning slot map.
     * @param generation Slot generation observed when the handle was issued.
     * @return A handle storing exactly `index` and `generation`.
     * @pre Trusted callers should pass a non-sentinel index and nonzero
     * generation when representing an allocated slot. Passing a sentinel
     * component is permitted for non-stateful property tests and creates an
     * intrinsically invalid handle.
     * @post `index()` returns `index` and `generation()` returns `generation`.
     * @invariant Construction stores no owner pointer; full generation-checked
     * validity requires the owning `SlotMap<T>` context.
     * @throws Nothing; this constructor is `constexpr noexcept`.
     * @note Ownership/thread-safety: constructed handles are independent values.
     *
     * Example:
     * @code
     * memsafe::Handle<int> h(3u, memsafe::Slot<int>::initial_generation);
     * const bool intrinsically_valid = h.is_valid();
     * @endcode
     */
    constexpr Handle(index_type index, generation_type generation) noexcept
        : index_(index), generation_(generation) {}

    /**
     * @brief Return the remembered slot index.
     *
     * @return The index component stored in this handle.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant The value is a plain scalar copied from construction.
     * @throws Nothing; this function is `constexpr noexcept`.
     * @note Ownership/thread-safety: reading the scalar field does not
     * synchronize with any slot map.
     */
    MEMSAFE_NODISCARD constexpr index_type index() const noexcept {
        return index_;
    }

    /**
     * @brief Return the remembered slot generation.
     *
     * @return The generation component stored in this handle.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant The value is a plain scalar copied from construction.
     * @throws Nothing; this function is `constexpr noexcept`.
     * @note Ownership/thread-safety: reading the scalar field does not
     * synchronize with any slot map.
     */
    MEMSAFE_NODISCARD constexpr generation_type generation() const noexcept {
        return generation_;
    }

    /**
     * @brief Test whether the handle is intrinsically non-default.
     *
     * @retval true Both components are outside their reserved default sentinels.
     * @retval false The index is `invalid_index` or the generation is
     * `invalid_generation`.
     * @pre The handle object is alive.
     * @post No state is modified.
     * @invariant This is only a non-stateful precondition check. It does not
     * prove that the owning slot still exists or that the slot generation still
     * matches.
     * @throws Nothing; this function is `constexpr noexcept`.
     * @note Ownership/thread-safety: no shared state is read. Full
     * generation-checked validity belongs to CPP_MEMSAFE-0205-FUNC.
     *
     * Example:
     * @code
     * memsafe::Handle<int> empty;
     * const bool empty_is_valid = empty.is_valid();
     * @endcode
     */
    MEMSAFE_NODISCARD constexpr bool is_valid() const noexcept {
        return index_ != invalid_index && generation_ != invalid_generation;
    }

    /**
     * @brief Compare two handles for identical stored components.
     *
     * @param lhs Left-hand handle.
     * @param rhs Right-hand handle.
     * @retval true Both handles store the same index and remembered generation.
     * @retval false At least one component differs.
     * @pre Both handle objects are alive.
     * @post No state is modified.
     * @invariant Equality is purely structural and never consults a slot map.
     * Default handles compare equal to each other but not to any allocated-style
     * handle.
     * @throws Nothing; this function is `constexpr noexcept`.
     * @note Ownership/thread-safety: comparison reads only scalar fields.
     */
    friend constexpr bool operator==(const Handle& lhs,
                                     const Handle& rhs) noexcept {
        return lhs.index_ == rhs.index_ && lhs.generation_ == rhs.generation_;
    }

    /**
     * @brief Compare two handles for different stored components.
     *
     * @param lhs Left-hand handle.
     * @param rhs Right-hand handle.
     * @retval true At least one stored component differs.
     * @retval false Both handles store the same index and remembered generation.
     * @pre Both handle objects are alive.
     * @post No state is modified.
     * @invariant Inequality is the logical negation of `operator==`.
     * @throws Nothing; this function is `constexpr noexcept`.
     * @note Ownership/thread-safety: comparison reads only scalar fields.
     */
    friend constexpr bool operator!=(const Handle& lhs,
                                     const Handle& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    /*
     * CPP_MEMSAFE-0200-FUNC acceptance requires Handle<T> to carry only index
     * plus remembered generation. Do not add owner, pointer, capacity, or
     * allocator state here; checked lookup needs the owning SlotMap context in
     * CPP_MEMSAFE-0205-FUNC.
     */
    index_type index_ = invalid_index;
    generation_type generation_ = invalid_generation;
};

/**
 * @brief Fixed-capacity lock-free slot allocator that mints generational
 * handles.
 *
 * @tparam T Object type constructed in the map's inline slots. `T` must be a
 * complete non-array object type when the map is instantiated and must be
 * constructible from the arguments passed to `allocate()`.
 * @tparam Capacity Compile-time storage bound for the map. The default template
 * argument is 64, matching the F3 `Slot_Capacity` model; tests may instantiate
 * smaller capacities such as `SlotMap<int, 2>`.
 *
 * @pre Public member functions require the `SlotMap` object to be alive.
 * Handles passed to `get()`, `deref()`, or `deallocate()` must have been issued
 * by this map and not already deallocated.
 * @post Construction creates a bounded free-list containing every configured
 * slot. Destruction ends all remaining live payload lifetimes.
 * @invariant The map never allocates dynamic slot storage, never uses a global
 * mutex, and never uses a global generation counter. Each slot carries its own
 * atomic generation, and the free-list head is one packed atomic word with an
 * ABA counter.
 * @throws Construction reports `capacity_exhausted` when the requested runtime
 * capacity exceeds the compile-time bound. `allocate()` may throw exceptions
 * from `T` construction or the configured violation policy. `get()`,
 * `deref()`, and `deallocate()` may throw `memsafe::violation` under THROW
 * policy or exceptions from an installed handler; `deallocate()` may also
 * propagate `T` destructor exceptions.
 * @note Ownership/thread-safety: `SlotMap` owns payload lifetimes and slot
 * reuse. Allocation and deallocation contend through a lock-free Treiber stack
 * of free indices. Returned pointers and references are non-owning borrows;
 * callers must synchronize ordinary `T` object access and must not use a
 * pointer after deallocating its handle.
 *
 * Example:
 * @code
 * memsafe::SlotMap<int, 2> map;
 * auto first = map.allocate(7);
 * int value = map.deref(first);
 * map.deallocate(first);
 * auto reused = map.allocate(9);
 * (void)(first.index() == reused.index());
 * (void)value;
 * @endcode
 */
template <typename T, std::size_t Capacity>
class SlotMap final {
    static_assert(std::is_object<T>::value,
                  "SlotMap<T> requires a complete non-array object type");
    static_assert(!std::is_array<T>::value,
                  "SlotMap<T> stores single objects and does not support arrays");
    static_assert(Capacity > 0U, "SlotMap<T, Capacity> requires Capacity > 0");
    static_assert(
        Capacity < static_cast<std::size_t>(Handle<T>::invalid_index),
        "SlotMap<T, Capacity> capacity must fit in Handle<T>::index_type");

public:
    /**
     * @typedef handle_type
     * @brief Handle value issued by this slot map.
     *
     * @return Type alias for `memsafe::Handle<T>`.
     * @pre Use handles with the `SlotMap<T, Capacity>` instance that issued
     * them.
     * @post No runtime state is modified.
     * @invariant The handle stores only an index and remembered generation; it
     * does not store a back-pointer to this map.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no resources. Handle values
     * are non-owning and do not synchronize access to payload objects.
     */
    using handle_type = Handle<T>;

    /**
     * @typedef index_type
     * @brief Unsigned slot-index type used by handles and free-list nodes.
     *
     * @return Type alias for the index component stored in `Handle<T>`.
     * @pre Values equal to `handle_type::invalid_index` are reserved for the
     * empty free-list and default-handle sentinel.
     * @post No runtime state is modified.
     * @invariant Every live slot index is smaller than both `capacity()` and the
     * invalid-index sentinel.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage.
     */
    using index_type = typename handle_type::index_type;

    /**
     * @typedef generation_type
     * @brief Unsigned per-slot generation representation.
     *
     * @return Type alias matching `Slot<T>::generation_type`.
     * @pre Generation zero is reserved for invalid/default handles.
     * @post No runtime state is modified.
     * @invariant Generation comparisons use each selected slot's atomic
     * counter; there is no map-wide generation counter.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage and introduce no
     * synchronization.
     */
    using generation_type = typename handle_type::generation_type;

    /**
     * @typedef size_type
     * @brief Unsigned size and capacity representation for this map.
     *
     * @return Type alias for public capacity and size observers.
     * @pre Values returned by this map fit in `index_type` by construction.
     * @post No runtime state is modified.
     * @invariant The compile-time capacity is checked against the handle index
     * sentinel so every active slot can be represented by a handle.
     * @throws Nothing; this is a type alias.
     * @note Ownership/thread-safety: aliases own no storage.
     */
    using size_type = std::size_t;

    /**
     * @brief Compile-time upper bound on the number of slots in this map.
     *
     * @return Constant expression equal to the template `Capacity`.
     * @pre The value is positive and smaller than `Handle<T>::invalid_index`.
     * @post No runtime state is modified.
     * @invariant The backing slot array contains exactly this many slot records.
     * @throws Nothing; this is a compile-time constant.
     * @note Ownership/thread-safety: the constant owns no storage.
     */
    static constexpr size_type max_capacity = Capacity;

    /**
     * @brief Construct a slot map with a bounded active capacity.
     *
     * @param requested_capacity Number of slots to place in the initial free
     * list. The default uses the full compile-time `Capacity`.
     * @return No value; constructors initialize the receiving object.
     * @pre `requested_capacity <= max_capacity` and the value fits in
     * `Handle<T>::index_type`.
     * @post `capacity()` reports `requested_capacity`, `size()` is zero, and
     * every active slot index is reachable from the packed-head free list.
     * @invariant Free-list initialization stores only slot indices; slot
     * payload lifetimes have not begun.
     * @throws Reports `capacity_exhausted` through the configured policy when
     * `requested_capacity` exceeds the fixed bound. If a HANDLER policy returns
     * after that violation, the map is left with zero active capacity.
     * @note Ownership/thread-safety: construction owns the slot array and must
     * complete before the map is shared with other threads.
     *
     * Example:
     * @code
     * memsafe::SlotMap<int, 8> full;
     * memsafe::SlotMap<int, 8> small(2);
     * @endcode
     */
    explicit SlotMap(size_type requested_capacity = Capacity)
        : capacity_(bounded_capacity_or_zero(requested_capacity)),
          live_count_(0U),
          free_head_(pack_free_head(handle_type::invalid_index, 0U)),
          records_() {
        if (!is_supported_requested_capacity(requested_capacity)) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::capacity_exhausted,
                "SlotMap requested capacity exceeds fixed storage");
        }

        initialize_free_list();
    }

    /**
     * @brief Disable copying because the map owns payload lifetimes.
     *
     * @param other Source map that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying a `SlotMap` is a compile-time error.
     * @post No duplicate slot owner is created.
     * @invariant Handles name indices into exactly one owning map instance.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling copies prevents double
     * destruction and duplicated free-list ownership.
     */
    SlotMap(const SlotMap& other) = delete;

    /**
     * @brief Disable copy assignment because the map owns payload lifetimes.
     *
     * @param other Source map that would otherwise be assigned.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post No existing slot array is overwritten by another owner.
     * @invariant Free-list head, per-slot generations, and live payloads remain
     * tied to one map object.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling assignment avoids racing or
     * duplicating payload destruction.
     */
    SlotMap& operator=(const SlotMap& other) = delete;

    /**
     * @brief Disable moving so issued handles keep stable map identity.
     *
     * @param other Source map that would otherwise be moved.
     * @return No value; this overload is deleted.
     * @pre Not available. Move construction is a compile-time error.
     * @post Existing handles cannot silently start referring to a different map
     * object after storage relocation.
     * @invariant Slot indices are meaningful only for the original map object.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling moves preserves address and
     * ownership stability for the entire slot array.
     */
    SlotMap(SlotMap&& other) = delete;

    /**
     * @brief Disable move assignment for the same stability reason as move
     * construction.
     *
     * @param other Source map that would otherwise be assigned.
     * @return No value; this overload is deleted.
     * @pre Not available. Move assignment is a compile-time error.
     * @post An existing map cannot be replaced while handles to its slots may
     * still exist.
     * @invariant Free-list and generation state remain owned by one stable map.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: disabling moves avoids transferring slot
     * storage without coordinating outstanding handles.
     */
    SlotMap& operator=(SlotMap&& other) = delete;

    /**
     * @brief Destroy the map and every payload still live in its slots.
     *
     * @return No value; destructors release the receiving object.
     * @pre No other thread is concurrently calling member functions on this map.
     * @post All live payload lifetimes owned by the map have ended and the slot
     * array storage is released with the map object.
     * @invariant Destruction does not report stale handles; outstanding handles
     * are non-owning values and become unusable when the map dies.
     * @throws Propagates a `T` destructor exception when `T` is not nothrow
     * destructible.
     * @note Ownership/thread-safety: teardown is externally synchronized and
     * consumes the owning map. Outstanding pointers or handles must not be used
     * afterwards.
     */
    ~SlotMap() noexcept(std::is_nothrow_destructible<T>::value) {
        destroy_live_slots();
    }

    /**
     * @brief Allocate one slot and construct a `T` payload in place.
     *
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param args Arguments forwarded exactly once to `T`'s constructor.
     * @return A non-default `Handle<T>` naming the allocated slot and its
     * remembered generation.
     * @pre The map has at least one free slot, and `T` is constructible from
     * `Args&&...`.
     * @post On success, `size()` is increased by one, the returned handle's
     * index was removed from the free-list, and that slot contains a live `T`.
     * If construction throws, the slot index is pushed back before the
     * exception escapes.
     * @invariant Allocation obtains capacity by a Treiber-stack pop using the
     * shared packed-head CAS path and never consults a global allocator state.
     * @throws Reports `capacity_exhausted` through the configured policy when
     * no free slot is available. Propagates exceptions from `T` construction.
     * If a HANDLER policy returns after exhaustion, a default-invalid handle is
     * returned.
     * @note Ownership/thread-safety: slot-index allocation is lock-free. The
     * returned handle is non-owning and does not synchronize later payload
     * access.
     *
     * Example:
     * @code
     * memsafe::SlotMap<int, 1> map;
     * auto h = map.allocate(42);
     * @endcode
     */
    template <typename... Args>
    MEMSAFE_NODISCARD handle_type allocate(Args&&... args) {
        const index_type index = pop_free_index();
        if (index == handle_type::invalid_index) {
            MEMSAFE_DETAIL_VIOLATE(
                ::memsafe::violation_kind::capacity_exhausted,
                "SlotMap capacity exhausted");
            return handle_type{};
        }

        slot_record& record = records_[index];
        try {
            record.slot.construct(std::forward<Args>(args)...);
        } catch (...) {
            /*
             * F2 bounded-capacity/property tests require constructor failure
             * not to leak a slot. Reusing the same Treiber push path also keeps
             * the ABA counter monotonic for failed allocations.
             */
            push_free_index(index);
            throw;
        }

        const generation_type generation = record.slot.generation();
        record.occupied.store(true, std::memory_order_release);
        live_count_.fetch_add(1U, std::memory_order_acq_rel);
        return handle_type{index, generation};
    }

    /**
     * @brief Deallocate the slot named by a live handle.
     *
     * @param handle Handle previously returned by this map's `allocate()`.
     * @return Nothing.
     * @pre `handle` names a currently live slot in this map. Passing a stale,
     * default, foreign, or already deallocated handle is a use-after-free
     * violation when checks are enabled.
     * @post On success, the payload lifetime has ended, the slot generation has
     * been atomically incremented, `size()` is decreased by one, and the slot
     * index is pushed onto the free-list for immediate reuse.
     * @invariant The generation compare-and-bump is per-slot. The free-list
     * push uses the same packed-head CAS helper as allocation pop, so the ABA
     * counter advances on every successful push or pop.
     * @throws Reports `use_after_free` through the configured policy when the
     * handle does not match a live slot. Propagates a `T` destructor exception
     * after the slot has been logically retired and returned to the free list.
     * @note Ownership/thread-safety: the free-list mutation is lock-free. The
     * caller owns synchronization that prevents ordinary data races on the
     * payload being destroyed.
     *
     * Example:
     * @code
     * memsafe::SlotMap<int, 1> map;
     * auto h = map.allocate(1);
     * map.deallocate(h);
     * @endcode
     */
    void deallocate(handle_type handle) {
        const index_type index =
            checked_index(handle, "cannot deallocate a stale SlotMap handle");
        if (index == handle_type::invalid_index) {
            return;
        }

        slot_record& record = records_[index];
        generation_type updated_generation = handle_type::invalid_generation;
        if (!record.slot.try_bump_generation_from(handle.generation(),
                                                  updated_generation)) {
            report_use_after_free("cannot deallocate a stale SlotMap handle");
            return;
        }

        /*
         * The generation bump is the logical retirement point from F1/F3:
         * stale readers reject the old handle before the index is made
         * allocatable again through the Treiber stack.
         */
        record.occupied.store(false, std::memory_order_release);
        try {
            record.slot.destroy();
        } catch (...) {
            finish_deallocation(index);
            throw;
        }
        finish_deallocation(index);
        (void)updated_generation;
    }

    /**
     * @brief Return a mutable pointer for a live handle after generation check.
     *
     * @param handle Handle naming the slot to access.
     * @return Pointer to the live payload, or null only if a violation handler
     * returns after a failed check.
     * @pre `handle` was issued by this map and has not been deallocated.
     * @post No ownership or free-list state is modified.
     * @invariant Lookup performs one atomic generation load from the selected
     * slot and compares it with the handle's remembered generation; no global
     * mutex or generation counter participates.
     * @throws Reports `use_after_free` through the configured policy when the
     * handle is default, out of range, already freed, or generation-stale.
     * @note Ownership/thread-safety: the returned pointer is borrowed from this
     * map. The caller must synchronize concurrent reads/writes to the payload.
     *
     * Example:
     * @code
     * memsafe::SlotMap<int, 1> map;
     * auto h = map.allocate(3);
     * *map.get(h) = 4;
     * @endcode
     */
    T* get(handle_type handle) MEMSAFE_BORROWS(*this) {
        const index_type index =
            checked_index(handle, "cannot get value for a stale SlotMap handle");
        if (index == handle_type::invalid_index) {
            return nullptr;
        }
        return records_[index].slot.value_ptr();
    }

    /**
     * @brief Return a const pointer for a live handle after generation check.
     *
     * @param handle Handle naming the slot to access.
     * @return Const pointer to the live payload, or null only if a violation
     * handler returns after a failed check.
     * @pre `handle` was issued by this map and has not been deallocated.
     * @post No ownership or free-list state is modified.
     * @invariant Lookup performs one atomic generation load from the selected
     * slot and compares it with the handle's remembered generation.
     * @throws Reports `use_after_free` through the configured policy when the
     * handle is default, out of range, already freed, or generation-stale.
     * @note Ownership/thread-safety: the returned pointer is borrowed and does
     * not extend the payload lifetime.
     *
     * Example:
     * @code
     * const memsafe::SlotMap<int, 1>& map_ref = map;
     * const int* value = map_ref.get(h);
     * @endcode
     */
    const T* get(handle_type handle) const MEMSAFE_BORROWS(*this) {
        const index_type index =
            checked_index(handle, "cannot get value for a stale SlotMap handle");
        if (index == handle_type::invalid_index) {
            return nullptr;
        }
        return records_[index].slot.value_ptr();
    }

    /**
     * @brief Dereference a live handle as a mutable reference.
     *
     * @param handle Handle naming the slot to access.
     * @return Mutable reference to the live payload.
     * @pre `handle` was issued by this map and has not been deallocated.
     * @post No ownership or free-list state is modified.
     * @invariant This is the reference-returning companion to `get()` and uses
     * the same generation-checked path.
     * @throws Reports `use_after_free` through the configured policy when the
     * handle is stale or invalid; a returning handler leaves this function with
     * no valid object to reference, matching the rest of the library's
     * fail-fast violation contract.
     * @note Ownership/thread-safety: the returned reference is borrowed and
     * requires caller-side synchronization for ordinary payload access.
     *
     * Example:
     * @code
     * auto h = map.allocate(5);
     * map.deref(h) = 6;
     * @endcode
     */
    T& deref(handle_type handle) MEMSAFE_BORROWS(*this) {
        return *get(handle);
    }

    /**
     * @brief Dereference a live handle as a const reference.
     *
     * @param handle Handle naming the slot to access.
     * @return Const reference to the live payload.
     * @pre `handle` was issued by this map and has not been deallocated.
     * @post No ownership or free-list state is modified.
     * @invariant This is the const reference-returning companion to `get()` and
     * uses the same generation-checked path.
     * @throws Reports `use_after_free` through the configured policy when the
     * handle is stale or invalid.
     * @note Ownership/thread-safety: the returned reference is borrowed and
     * requires caller-side synchronization for payload access.
     */
    const T& deref(handle_type handle) const MEMSAFE_BORROWS(*this) {
        return *get(handle);
    }

    /**
     * @brief Return the active bounded capacity of this map.
     *
     * @return Number of slot indices initially placed in the free-list.
     * @pre The map object is alive.
     * @post No state is modified.
     * @invariant The result never exceeds `max_capacity` and never equals the
     * handle invalid-index sentinel.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this is an immutable observer and is safe
     * to call concurrently with allocation and deallocation.
     */
    MEMSAFE_NODISCARD size_type capacity() const noexcept {
        return capacity_;
    }

    /**
     * @brief Return the number of currently live payloads.
     *
     * @return Count of slots allocated and not yet successfully deallocated.
     * @pre The map object is alive.
     * @post No state is modified.
     * @invariant The count is updated atomically after construction and after
     * payload destruction. It is an observation under concurrency, not a
     * reservation.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: concurrent calls are atomic and may observe
     * any value consistent with in-flight allocation/deallocation operations.
     */
    MEMSAFE_NODISCARD size_type size() const noexcept {
        return live_count_.load(std::memory_order_acquire);
    }

private:
    using head_word = std::uint64_t;

    /**
     * @brief Internal record pairing one slot with its free-list metadata.
     *
     * @pre Records are owned exclusively by one `SlotMap<T, Capacity>` instance.
     * @post Construction leaves no live payload, no free-list successor, and an
     * unoccupied marker until `initialize_free_list()` wires usable records.
     * @invariant `slot.generation()` is per-record atomic state; `next_free`
     * participates only in the Treiber free list; `occupied` guards stale handle
     * rejection after generation validation.
     * @throws Nothing; construction is `noexcept`.
     * @note Ownership/thread-safety: the record owns its slot storage inline.
     * Free-list and occupied flags are atomic because allocation/deallocation
     * may race across threads, while payload access remains caller-synchronized.
     */
    struct slot_record {
        /**
         * @brief Construct an empty slot record with sentinel free-list state.
         *
         * @return A record with default slot generation, invalid next index,
         * and `occupied == false`.
         * @pre None.
         * @post No payload lifetime is active; the record is ready for the
         * owning map's free-list initialization pass.
         * @invariant The constructor does not publish the record to the free
         * list; publication is centralized in `initialize_free_list()`.
         * @throws Nothing; this constructor is `noexcept`.
         * @note Ownership/thread-safety: construction occurs before the map is
         * shared with other threads.
         */
        slot_record() noexcept
            : slot(),
              next_free(handle_type::invalid_index),
              occupied(false) {}

        /// Payload storage plus the per-slot atomic generation counter.
        Slot<T> slot;
        /// Singly linked free-list successor used only while the record is free.
        std::atomic<index_type> next_free;
        /// Atomic occupancy flag used to reject stale or duplicate handles.
        std::atomic<bool> occupied;
    };

    /// Mask for extracting the low 32-bit slot index from a packed head word.
    static constexpr head_word index_mask = 0xffffffffULL;

    /**
     * @brief Check whether a runtime capacity can be represented by this map.
     *
     * @param requested Runtime slot capacity requested by the constructor.
     * @retval true `requested` fits inside the compile-time capacity and cannot
     * collide with the handle invalid-index sentinel.
     * @retval false `requested` exceeds either bound and must be reported as a
     * capacity violation by the public constructor.
     * @pre `requested` is the user-supplied capacity for this bounded map.
     * @post No state is modified.
     * @invariant F3 `Bounded_Capacity` and `Slot_Capacity` require a fixed
     * representable capacity; this helper rejects values that cannot be encoded
     * in a `Handle<T>` index.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the helper is pure and owns no shared
     * state.
     */
    static bool is_supported_requested_capacity(size_type requested) noexcept {
        return requested <= max_capacity &&
               requested < static_cast<size_type>(handle_type::invalid_index);
    }

    /**
     * @brief Convert an unsupported runtime capacity request to an empty map.
     *
     * @param requested Runtime slot capacity requested by the constructor.
     * @return `requested` when supported, otherwise zero.
     * @pre `requested` is the user-supplied capacity for this bounded map.
     * @post No state is modified; the public constructor remains responsible
     * for reporting `capacity_exhausted` when support is absent.
     * @invariant Unsupported construction never creates partially usable slots.
     * Zero capacity makes subsequent allocation fail through the same
     * `capacity_exhausted` path as a full free list.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the helper is pure and owns no shared
     * state.
     */
    static size_type bounded_capacity_or_zero(size_type requested) noexcept {
        return is_supported_requested_capacity(requested) ? requested : 0U;
    }

    /**
     * @brief Pack a free-list head index and ABA counter into one CAS word.
     *
     * @param index Slot index to publish as the Treiber stack head, or
     * `handle_type::invalid_index` for an empty stack.
     * @param aba_counter Counter value stored in the high 32 bits.
     * @return Packed 64-bit head word with index in the low half and ABA counter
     * in the high half.
     * @pre `index` is either a valid slot index below `capacity_` or the invalid
     * sentinel; `aba_counter` is the next counter chosen by the caller.
     * @post No state is modified.
     * @invariant F1[5]/E7 requires pop and push to use one packed-head CAS path;
     * this helper defines the single representation used by both paths.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: packing is pure arithmetic. Atomic
     * publication happens only through `compare_exchange_free_head()`.
     *
     * Example:
     * @code
     * auto empty = SlotMap<int>::pack_free_head(
     *     memsafe::Handle<int>::invalid_index, 0u);
     * (void)empty;
     * @endcode
     */
    static head_word pack_free_head(index_type index,
                                    std::uint32_t aba_counter) noexcept {
        return (static_cast<head_word>(aba_counter) << 32U) |
               static_cast<head_word>(index);
    }

    /**
     * @brief Extract the head slot index from a packed free-list word.
     *
     * @param packed Head word previously produced by `pack_free_head()`.
     * @return Low 32-bit slot index component.
     * @pre `packed` is a value loaded from `free_head_` or produced by the head
     * packing helper.
     * @post No state is modified.
     * @invariant The extraction mirrors `pack_free_head()` so every pop/push
     * observes the same index encoding.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: extraction is pure arithmetic and owns no
     * shared state.
     */
    static index_type free_head_index(head_word packed) noexcept {
        return static_cast<index_type>(packed & index_mask);
    }

    /**
     * @brief Extract the ABA counter from a packed free-list word.
     *
     * @param packed Head word previously produced by `pack_free_head()`.
     * @return High 32-bit ABA counter component.
     * @pre `packed` is a value loaded from `free_head_` or produced by the head
     * packing helper.
     * @post No state is modified.
     * @invariant The counter value is advanced only by
     * `compare_exchange_free_head()`, preserving one shared CAS path for pop and
     * push.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: extraction is pure arithmetic and owns no
     * shared state.
     */
    static std::uint32_t free_head_aba(head_word packed) noexcept {
        return static_cast<std::uint32_t>(packed >> 32U);
    }

    /**
     * @brief Build the initial Treiber free list over the bounded slot array.
     *
     * @return Nothing.
     * @pre `capacity_` has been clamped to a supported value and no concurrent
     * operation can observe this map yet.
     * @post Every record below `capacity_` is linked in ascending-index free-list
     * order, every record is marked unoccupied, and `free_head_` names index zero
     * or the invalid sentinel for an empty map.
     * @invariant Records at or above `capacity_` are never reachable from the
     * free list, preserving F3 `Bounded_Capacity`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: initialization happens during construction
     * before publication of the map to other threads.
     */
    void initialize_free_list() noexcept {
        for (size_type i = 0U; i < max_capacity; ++i) {
            records_[i].next_free.store(handle_type::invalid_index,
                                        std::memory_order_relaxed);
            records_[i].occupied.store(false, std::memory_order_relaxed);
        }

        for (size_type i = 0U; i < capacity_; ++i) {
            const index_type next =
                (i + 1U < capacity_)
                    ? static_cast<index_type>(i + 1U)
                    : handle_type::invalid_index;
            records_[i].next_free.store(next, std::memory_order_relaxed);
        }

        const index_type first =
            capacity_ == 0U ? handle_type::invalid_index : index_type{0};
        free_head_.store(pack_free_head(first, 0U), std::memory_order_release);
    }

    /**
     * @brief Swing the packed Treiber head to a desired index and bump ABA.
     *
     * @param observed In/out packed head word. On entry it is the caller's
     * observed head value; on CAS failure it is updated by the atomic operation
     * with the current head value.
     * @param desired_index Slot index to publish as the new free-list head.
     * @param success Memory ordering for a successful compare-and-swap.
     * @param failure Memory ordering for a failed compare-and-swap.
     * @retval true The packed head was updated to `desired_index` with the ABA
     * counter incremented from `observed`.
     * @retval false The packed head changed before the update; `observed` now
     * contains the current packed head.
     * @pre The caller has prepared any per-node `next_free` state required by
     * the Treiber operation before publishing `desired_index`.
     * @post On success, exactly one packed `free_head_` CAS has linearized the
     * pop or push operation. On failure, no state is changed by this helper
     * beyond the compare-exchange writeback to `observed`.
     * @invariant Both `pop_free_index()` and `push_free_index()` use this same
     * helper, satisfying the CPP_MEMSAFE-0205-FUNC requirement for one
     * packed-head CAS path with an ABA counter.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this is the synchronization point for the
     * lock-free free list. It owns no payload lifetime and does not touch slot
     * generations.
     */
    bool compare_exchange_free_head(head_word& observed,
                                    index_type desired_index,
                                    std::memory_order success,
                                    std::memory_order failure) noexcept {
        const head_word desired = pack_free_head(
            desired_index,
            static_cast<std::uint32_t>(free_head_aba(observed) + 1U));
        return free_head_.compare_exchange_weak(observed,
                                                desired,
                                                success,
                                                 failure);
    }

    /**
     * @brief Pop one free slot index from the Treiber free list.
     *
     * @return A valid free slot index, or `handle_type::invalid_index` when the
     * bounded capacity is exhausted.
     * @pre The map has been initialized and may be concurrently used by
     * allocation/deallocation operations.
     * @post On success, the returned slot is no longer reachable from the free
     * list and its `next_free` link is reset to the invalid sentinel. On
     * exhaustion, no state is modified.
     * @invariant The operation linearizes through
     * `compare_exchange_free_head()`, preserving the single packed-head CAS path
     * required by F1[5]/E7 and F3 `Treiber_Stack`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this helper reserves only the slot index.
     * The caller constructs the payload, marks occupancy, and mints the handle.
     *
     * Example:
     * @code
     * // SlotMap<T>::allocate() uses this helper before placement construction.
     * @endcode
     */
    index_type pop_free_index() noexcept {
        head_word observed = free_head_.load(std::memory_order_acquire);
        for (;;) {
            const index_type index = free_head_index(observed);
            if (index == handle_type::invalid_index) {
                return handle_type::invalid_index;
            }

            const index_type next =
                records_[index].next_free.load(std::memory_order_acquire);
            if (compare_exchange_free_head(observed,
                                           next,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                records_[index].next_free.store(handle_type::invalid_index,
                                                std::memory_order_release);
                return index;
            }
        }
    }

    /**
     * @brief Push a freed slot index back onto the Treiber free list.
     *
     * @param index Slot index that has already had its payload destroyed and
     * generation retired.
     * @return Nothing.
     * @pre `index < capacity_`, the record is unoccupied, and no live `T`
     * payload remains in the slot.
     * @post The slot index is reachable from `free_head_` after one successful
     * packed-head CAS.
     * @invariant The operation writes the node link before CAS publication and
     * linearizes through `compare_exchange_free_head()`, matching F1[5]/E7's
     * Treiber stack requirement.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: this helper publishes only the reusable
     * index. The caller has already advanced the per-slot generation so stale
     * handles are rejected even if the slot is immediately reallocated.
     */
    void push_free_index(index_type index) noexcept {
        head_word observed = free_head_.load(std::memory_order_acquire);
        for (;;) {
            /*
             * F1[5]/E7 requires a Treiber stack: publish the node's next link
             * before the single packed-head CAS attempts to swing the head to
             * this slot index.
             */
            records_[index].next_free.store(free_head_index(observed),
                                            std::memory_order_release);
            if (compare_exchange_free_head(observed,
                                           index,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                return;
            }
        }
    }

    /**
     * @brief Report a stale-handle access through the configured policy.
     *
     * @param message Static diagnostic string describing the rejected operation.
     * @return Nothing.
     * @pre The caller has determined that a handle is default, out of range,
     * generation-stale, or not currently occupied.
     * @post When release checks are enabled, `MEMSAFE_DETAIL_VIOLATE` is invoked
     * with `violation_kind::use_after_free`; otherwise the message is consumed
     * without side effects.
     * @invariant All invalid `get()`, `deref()`, and `deallocate()` handle paths
     * converge here so stale handles are rejected through the configured
     * MEMSAFE policy.
     * @throws May throw when the active violation policy is configured to throw
     * or when a configured violation handler throws.
     * @note Ownership/thread-safety: reporting owns no slot state. The installed
     * violation policy controls any process-wide synchronization or termination
     * behavior.
     */
    void report_use_after_free(const char* message) const {
#if MEMSAFE_RELEASE_CHECKS
        MEMSAFE_DETAIL_VIOLATE(::memsafe::violation_kind::use_after_free,
                               message);
#else
        (void)message;
#endif
    }

    /**
     * @brief Validate a handle and return the addressed slot index.
     *
     * @param handle Handle supplied to `get()`, `deref()`, or `deallocate()`.
     * @param message Diagnostic string forwarded when validation fails.
     * @return The validated slot index, or `handle_type::invalid_index` after
     * reporting a use-after-free violation.
     * @pre `handle` may be any handle value; callers must tolerate rejection.
     * @post No state is modified. Invalid, out-of-range, generation-stale, and
     * unoccupied handles have been reported through `report_use_after_free()`.
     * @invariant Lookup performs one per-slot atomic generation load and never
     * consults a global generation counter or mutex, matching F3
     * `Generation_Checked` and `Atomic_Generation`.
     * @throws May throw if `report_use_after_free()` throws under the active
     * violation policy.
     * @note Ownership/thread-safety: a successful result is a borrowed index
     * only. The caller remains responsible for payload synchronization while
     * using the returned slot.
     *
     * Example:
     * @code
     * // SlotMap<T>::get() calls this before returning a payload pointer.
     * @endcode
     */
    index_type checked_index(handle_type handle, const char* message) const {
        if (!handle.is_valid()) {
            report_use_after_free(message);
            return handle_type::invalid_index;
        }

        const size_type index_as_size = static_cast<size_type>(handle.index());
        if (index_as_size >= capacity_) {
            report_use_after_free(message);
            return handle_type::invalid_index;
        }

        const slot_record& record = records_[handle.index()];
        const generation_type observed_generation = record.slot.generation();
        if (observed_generation != handle.generation()) {
            report_use_after_free(message);
            return handle_type::invalid_index;
        }

        if (!record.occupied.load(std::memory_order_acquire)) {
            report_use_after_free(message);
            return handle_type::invalid_index;
        }

        return handle.index();
    }

    /**
     * @brief Complete successful deallocation accounting and free-list publish.
     *
     * @param index Slot index whose payload has already been destroyed and whose
     * generation has already been advanced.
     * @return Nothing.
     * @pre `index < capacity_`, `records_[index].occupied == false`, and no live
     * payload remains in the slot.
     * @post `size()` is decremented and the slot index is available for reuse by
     * a later `allocate()` call.
     * @invariant The generation bump precedes this helper, so deallocate-then-
     * allocate reuse cannot make a stale handle valid again.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the live count is atomic and the free-list
     * push is lock-free. The helper owns no payload lifetime.
     */
    void finish_deallocation(index_type index) noexcept {
        live_count_.fetch_sub(1U, std::memory_order_acq_rel);
        push_free_index(index);
    }

    /**
     * @brief Destroy all currently live payloads during map destruction.
     *
     * @return Nothing.
     * @pre The `SlotMap<T, Capacity>` destructor is running; no concurrent user
     * operation may access this map.
     * @post Every occupied record has been marked unoccupied and destroyed,
     * `live_count_` is zero, and `free_head_` is set to the empty sentinel.
     * @invariant Destruction does not reuse slots or mint handles; it only ends
     * remaining payload lifetimes owned by the map.
     * @throws Nothing when `T` is nothrow-destructible; otherwise it has the same
     * exception behavior as `T`'s destructor and the enclosing destructor's
     * exception specification.
     * @note Ownership/thread-safety: this helper runs after external ownership
     * of the map has ended. It intentionally does not rebuild the free list
     * because the map object is being torn down.
     */
    void destroy_live_slots() noexcept(std::is_nothrow_destructible<T>::value) {
        for (size_type i = 0U; i < capacity_; ++i) {
            slot_record& record = records_[i];
            if (record.occupied.exchange(false, std::memory_order_acq_rel)) {
                record.slot.destroy();
            }
        }
        live_count_.store(0U, std::memory_order_release);
        free_head_.store(pack_free_head(handle_type::invalid_index, 0U),
                         std::memory_order_release);
    }

    /// Runtime capacity bounded by the compile-time `Capacity` parameter.
    size_type capacity_;
    /// Number of currently occupied slots, maintained atomically.
    std::atomic<size_type> live_count_;
    /// Packed Treiber stack head: low 32 bits are index, high 32 bits are ABA.
    std::atomic<head_word> free_head_;
    /// Fixed storage backing every possible slot in this bounded map.
    std::array<slot_record, Capacity> records_;
};

namespace detail {

/**
 * @brief Complete probe type used only for handle layout static assertions.
 *
 * @pre None.
 * @post No runtime state is modified.
 * @invariant The type is intentionally ordinary and complete so `Slot<T>` and
 * `Handle<T>` can be instantiated for compile-time contract checks.
 * @throws Nothing; the type has only implicit trivial special members.
 * @note Ownership/thread-safety: instances, if any are created by user code, own
 * one integer and no shared state.
 */
struct handle_layout_probe {
    /// Concrete payload field that makes the probe a complete object type.
    int value;
};

static_assert(std::is_trivially_copyable<Handle<handle_layout_probe>>::value,
              "Handle<T> must be trivially copyable / cheap to copy");
static_assert(sizeof(Handle<handle_layout_probe>) ==
                  sizeof(Handle<handle_layout_probe>::index_type) +
                      sizeof(Handle<handle_layout_probe>::generation_type),
              "Handle<T> must carry only an index plus remembered generation");
static_assert(!Handle<handle_layout_probe>{}.is_valid(),
              "a default-constructed Handle<T> must never be valid");
static_assert(Handle<handle_layout_probe>{} !=
                  Handle<handle_layout_probe>{
                      0u, Slot<handle_layout_probe>::initial_generation},
              "a default Handle<T> must not equal an allocated-style handle");
static_assert(Slot<handle_layout_probe>::initial_generation !=
                  Slot<handle_layout_probe>::invalid_generation,
              "Slot<T> generations must start at a nonzero value");

} // namespace detail

} // namespace memsafe

#endif /* MEMSAFE_HANDLE_HPP */
