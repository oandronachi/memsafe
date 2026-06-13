/**
 * @file ex_scope.cpp
 * @brief Standalone smoke example for the `memsafe::Scope` RAII arena.
 *
 * @details
 * Work package: CPP_MEMSAFE-0900-TEST.
 *
 * Purpose:
 * - Demonstrate the finalized CPP_MEMSAFE-0300-FUNC `Scope::create<T>` API in
 *   a self-contained example executable.
 * - Show that raw pointers returned by `create<T>` are usable while the owning
 *   `Scope` is alive and that scope destruction releases owned objects.
 * - Return an ordinary process exit code for the F4 `examples/*.cpp` smoke
 *   discovery path.
 *
 * Key invariants:
 * - Each successful `create<tracked_value>` call appends one scope-owned object
 *   to the arena's private ledger.
 * - The example observes scope-owned raw pointers only before the `Scope`
 *   object is destroyed; post-scope raw-pointer access is a sanitizer-only
 *   negative test in the Slice 3 test plan, not a valid smoke path.
 * - The tracked payload destructor is `noexcept`, satisfying
 *   `Scope::create<T>`'s accepted object-type requirements.
 *
 * Ownership and thread-safety:
 * - `Scope` owns every `tracked_value` it creates.
 * - Returned raw pointers are non-owning, lifetime-bounded by the scope, and
 *   used on one thread in this example.
 */
#include <memsafe/scope.hpp>

#include <cstddef>

namespace {

/**
 * @brief Scope-owned payload that records live-object count changes.
 *
 * @pre The integer counter supplied to the constructor must outlive the
 * `tracked_value` instance.
 * @post Construction increments the counter and destruction decrements it.
 * @invariant `live_count_` points to the same external counter for the entire
 * object lifetime.
 * @throws Nothing; construction, mutation, observation, and destruction are
 * all `noexcept`.
 * @note Ownership/thread-safety: instances are created and destroyed by
 * `memsafe::Scope` in this example, and the borrowed counter is touched on one
 * thread only.
 *
 * Example:
 * @code
 * int live = 0;
 * memsafe::Scope scope;
 * tracked_value* value = scope.create<tracked_value>(live, 7);
 * value->add(1);
 * @endcode
 */
class tracked_value final {
public:
    /**
     * @brief Construct a tracked value and record that it is live.
     *
     * @param live_count Borrowed counter updated by construction and
     * destruction.
     * @param initial_value Initial integer payload.
     * @return Constructors do not return a value.
     * @pre `live_count` outlives this object.
     * @post `live_count` has increased by one and `value()` returns
     * `initial_value`.
     * @invariant The object never owns the counter; it only records lifetime
     * transitions into caller-owned instrumentation.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: the constructed object is intended to be
     * owned by one `memsafe::Scope`.
     */
    tracked_value(int& live_count, int initial_value) noexcept
        : live_count_(&live_count), value_(initial_value) {
        ++(*live_count_);
    }

    /**
     * @brief Record that this tracked value is no longer live.
     *
     * @return Destructors do not return a value.
     * @pre `live_count_` points to a live counter.
     * @post The counter has decreased by one.
     * @invariant Scope destruction calls this destructor exactly once for each
     * successfully created payload.
     * @throws Nothing; this destructor is `noexcept`.
     * @note Ownership/thread-safety: invoked by `memsafe::Scope` while the
     * example has no concurrent access to the payload.
     */
    ~tracked_value() noexcept {
        --(*live_count_);
    }

    /**
     * @brief Return the current integer payload.
     *
     * @return Current payload value.
     * @pre The object is alive.
     * @post No state is modified.
     * @invariant The value changes only through `add`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned scalar is observed while the
     * owning `Scope` is alive.
     */
    int value() const noexcept {
        return value_;
    }

    /**
     * @brief Add a delta to the payload.
     *
     * @param delta Amount to add to the current payload.
     * @return Nothing.
     * @pre The object is alive and addition does not overflow for the example
     * values.
     * @post `value()` returns the previous value plus `delta`.
     * @invariant Mutation affects only this object's payload, not the scope
     * ledger or the live-object counter.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: called on one thread while the owning
     * `Scope` is alive.
     */
    void add(int delta) noexcept {
        value_ += delta;
    }

private:
    tracked_value(const tracked_value& other) = delete;
    tracked_value& operator=(const tracked_value& other) = delete;

    int* live_count_;
    int value_;
};

} // namespace

/**
 * @brief Run the scope smoke example.
 *
 * @retval 0 The example created, used, and destroyed scope-owned objects
 * successfully.
 * @retval 1 A new scope did not report an empty arena.
 * @retval 2 One of the returned raw pointers was unexpectedly null.
 * @retval 3 The scope size or live-object count did not match the two
 * allocations.
 * @retval 4 In-scope raw-pointer access did not observe the expected values.
 * @retval 5 In-scope mutation through returned raw pointers did not persist.
 * @retval 6 Scope destruction did not release all tracked objects.
 * @pre The executable is built with the project `include/` directory on the
 * compiler include path and a C++17-or-newer compiler.
 * @post The local `Scope` has been destroyed before the final live-count check.
 * @invariant No raw pointer returned by `Scope::create<T>` is accessed after
 * the owning scope leaves its lexical block.
 * @throws The example does not intentionally throw. Allocation failure,
 * payload construction failure, or an unexpected library exception is allowed
 * to terminate the smoke executable under the runner's exit-code model.
 * @note Ownership/thread-safety: the scope and returned raw pointers are local
 * to `main` and are used on a single thread.
 */
int main() {
    int live_objects = 0;

    {
        memsafe::Scope scope;
        if (scope.size() != 0U) {
            return 1;
        }

        tracked_value* const first = scope.create<tracked_value>(live_objects, 10);
        tracked_value* const second = scope.create<tracked_value>(live_objects, 20);

        if (first == nullptr || second == nullptr) {
            return 2;
        }

        if (scope.size() != 2U || live_objects != 2) {
            return 3;
        }

        if (first->value() != 10 || second->value() != 20) {
            return 4;
        }

        first->add(1);
        second->add(2);

        if (first->value() != 11 || second->value() != 22) {
            return 5;
        }

        /*
         * F2 marks stale `Scope::create` raw-pointer access as sanitizer-only,
         * so the example deliberately stops using `first` and `second` before
         * the owning scope is destroyed.
         */
    }

    if (live_objects != 0) {
        return 6;
    }

    return 0;
}
