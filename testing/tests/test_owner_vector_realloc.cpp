/**
 * @file test_owner_vector_realloc.cpp
 * @brief Runtime vector-reallocation test for Slice 1 `memsafe::Owner<T>`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0150-TEST.
 *
 * Purpose:
 * - Verify that `memsafe::Owner<T>` can be inserted into `std::vector`.
 * - Force `std::vector` capacity growth with `reserve()` after an owner is
 *   already present, exercising the standard-library reallocation path that
 *   moves existing elements.
 * - Assert the F3 `Move_Noexcept` property with
 *   `std::is_nothrow_move_constructible_v<memsafe::Owner<T>>`, matching the
 *   package oracle: this test proves "compiles plus nothrow move" rather than
 *   relying on an implementation-dependent failure when a move is not
 *   `noexcept`.
 *
 * Key invariants:
 * - `owner_type` is move-only, so any attempted vector copy of an existing
 *   owner would be rejected at compile time.
 * - `reserve(old_capacity + N)` is called with a value greater than the
 *   current capacity; the C++ vector contract therefore reallocates storage for
 *   the existing `Owner<T>` element before the test continues.
 * - The payload object is intentionally non-copyable and non-movable. A
 *   successful reallocation therefore demonstrates that only the owner handle
 *   moves and the owned heap allocation remains alive.
 *
 * Ownership and thread-safety:
 * - Every `memsafe::Owner<reallocation_payload>` uniquely owns one heap
 *   payload; raw payload pointers captured by the test are non-owning
 *   observations used only while the owning vector remains alive.
 * - The test is single-threaded and does not exercise `SyncOwner<T>` or any
 *   cross-thread borrow behavior.
 */

#include "../test_harness.hpp"

#include <memsafe/owner.hpp>

#include <cstddef>
#include <type_traits>
#include <vector>

namespace {

/**
 * @brief Stack-owned ledger for payload construction and destruction counts.
 *
 * @pre The ledger must outlive every `reallocation_payload` that stores its
 * address.
 * @post Payload construction increments `constructed`; payload destruction
 * increments `destroyed`.
 * @invariant `destroyed` must never exceed `constructed` on the successful
 * test path.
 * @throws This aggregate owns no resources and throws nothing.
 * @note Ownership/thread-safety: the ledger is owned by the test stack frame
 * and touched only by the single runtime-test thread.
 */
struct reallocation_ledger final {
    /// Number of payload constructors that completed.
    int constructed = 0;
    /// Number of payload destructors observed.
    int destroyed = 0;
};

/**
 * @brief Non-copyable, non-movable payload owned by `memsafe::Owner<T>`.
 *
 * @details
 * The payload cannot be copied or moved, which keeps this test focused on the
 * owner wrapper's move behavior under `std::vector` reallocation. Moving an
 * `Owner<reallocation_payload>` transfers the owner control block; it does not
 * move the `reallocation_payload` object itself.
 *
 * Example:
 * @code
 * reallocation_ledger ledger;
 * memsafe::Owner<reallocation_payload> owner(ledger, 7);
 * CHECK(owner->id() == 7);
 * @endcode
 *
 * @pre The referenced `reallocation_ledger` outlives this payload.
 * @post Construction records one live payload and stores the supplied id.
 * @invariant `ledger_` remains the construction-supplied ledger pointer for
 * the payload lifetime, and `id_` remains stable across owner moves.
 * @throws Nothing; construction, destruction, and observation are `noexcept`.
 * @note Ownership/thread-safety: instances are uniquely owned by
 * `memsafe::Owner<reallocation_payload>` and used on one thread.
 */
class reallocation_payload final {
public:
    /**
     * @brief Construct a payload and record it in the supplied ledger.
     *
     * @param ledger Ledger that receives construction and destruction counts.
     * @param id Stable identifier used to verify payload identity after vector
     * reallocation.
     * @return Constructors do not return a value.
     * @pre `ledger` outlives this payload.
     * @post `ledger.constructed` is incremented and `id()` returns `id`.
     * @invariant The payload stores only a borrowed ledger pointer and a stable
     * integer identifier.
     * @throws Nothing.
     * @note Ownership/thread-safety: the constructed object is intended to be
     * uniquely owned by one `memsafe::Owner<reallocation_payload>`.
     */
    reallocation_payload(reallocation_ledger& ledger, int id) noexcept
        : ledger_(&ledger), id_(id) {
        ++ledger_->constructed;
    }

    /**
     * @brief Copy construction is disabled for the payload probe.
     *
     * @param other Source payload that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying is a compile-time error.
     * @post No duplicate ledger entry or payload identity is created.
     * @invariant The vector reallocation test must not be able to pass by
     * copying payload objects.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: deleting copies preserves unique payload
     * identity inside its owning `memsafe::Owner<T>`.
     */
    reallocation_payload(const reallocation_payload& other) = delete;

    /**
     * @brief Copy assignment is disabled for the payload probe.
     *
     * @param other Source payload that would otherwise replace this payload.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post The stored ledger pointer and payload id are never overwritten by a
     * copy operation.
     * @invariant Payload identity remains fixed for the object lifetime.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: deleting copy assignment avoids duplicate
     * ownership-observation records.
     */
    reallocation_payload& operator=(const reallocation_payload& other) = delete;

    /**
     * @brief Move construction is disabled for the payload probe.
     *
     * @param other Source payload that would otherwise be moved.
     * @return No value; this overload is deleted.
     * @pre Not available. Moving the payload is a compile-time error.
     * @post No payload object is relocated by test code.
     * @invariant Vector reallocation must move `Owner<T>` wrappers only; the
     * heap payload address remains stable.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: deleting payload moves makes accidental
     * direct payload relocation impossible in this single-threaded test.
     */
    reallocation_payload(reallocation_payload&& other) = delete;

    /**
     * @brief Move assignment is disabled for the payload probe.
     *
     * @param other Source payload that would otherwise replace this payload.
     * @return No value; this overload is deleted.
     * @pre Not available. Move assignment is a compile-time error.
     * @post The payload id and ledger pointer remain immutable after
     * construction.
     * @invariant Owner movement transfers the owner control block without
     * assigning the contained payload.
     * @throws Nothing at runtime because the function is deleted.
     * @note Ownership/thread-safety: deleting move assignment keeps payload
     * identity stable for pointer checks after vector reallocation.
     */
    reallocation_payload& operator=(reallocation_payload&& other) = delete;

    /**
     * @brief Record payload destruction.
     *
     * @return Destructors do not return a value.
     * @pre `ledger_` points to the live ledger supplied at construction.
     * @post `ledger_->destroyed` is incremented exactly once for this payload.
     * @invariant Destruction is balanced with one completed construction.
     * @throws Nothing.
     * @note Ownership/thread-safety: called during single-threaded owner/vector
     * teardown.
     */
    ~reallocation_payload() noexcept {
        ++ledger_->destroyed;
    }

    /**
     * @brief Return the stable payload identifier.
     *
     * @return Identifier supplied at construction.
     * @pre The payload is alive.
     * @post The payload is unchanged.
     * @invariant The returned id is not modified by owner moves or vector
     * reallocation.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; the caller must respect the
     * owning `memsafe::Owner<reallocation_payload>` lifetime.
     */
    [[nodiscard]] int id() const noexcept {
        return id_;
    }

private:
    /// Non-owning pointer to the stack ledger that outlives this payload.
    reallocation_ledger* ledger_;
    /// Stable value used to verify payload identity through owner moves.
    int id_;
};

/**
 * @brief Concrete owner instantiation exercised by this test.
 *
 * @pre `reallocation_payload` is complete before the alias is used in trait
 * checks or vector storage.
 * @post No object is created by this alias declaration.
 * @invariant The aliased owner remains move-only and nothrow-move-constructible
 * for the `std::vector` reserve-growth oracle.
 * @throws Nothing; a type alias has no runtime behavior.
 * @note Ownership/thread-safety: objects of this aliased type uniquely own
 * single-threaded payloads in this test.
 */
using owner_type = memsafe::Owner<reallocation_payload>;

static_assert(std::is_nothrow_move_constructible_v<owner_type>,
              "Owner<T> must be nothrow move-constructible so std::vector "
              "reallocation can move existing owners");

static_assert(!std::is_copy_constructible_v<owner_type>,
              "Owner<T> must remain move-only; a vector copy fallback would "
              "not satisfy the ownership contract");

/**
 * @brief Verify owner insertion and reserve-growth reallocation in a vector.
 *
 * @return Nothing.
 * @pre The test executable is compiled with the project include directory and
 * a standards-conforming C++20 library implementation.
 * @post The harness records failures for any vector size/capacity mismatch,
 * payload identity loss, or destruction-count imbalance.
 * @invariant Calling `reserve()` with a larger capacity after the first
 * insertion exercises the vector reallocation path named by F2 Ownership case
 * 3 and the F3 `Move_Noexcept` property.
 * @throws Nothing intentionally; unexpected allocation or library exceptions
 * are allowed to terminate the process under the runner's exit-code model.
 * @note Ownership/thread-safety: owners, payloads, and the ledger are local to
 * this function and accessed on one thread.
 */
void verify_owner_vector_reallocation() {
    reallocation_ledger ledger;

    {
        std::vector<owner_type> owners;
        owners.reserve(1U);

        CHECK(owners.empty());
        CHECK(owners.capacity() >= 1U);

        owners.push_back(owner_type(ledger, 11));
        CHECK(owners.size() == 1U);
        CHECK(owners.front().has_value());
        CHECK(owners.front()->id() == 11);

        reallocation_payload* const first_payload = owners.front().operator->();
        const std::size_t before_capacity = owners.capacity();

        /*
         * The package acceptance specifically asks for reserve growth. Passing
         * a value greater than the current capacity forces vector storage
         * reallocation, which must move the existing move-only Owner<T>.
         */
        owners.reserve(before_capacity + 2U);

        CHECK(owners.capacity() >= before_capacity + 2U);
        CHECK(owners.size() == 1U);
        CHECK(owners.front().has_value());
        CHECK(owners.front()->id() == 11);
        CHECK(owners.front().operator->() == first_payload);

        owners.emplace_back(ledger, 22);
        owners.emplace_back(ledger, 33);

        CHECK(owners.size() == 3U);
        CHECK(owners[0]->id() == 11);
        CHECK(owners[1]->id() == 22);
        CHECK(owners[2]->id() == 33);
        CHECK(ledger.constructed == 3);
        CHECK(ledger.destroyed == 0);
    }

    CHECK(ledger.constructed == 3);
    CHECK(ledger.destroyed == 3);
}

} // namespace

/**
 * @brief Run the owner vector-reallocation test.
 *
 * @return Zero when `Owner<T>` compiles as nothrow-move-constructible and all
 * runtime vector reallocation checks pass; nonzero when the harness records a
 * failed check.
 * @pre The executable is built as a standalone F4 runtime test with
 * `testing/test_harness.hpp` and `<memsafe/owner.hpp>` available.
 * @post The test harness prints a summary and returns its process verdict.
 * @invariant Runtime success means a `std::vector<Owner<T>>` stored an owner,
 * grew reserved capacity, moved the existing owner under reallocation, and
 * preserved payload ownership until vector teardown.
 * @throws Nothing intentionally; unexpected exceptions surface as process
 * failures under the runner's exit-code model.
 * @note Ownership/thread-safety: the test starts no worker threads and uses
 * only local owner objects.
 */
int main() {
    verify_owner_vector_reallocation();

    RUN_TESTS("test_owner_vector_realloc");
}
