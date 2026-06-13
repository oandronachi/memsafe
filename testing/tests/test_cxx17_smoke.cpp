/**
 * @file test_cxx17_smoke.cpp
 * @brief C++17 compatibility and Valgrind-clean smoke coverage for the full memsafe roster.
 *
 * @details
 * Work package: CPP_MEMSAFE-0810-TEST.
 *
 * Purpose:
 * - Validate the finalized CPP_MEMSAFE-0800-FUNC compatibility layer from a
 *   standalone F4 runtime test discovered as `testing/tests/*.cpp`.
 * - Compile the umbrella header and the complete Slice 8 type roster in every
 *   default lane, with additional compile-time assertions when the F4 `cxx17`
 *   and optional `valgrind-cxx17` lanes define `MEMSAFE_CXX17_COMPAT=1`.
 * - Exercise small, positive API flows for `Owner`, `Ref`, `MutRef`, `Slot`,
 *   `Handle`, `SlotMap`, `Scope`, `SyncOwner`, `SyncRef`, `SyncMutRef`, `Arc`,
 *   and `Mutex` so the C++17 lane does more than parse declarations.
 *
 * Key invariants:
 * - The test does not define `MEMSAFE_CXX17_COMPAT`; lane selection remains the
 *   responsibility of `testing/infra_lanes.json`.
 * - When compatibility mode is active, `MEMSAFE_HAS_CONCEPTS` and
 *   `MEMSAFE_HAS_SOURCE_LOCATION` must both be zero, matching F1 Slice 8's
 *   "replace concepts with SFINAE" and "drop std::source_location" contract.
 * - Runtime coverage is strictly positive and releases every resource through
 *   ordinary RAII. The optional Linux Valgrind memcheck lane should therefore
 *   report no leaks, invalid accesses, or uninitialized reads from this file.
 * - No sanitizer-negative stale raw pointer access appears here. F2 assigns
 *   those Scope oracles to dedicated sanitizer/memcheck tests; this package is
 *   the C++17 smoke for the normal subset.
 *
 * Ownership and thread-safety:
 * - All objects are automatic storage owned by the test process. Heap and arena
 *   allocations are released by `Owner`, `SlotMap`, `Scope`, and `Arc` before
 *   process exit.
 * - The smoke test starts no worker threads. `SyncOwner` and `Mutex` are
 *   exercised only on the main thread to validate their C++17 surface without
 *   duplicating the separate concurrency work packages.
 */
#include "../test_harness.hpp"

#include <memsafe/memsafe.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

static_assert(MEMSAFE_LANG_VERSION >= 201703L,
              "memsafe compatibility smoke requires at least C++17");

#if MEMSAFE_CXX17_COMPAT
static_assert(MEMSAFE_HAS_CONCEPTS == 0,
              "MEMSAFE_CXX17_COMPAT must keep concept syntax unreachable");
static_assert(MEMSAFE_HAS_SOURCE_LOCATION == 0,
              "MEMSAFE_CXX17_COMPAT must keep std::source_location unreachable");
#endif

static_assert(std::is_same<memsafe::detail::remove_cvref_t<const int&>, int>::value,
              "C++17 remove_cvref compatibility alias must normalize payload types");
static_assert(memsafe::detail::is_constructible_object<std::string, const char*>::value,
              "C++17 constructible-object SFINAE helper must accept valid payloads");
static_assert(!memsafe::detail::is_complete_object<int[2]>::value,
              "C++17 object SFINAE helper must reject array payloads");

static_assert(!std::is_copy_constructible<memsafe::Owner<int>>::value,
              "Owner<T> must remain move-only in the C++17 lane");
static_assert(std::is_nothrow_move_constructible<memsafe::Owner<int>>::value,
              "Owner<T> must keep noexcept move construction in the C++17 lane");
static_assert(std::is_nothrow_move_assignable<memsafe::Owner<int>>::value,
              "Owner<T> must keep noexcept move assignment in the C++17 lane");

static_assert(!std::is_copy_constructible<memsafe::SyncOwner<int>>::value,
              "SyncOwner<T> must remain move-only in the C++17 lane");
static_assert(std::is_nothrow_move_constructible<memsafe::SyncOwner<int>>::value,
              "SyncOwner<T> must keep noexcept move construction in the C++17 lane");
static_assert(std::is_nothrow_move_assignable<memsafe::SyncOwner<int>>::value,
              "SyncOwner<T> must keep noexcept move assignment in the C++17 lane");

static_assert(std::is_trivially_copyable<memsafe::Handle<int>>::value,
              "Handle<T> must stay a cheap value type in the C++17 lane");
static_assert(!memsafe::Handle<int>{}.is_valid(),
              "Default Handle<T> must stay invalid in the C++17 lane");

namespace {

/**
 * @brief Ledger used to prove that `Scope` destroys C++17 smoke payloads.
 *
 * @pre The ledger must outlive every `scope_smoke_payload` that stores its
 * address.
 * @post Payload construction and destruction update the counters directly.
 * @invariant `destroyed` never exceeds `constructed` in the passing test path.
 * @throws This aggregate owns no dynamic resources and does not throw.
 * @note Ownership/thread-safety: the ledger is stack-owned by the single test
 * thread and is not shared.
 */
struct scope_smoke_ledger final {
    /// Number of payload constructors that completed successfully.
    int constructed = 0;
    /// Number of payload destructors observed so far.
    int destroyed = 0;
    /// Last value recorded by a payload destructor.
    int last_destroyed_value = 0;
};

/**
 * @brief Scope-owned payload with an observable non-throwing destructor.
 *
 * @details
 * `memsafe::Scope::create<T>` requires a nothrow-destructible payload. This
 * type gives the smoke test a destructor oracle without relying on external
 * tooling or on undefined post-scope pointer use.
 *
 * Example:
 * @code
 * scope_smoke_ledger ledger;
 * {
 *     memsafe::Scope scope;
 *     auto* payload = scope.create<scope_smoke_payload>(ledger, 7);
 *     payload->set_value(9);
 * }
 * CHECK(ledger.destroyed == 1);
 * @endcode
 *
 * @pre The referenced ledger remains alive until this payload is destroyed by
 * its owning `memsafe::Scope`.
 * @post Construction increments `constructed`; destruction increments
 * `destroyed` and records the final payload value.
 * @invariant `ledger_` points to the constructor-supplied ledger for the whole
 * object lifetime.
 * @throws Nothing; construction, destruction, observation, and mutation are
 * `noexcept`.
 * @note Ownership/thread-safety: instances are owned by `memsafe::Scope` and
 * used on the main test thread.
 */
class scope_smoke_payload final {
public:
    /**
     * @brief Construct a payload and record the construction event.
     *
     * @param ledger Ledger that receives construction and destruction counts.
     * @param initial_value Initial integer payload.
     * @return Constructors do not return a value.
     * @pre `ledger` outlives the constructed payload.
     * @post `ledger.constructed` has increased by one and `value()` returns
     * `initial_value`.
     * @invariant The stored ledger pointer is non-null after construction.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes only the receiving object and
     * caller-owned ledger on one thread.
     */
    scope_smoke_payload(scope_smoke_ledger& ledger, int initial_value) noexcept
        : ledger_(&ledger),
          value_(initial_value) {
        ++ledger_->constructed;
    }

    /**
     * @brief Record payload destruction in the borrowed ledger.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post `destroyed` has increased by one and `last_destroyed_value` equals
     * this object's final payload value.
     * @invariant C++ invokes this destructor exactly once for each scope-owned
     * object in the passing path.
     * @throws Nothing.
     * @note Ownership/thread-safety: invoked during single-threaded scope
     * teardown.
     */
    ~scope_smoke_payload() noexcept {
        ++ledger_->destroyed;
        ledger_->last_destroyed_value = value_;
    }

    /**
     * @brief Return the current integer payload.
     *
     * @return Current payload value.
     * @pre The object is alive.
     * @post No state is modified.
     * @invariant The value changes only through `set_value`.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only, used on one thread.
     */
    int value() const noexcept {
        return value_;
    }

    /**
     * @brief Replace the current integer payload.
     *
     * @param next_value Value to store.
     * @return Nothing.
     * @pre The object is alive.
     * @post `value()` returns `next_value`.
     * @invariant The mutation does not change the ledger pointer.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutator only, used on one thread.
     */
    void set_value(int next_value) noexcept {
        value_ = next_value;
    }

private:
    scope_smoke_payload(const scope_smoke_payload&) = delete;
    scope_smoke_payload& operator=(const scope_smoke_payload&) = delete;

    /// Non-owning pointer to the ledger that outlives this scope-owned object.
    scope_smoke_ledger* ledger_;
    /// Observable integer payload used by the smoke assertions.
    int value_;
};

/**
 * @brief Exercise `Owner<T>`, `Ref<T>`, and `MutRef<T>` in the active lane.
 *
 * @return Nothing.
 * @pre The memsafe owner header is reachable through the umbrella include.
 * @post Harness failures are recorded for any construction, read-borrow, or
 * mutable-borrow regression.
 * @invariant This flow uses only legal positive borrows, making it safe for the
 * default ABORT policy and for Valgrind memcheck.
 * @throws Unexpected allocation or payload-construction exceptions may escape
 * and fail the executable under the F4 exit-code model.
 * @note Ownership/thread-safety: all owner and borrow handles stay on one
 * thread and end before the owner is destroyed.
 */
void exercise_owner_roster() {
    memsafe::Owner<std::string> owner("cxx17");

    CHECK(owner.has_value());
    CHECK(*owner == "cxx17");

    {
        memsafe::Ref<std::string> read = owner.borrow();
        memsafe::Ref<std::string> copied_read = read;
        CHECK(*read == "cxx17");
        CHECK(*copied_read == "cxx17");
    }

    {
        memsafe::MutRef<std::string> write = owner.borrow_mut();
        *write = "cxx17-owner";
        CHECK(*write == "cxx17-owner");
    }

    CHECK(*owner == "cxx17-owner");
}

/**
 * @brief Exercise `Slot<T>`, `Handle<T>`, and `SlotMap<T>` in C++17-compatible code.
 *
 * @return Nothing.
 * @pre The handle header is reachable through the umbrella include.
 * @post Harness failures are recorded for generation, allocation, lookup, and
 * deallocation regressions.
 * @invariant The direct `Slot<T>` payload is explicitly destroyed before its
 * storage wrapper leaves scope, and every `SlotMap<T>` handle is deallocated
 * before the map is destroyed.
 * @throws Allocation, payload construction, or configured violation-policy
 * exceptions may escape and fail the test process.
 * @note Ownership/thread-safety: all slot and map operations are
 * single-threaded; lock-free contention is covered by separate Slice 2 tests.
 */
void exercise_handle_roster() {
    memsafe::Slot<int> slot;
    const memsafe::Slot<int>::generation_type initial_generation =
        slot.generation();
    int* const direct_value = slot.construct(17);
    CHECK(direct_value != nullptr);
    CHECK(*direct_value == 17);
    slot.destroy();
    const memsafe::Slot<int>::generation_type next_generation =
        slot.bump_generation();
    CHECK(next_generation != initial_generation);
    CHECK(next_generation != memsafe::Slot<int>::invalid_generation);

    memsafe::Handle<int> default_handle;
    CHECK(!default_handle.is_valid());

    memsafe::SlotMap<std::string, 2U> map;
    CHECK(map.capacity() == 2U);
    CHECK(map.size() == 0U);

    memsafe::SlotMap<std::string, 2U>::handle_type first =
        map.allocate("alpha");
    memsafe::SlotMap<std::string, 2U>::handle_type second =
        map.allocate("beta");

    CHECK(first.is_valid());
    CHECK(second.is_valid());
    CHECK(first != second);
    CHECK(map.size() == 2U);
    CHECK(map.get(first) != nullptr);
    CHECK(map.deref(first) == "alpha");
    CHECK(map.deref(second) == "beta");

    map.deref(first) = "gamma";
    CHECK(map.deref(first) == "gamma");

    map.deallocate(second);
    map.deallocate(first);
    CHECK(map.size() == 0U);
}

/**
 * @brief Exercise `Scope` creation and deterministic cleanup.
 *
 * @return Nothing.
 * @pre `scope_smoke_payload` remains nothrow-destructible.
 * @post Harness failures are recorded for allocation, in-scope access, or
 * destructor-count regressions.
 * @invariant The test never touches the raw pointer after `scope` is destroyed;
 * F2 assigns that negative case to sanitizer/memcheck-specific tests.
 * @throws `std::bad_alloc` or payload-construction exceptions may escape as
 * ordinary test failures.
 * @note Ownership/thread-safety: the scope, payload, and ledger are local to
 * one thread.
 */
void exercise_scope_roster() {
    scope_smoke_ledger ledger;

    {
        memsafe::Scope scope;
        CHECK(scope.size() == 0U);

        scope_smoke_payload* const payload =
            scope.create<scope_smoke_payload>(ledger, 31);
        CHECK(payload != nullptr);
        CHECK(scope.size() == 1U);
        CHECK(payload->value() == 31);

        payload->set_value(37);
        CHECK(payload->value() == 37);
        CHECK(ledger.constructed == 1);
        CHECK(ledger.destroyed == 0);
    }

    CHECK(ledger.constructed == 1);
    CHECK(ledger.destroyed == 1);
    CHECK(ledger.last_destroyed_value == 37);
}

/**
 * @brief Exercise `SyncOwner<T>`, `SyncRef<T>`, and `SyncMutRef<T>` on one thread.
 *
 * @return Nothing.
 * @pre The sync header is reachable through the umbrella include.
 * @post Harness failures are recorded for construction, immutable borrowing,
 * mutable borrowing, or final payload observation regressions.
 * @invariant The flow is the legal positive subset of Slice 4; no overlapping
 * conflicting borrows are created, so the default violation policy is safe.
 * @throws Unexpected allocation or payload-construction exceptions may escape.
 * @note Ownership/thread-safety: this smoke is intentionally single-threaded;
 * Relacy and thread interleavings belong to CPP_MEMSAFE-0430-TEST.
 */
void exercise_sync_owner_roster() {
    memsafe::SyncOwner<int> owner(41);

    CHECK(owner.has_value());
    CHECK(*owner == 41);

    {
        memsafe::SyncRef<int> read = owner.borrow();
        memsafe::SyncRef<int> copied_read = read;
        CHECK(*read == 41);
        CHECK(*copied_read == 41);
    }

    {
        memsafe::SyncMutRef<int> write = owner.borrow_mut();
        *write = 43;
        CHECK(*write == 43);
    }

    CHECK(*owner == 43);
}

/**
 * @brief Exercise `Arc<T>` shared ownership with clean final release.
 *
 * @return Nothing.
 * @pre The sync header's `Arc<T>` implementation is reachable through the
 * umbrella include.
 * @post Harness failures are recorded for refcount, shared-payload, or reset
 * regressions.
 * @invariant Copies share one payload and all `Arc<T>` handles release before
 * the function returns, keeping the Valgrind lane leak-free.
 * @throws Allocation or payload-construction exceptions may escape as test
 * failures.
 * @note Ownership/thread-safety: all handle mutations occur on the main thread.
 */
void exercise_arc_roster() {
    memsafe::Arc<std::string> first("arc");
    CHECK(first.has_value());
    CHECK(first.strong_count() == 1U);
    CHECK(first.use_count() == 1U);
    CHECK(*first == "arc");

    {
        memsafe::Arc<std::string> second(first);
        CHECK(first.strong_count() == 2U);
        CHECK(second.strong_count() == 2U);
        CHECK(second.get() == first.get());

        *second = "arc-shared";
        CHECK(*first == "arc-shared");
    }

    CHECK(first.strong_count() == 1U);
    first.reset();
    CHECK(!first.has_value());
    CHECK(first.get() == nullptr);
    CHECK(first.strong_count() == 0U);
}

/**
 * @brief Exercise `Mutex<T>::lock()` and the `MutRef<T>` unlock hook.
 *
 * @return Nothing.
 * @pre The sync header's `Mutex<T>` implementation is reachable through the
 * umbrella include.
 * @post Harness failures are recorded for lock, mutation, or unlock-regression
 * symptoms observable through a second lock.
 * @invariant The first lock handle is destroyed before the second lock is
 * attempted; this proves the C++17 `MutRef<T>` release-hook path unlocks
 * cleanly without requiring worker threads.
 * @throws `std::system_error` from `std::mutex::lock` or unexpected
 * construction exceptions may escape as test failures.
 * @note Ownership/thread-safety: the mutex is local to the main thread in this
 * smoke test; concurrent serialization is covered by CPP_MEMSAFE-0520-TEST and
 * CPP_MEMSAFE-0530-TEST.
 */
void exercise_mutex_roster() {
    memsafe::Mutex<int> mutex(0);

    {
        memsafe::MutRef<int> locked = mutex.lock();
        *locked = 59;
        CHECK(*locked == 59);
    }

    {
        memsafe::MutRef<int> locked_again = mutex.lock();
        CHECK(*locked_again == 59);
        *locked_again += 1;
        CHECK(*locked_again == 60);
    }
}

} // namespace

/**
 * @brief Run the Slice 8 C++17 compatibility smoke test.
 *
 * @retval 0 All compatibility smoke checks passed.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built by the F4 convention-based runtime-test lane
 * with the project include directory and `testing/test_harness.hpp` available.
 * @post The test harness prints a summary and returns the process verdict.
 * @invariant In the `cxx17` and `valgrind-cxx17` lanes, the same source is
 * compiled with `-std=c++17` and `MEMSAFE_CXX17_COMPAT=1`, proving the full
 * roster has no reachable concept or `std::source_location` dependency.
 * @throws Nothing intentionally; unexpected allocation, mutex, or library
 * exceptions are allowed to terminate the process as validation failures.
 * @note Ownership/thread-safety: the test performs no concurrent work and ends
 * every owner, borrow, lock, and shared-reference lifetime before exit.
 */
int main() {
    exercise_owner_roster();
    exercise_handle_roster();
    exercise_scope_roster();
    exercise_sync_owner_roster();
    exercise_arc_roster();
    exercise_mutex_roster();

    RUN_TESTS("test_cxx17_smoke");
}
