/**
 * @file test_scope.cpp
 * @brief Runtime unit test for `memsafe::Scope` lifetime ownership.
 *
 * @details
 * Work package: CPP_MEMSAFE-0310-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0300-FUNC `memsafe::Scope` artifact.
 * - Create several scope-owned objects, prove their returned raw pointers are
 *   usable while the owning scope is alive, and then verify every destructor is
 *   invoked exactly once when the scope exits.
 * - Keep this test to the non-sanitizer lifetime oracle from F2
 *   `Scope and Concurrency` case 1. The sanitizer-only post-scope raw-pointer
 *   access check is intentionally left to CPP_MEMSAFE-0320-TEST.
 *
 * Key invariants:
 * - Each successful `Scope::create<tracked_object>` call increments the
 *   construction counter exactly once.
 * - No destructor runs before the owning `memsafe::Scope` leaves its lexical
 *   block.
 * - After scope destruction, the total destruction count equals the total
 *   construction count and each tracked object id has exactly one destructor
 *   hit.
 * - Destruction order is observed as last-in, first-out, matching the Slice 3
 *   Scope requirement and the CPP_MEMSAFE-0300-FUNC artifact contract.
 *
 * Ownership and thread-safety:
 * - All instrumentation state is stack-owned by `main`.
 * - The test is single-threaded; no synchronization is required or provided.
 */
#include "../test_harness.hpp"

#include <memsafe/scope.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace {

/**
 * @brief Mutable instrumentation ledger for the scope lifetime test.
 *
 * @details
 * The ledger is intentionally separate from the objects owned by
 * `memsafe::Scope` so it remains alive after scope destruction and can be used
 * to assert destructor behavior. It records aggregate construction/destruction
 * counts plus per-object id hits and the observed destruction sequence.
 *
 * @pre The ledger must outlive every `tracked_object` that points at it.
 * @post Direct field updates reflect object construction and destruction events.
 * @invariant `destroyed_by_id[id]` is the number of destructor calls observed
 * for the object id represented by `id`; `destruction_order[n]` is the nth
 * destructor id recorded, bounded by the array size.
 * @throws This aggregate does not allocate or throw.
 * @note Ownership/thread-safety: owned by the test stack frame and accessed
 * only by the single runtime-test thread.
 */
struct lifetime_counters final {
    /// Number of tracked objects whose constructors completed successfully.
    int constructed = 0;
    /// Number of tracked object destructors observed so far.
    int destroyed = 0;
    /// Per-id destructor call counts; ids 1, 2, and 3 are used by this test.
    std::array<int, 4> destroyed_by_id{};
    /// Destructor ids in observed order; index zero is the first destructor run.
    std::array<int, 4> destruction_order{};
};

/**
 * @brief Scope-owned object that records lifetime events in a shared ledger.
 *
 * @details
 * `tracked_object` supplies visible fields through accessors so the test can
 * prove every object returned by `Scope::create` is accessible while the scope
 * is alive. Its destructor is `noexcept`, satisfying `memsafe::Scope::create`
 * requirements and making destructor counts deterministic for this test.
 *
 * Example:
 * @code
 * lifetime_counters counters;
 * memsafe::Scope scope;
 * tracked_object* object = scope.create<tracked_object>(counters, 1, 10, "first");
 * object->add_to_value(1);
 * @endcode
 *
 * @pre The referenced `lifetime_counters` object outlives this object. The
 * supplied name pointer is non-null.
 * @post Construction increments `lifetime_counters::constructed`; destruction
 * increments exactly one aggregate count and one per-id count.
 * @invariant `counters_` always points to the shared ledger supplied at
 * construction, and `id_` remains stable for the lifetime of the object.
 * @throws Construction may throw if `std::string` allocation fails. Destruction
 * and all observers/mutators are non-throwing.
 * @note Ownership/thread-safety: instances are owned solely by `memsafe::Scope`;
 * returned raw pointers are used only within the owning scope and on one thread.
 */
class tracked_object final {
public:
    /**
     * @brief Construct a tracked object and record successful construction.
     *
     * @param counters Shared instrumentation ledger that must outlive this
     * object.
     * @param object_id Stable logical id used for per-object destructor checks.
     * @param object_value Initial integer payload used to prove accessibility.
     * @param object_name Non-null name payload used to prove string members are
     * accessible while the scope is alive.
     * @return Constructors do not return a value.
     * @pre `object_name != nullptr`, and `counters` remains alive until this
     * object is destroyed by its owning `memsafe::Scope`.
     * @post `constructed` has increased by one after all members are
     * initialized successfully.
     * @invariant The stored counter pointer and object id are stable for the
     * object's lifetime.
     * @throws `std::bad_alloc` or another exception propagated by
     * `std::string` construction.
     * @note Ownership/thread-safety: the constructed object is scope-owned and
     * is not safe to access concurrently without external synchronization.
     */
    tracked_object(lifetime_counters& counters,
                   int object_id,
                   int object_value,
                   const char* object_name)
        : counters_(&counters),
          id_(object_id),
          value_(object_value),
          name_(object_name) {
        ++counters_->constructed;
    }

    /**
     * @brief Record a single destructor event for this object's id.
     *
     * @return Destructors do not return a value.
     * @pre `counters_` points to a live `lifetime_counters` object.
     * @post The aggregate destruction count and this object's per-id count have
     * each increased by one; the object's id is appended to the observed
     * destruction order if capacity remains.
     * @invariant A destructor call records only this object's stable `id_`.
     * @throws Nothing.
     * @note Ownership/thread-safety: invoked by `memsafe::Scope` during
     * single-threaded scope teardown in this test.
     */
    ~tracked_object() noexcept {
        const std::size_t slot = static_cast<std::size_t>(counters_->destroyed);
        if (slot < counters_->destruction_order.size()) {
            counters_->destruction_order[slot] = id_;
        }

        ++counters_->destroyed;

        if (id_ >= 0 &&
            static_cast<std::size_t>(id_) < counters_->destroyed_by_id.size()) {
            ++counters_->destroyed_by_id[static_cast<std::size_t>(id_)];
        }
    }

    /**
     * @brief Return this object's stable logical id.
     *
     * @return Object id supplied at construction.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The returned id does not change during the object's lifetime.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; callers must still respect
     * the owning scope lifetime for the raw pointer used to call it.
     */
    [[nodiscard]] int id() const noexcept {
        return id_;
    }

    /**
     * @brief Return the current integer payload.
     *
     * @return Current payload value.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The value changes only through `add_to_value`.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only and unsynchronized.
     */
    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    /**
     * @brief Return the current string payload.
     *
     * @return Const reference to the name stored by this object.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant The name is set at construction and never mutated.
     * @throws Nothing.
     * @note Ownership/thread-safety: the returned reference is bounded by this
     * object's lifetime, which is bounded by the owning `memsafe::Scope`.
     */
    [[nodiscard]] const std::string& name() const noexcept {
        return name_;
    }

    /**
     * @brief Add a delta to the integer payload.
     *
     * @param delta Amount to add to the current payload.
     * @return No value.
     * @pre The object is alive and integer addition stays within the range of
     * `int` for this test input.
     * @post `value() == old value() + delta`.
     * @invariant The mutation does not affect lifetime counters or object id.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutator only and unsynchronized; this test
     * calls it from a single thread while the owning scope is alive.
     */
    void add_to_value(int delta) noexcept {
        value_ += delta;
    }

private:
    tracked_object(const tracked_object&) = delete;
    tracked_object& operator=(const tracked_object&) = delete;

    lifetime_counters* counters_;
    int id_;
    int value_;
    std::string name_;
};

} // namespace

/**
 * @brief Run the Scope lifetime/destructor-once unit test.
 *
 * @retval 0 All checks passed and `memsafe::Scope` satisfied the lifetime
 * contract.
 * @retval 1 One or more harness checks failed.
 * @pre The program is run as a standalone executable by the F4 convention-based
 * CTest harness.
 * @post The process exit code is the test verdict; no global state is retained.
 * @invariant The test never accesses any `Scope::create` raw pointer after its
 * owning scope has been destroyed.
 * @throws The test does not intentionally throw. Unexpected allocation or
 * construction failures escape to the C++ runtime and fail the executable.
 * @note Ownership/thread-safety: all state is automatic storage in this
 * function and all operations are single-threaded.
 */
int main() {
    lifetime_counters counters;

    {
        memsafe::Scope scope;
        CHECK(scope.size() == 0U);

        tracked_object* const first =
            scope.create<tracked_object>(counters, 1, 10, "first");
        tracked_object* const second =
            scope.create<tracked_object>(counters, 2, 20, "second");
        tracked_object* const third =
            scope.create<tracked_object>(counters, 3, 30, "third");

        CHECK(scope.size() == 3U);
        CHECK(counters.constructed == 3);
        CHECK(counters.destroyed == 0);

        CHECK(first != nullptr);
        CHECK(second != nullptr);
        CHECK(third != nullptr);
        CHECK(first != second);
        CHECK(second != third);
        CHECK(first != third);

        CHECK(first->id() == 1);
        CHECK(first->value() == 10);
        CHECK(first->name() == "first");

        CHECK(second->id() == 2);
        CHECK(second->value() == 20);
        CHECK(second->name() == "second");

        CHECK(third->id() == 3);
        CHECK(third->value() == 30);
        CHECK(third->name() == "third");

        /*
         * F2 scope lifetime case 1 requires all objects returned by create() to
         * remain accessible while the owning scope is alive. Mutating after all
         * three allocations also guards against an implementation that
         * accidentally invalidates earlier raw pointers during later creates.
         */
        first->add_to_value(1);
        second->add_to_value(2);
        third->add_to_value(3);

        CHECK(first->value() == 11);
        CHECK(second->value() == 22);
        CHECK(third->value() == 33);
        CHECK(counters.destroyed == 0);
    }

    CHECK(counters.constructed == 3);
    CHECK(counters.destroyed == 3);
    CHECK(counters.destroyed_by_id[1] == 1);
    CHECK(counters.destroyed_by_id[2] == 1);
    CHECK(counters.destroyed_by_id[3] == 1);

    CHECK(counters.destruction_order[0] == 3);
    CHECK(counters.destruction_order[1] == 2);
    CHECK(counters.destruction_order[2] == 1);

    RUN_TESTS("test_scope");
}
