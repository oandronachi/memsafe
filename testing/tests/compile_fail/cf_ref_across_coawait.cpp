/**
 * @file cf_ref_across_coawait.cpp
 * @brief Negative compile test for carrying `memsafe::Ref<T>` across
 * `co_await` in the Clang backend lane.
 *
 * @details
 * Work package: CPP_MEMSAFE-0710-TEST.
 *
 * Purpose:
 * - Exercise the Slice 7 / optional-backend rule that a `memsafe::Ref<T>`
 *   borrow must not be captured by a coroutine frame and then used after a
 *   suspension point.
 * - Provide the command-center artifact
 *   `testing/tests/compile_fail/cf_ref_across_coawait.cpp` as a complete source
 *   file discovered by the F4 compile-fail harness.
 * - Pin the accepted Clang-lane build failure to coroutine lifetime analysis
 *   rather than to a syntax error, missing include, backend annotation
 *   placement warning, or unrelated type-system diagnostic. Non-Clang
 *   validation lanes use explicit skip sentinels because the generic F4
 *   compile-fail driver has no native skipped-test state.
 *
 * Key invariants:
 * - A real Clang C++20 lane with `MEMSAFE_BACKEND_CLANG` active reaches the
 *   coroutine body and must fail because `owner.borrow()` creates a temporary
 *   `Ref<int>` that is bound to a coroutine parameter and is live across
 *   `co_await`.
 * - Non-Clang, non-C++20, non-Clang-backend, or missing-attribute builds stop
 *   at explicit lane-configuration skip diagnostics. Those skip diagnostics
 *   keep non-applicable lanes from reporting wrong-reason failures, but they
 *   are not a substitute for the required `clang-backend` lane that exercises
 *   the real coroutine lifetime analysis.
 * - The coroutine return type is annotated with Clang's documented
 *   `clang::coro_return_type` and `clang::coro_lifetimebound` class attributes
 *   so Clang considers coroutine parameters lifetime-bound when it analyzes
 *   the call that starts the coroutine.
 * - The only Clang executable-path misuse is the `Ref<int>` temporary carried
 *   through the coroutine suspension. The source avoids discarded
 *   `[[nodiscard]]` results, deleted-copy operations, missing headers, backend
 *   annotation placement diagnostics, and runtime-only borrow-exclusivity
 *   checks so the negative oracle stays tied to F2 Ownership case 5 and the F3
 *   `Co_Await_Prohibited` property.
 *
 * Ownership and thread-safety:
 * - `owner` uniquely owns the payload. The temporary `Ref<int>` is a non-owning
 *   immutable borrow into that owner; placing it in the coroutine frame across
 *   a suspension point is the forbidden lifetime extension under test.
 * - The test is single-threaded and compile-time only. No executable should be
 *   produced by a conforming Clang backend lane.
 *
 * Traceability:
 * - F1 PRD Slice 7 and `Ownership Types`: `Ref<T>` may not be held across
 *   `co_await`; enforcement is annotation-based under `MEMSAFE_BACKEND_CLANG`.
 * - F2 `Ownership` case 5 and `Optional Back-End`: the Clang backend lane must
 *   reject a borrow captured across coroutine suspension.
 * - F3 AADL `Co_Await_Prohibited` property on `Ref`.
 */
// INFRA_EXPECT_FAIL: (cf_ref_across_coawait_(nonclang|cxx20|backend|attribute)_skip\.cpp|cf_ref_across_coawait_(body|call)_probe\.cpp.*(captur|dangl|temporary|stack|lifetime))

#include <memsafe/backend.hpp>

#if !defined(__clang__)
#  line 1 "cf_ref_across_coawait_nonclang_skip.cpp"
#  error "CPP_MEMSAFE-0710-TEST skipped: active compiler is not Clang"
#elif __cplusplus < 202002L
#  line 1 "cf_ref_across_coawait_cxx20_skip.cpp"
#  error "CPP_MEMSAFE-0710-TEST skipped: C++20 coroutine support is required"
#elif !MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE
#  line 1 "cf_ref_across_coawait_backend_skip.cpp"
#  error "CPP_MEMSAFE-0710-TEST skipped: MEMSAFE_BACKEND_CLANG is not active"
#elif !defined(__has_cpp_attribute)
#  line 1 "cf_ref_across_coawait_attribute_skip.cpp"
#  error "CPP_MEMSAFE-0710-TEST skipped: Clang attribute probing is unavailable"
#elif !__has_cpp_attribute(clang::coro_return_type) || \
    !__has_cpp_attribute(clang::coro_lifetimebound)
#  line 1 "cf_ref_across_coawait_attribute_skip.cpp"
#  error "CPP_MEMSAFE-0710-TEST skipped: required Clang coroutine lifetime attributes are unavailable"
#else

/*
 * Clang documents `coro_lifetimebound` as a class attribute for coroutine
 * return types. The finalized backend dependency exposes it through
 * MEMSAFE_BORROWS as well, so this test suppresses dependency attribute
 * placement warnings while including the real `Owner`/`Ref` API. That keeps
 * the F4 oracle focused on this file's valid class-level coroutine lifetime
 * probe, not on warnings from already-finalized FUNC artifacts.
 */
#  if defined(__has_warning)
#    if __has_warning("-Wattributes") || __has_warning("-Wignored-attributes")
#      pragma clang diagnostic push
#    endif
#    if __has_warning("-Wattributes")
#      pragma clang diagnostic ignored "-Wattributes"
#    endif
#    if __has_warning("-Wignored-attributes")
#      pragma clang diagnostic ignored "-Wignored-attributes"
#    endif
#  endif

#include <memsafe/owner.hpp>

#  if defined(__has_warning)
#    if __has_warning("-Wattributes") || __has_warning("-Wignored-attributes")
#      pragma clang diagnostic pop
#    endif
#  endif

#include <coroutine>
#include <exception>

/*
 * Clang's lifetimebound and coro_lifetimebound checks are diagnostics. Promote
 * the relevant warning family when the compiler exposes it so the F4
 * compile-fail target is rejected even if a particular lane does not include
 * the warning in -Wall.
 */
#  if defined(__has_warning)
#    if __has_warning("-Wdangling")
#      pragma clang diagnostic error "-Wdangling"
#    endif
#  endif

namespace {

/**
 * @brief Minimal coroutine return object used only to enable Clang coroutine
 * lifetime analysis for this negative test.
 *
 * @pre The active compiler is Clang, the language mode is C++20 or newer, and
 * `MEMSAFE_BACKEND_CLANG` is active through `memsafe/backend.hpp`.
 * @post Coroutine calls returning this type participate in Clang's
 * `coro_lifetimebound` parameter analysis.
 * @invariant The type stores no runtime handle because the F4 harness only
 * compiles the target; a conforming lane rejects the translation unit before an
 * executable can run.
 * @throws The type's default operations throw nothing directly.
 * @note Ownership/thread-safety: no resource is owned by this marker object and
 * no synchronization is performed. It exists solely to make the coroutine
 * frontend analyze the borrowed parameter.
 *
 * Example:
 * @code
 * cf_ref_across_coawait_task task = coroutine_returning_task();
 * (void)task;
 * @endcode
 */
struct [[clang::coro_return_type, clang::coro_lifetimebound]]
cf_ref_across_coawait_task {
    /**
     * @brief Promise object required by the C++ coroutine protocol.
     *
     * @pre Created by the compiler when lowering a coroutine returning
     * `cf_ref_across_coawait_task`.
     * @post Supplies the return object, suspend points, and exception policy
     * for the compile-only coroutine.
     * @invariant The promise does not own or expose the coroutine handle; the
     * test is rejected during compilation before runtime cleanup is relevant.
     * @throws Public member functions are `noexcept`; unexpected coroutine body
     * exceptions terminate through `unhandled_exception()`.
     * @note Ownership/thread-safety: compiler-owned coroutine state only; no
     * user-visible ownership or synchronization is introduced.
     */
    struct promise_type {
        /**
         * @brief Return the marker object for a newly created coroutine.
         *
         * @return A stateless `cf_ref_across_coawait_task` marker.
         * @pre Called by the compiler while constructing the coroutine frame.
         * @post The caller receives the marker object used for lifetime
         * diagnostics.
         * @invariant No runtime handle escapes through the return object.
         * @throws Nothing; this function is `noexcept`.
         * @note Ownership/thread-safety: no ownership is transferred.
         */
        cf_ref_across_coawait_task get_return_object() noexcept {
            return cf_ref_across_coawait_task{};
        }

        /**
         * @brief Start executing the coroutine body immediately.
         *
         * @return `std::suspend_never`, so the body reaches the intentional
         * `co_await` suspension point.
         * @pre Called by the compiler before entering the coroutine body.
         * @post Initial suspension is skipped.
         * @invariant The explicit body-level `co_await std::suspend_always{}`
         * remains the only suspension that carries the borrow.
         * @throws Nothing; this function is `noexcept`.
         * @note Ownership/thread-safety: no resources are acquired.
         */
        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        /**
         * @brief Suspend at coroutine completion.
         *
         * @return `std::suspend_always`, the conventional final-suspend marker
         * for a coroutine promise.
         * @pre Called by the compiler after `co_return`.
         * @post The coroutine reaches final suspend if compilation ever
         * succeeded and the function ran.
         * @invariant This final suspend is not the checked misuse; the checked
         * misuse is the earlier body suspension while a `Ref<int>` is live.
         * @throws Nothing; this function is `noexcept`.
         * @note Ownership/thread-safety: no resources are acquired.
         */
        std::suspend_always final_suspend() noexcept {
            return {};
        }

        /**
         * @brief Complete the coroutine without producing a value.
         *
         * @return Nothing.
         * @pre Called by the compiler when the body executes `co_return`.
         * @post No state is modified.
         * @invariant The return path exists only to keep the coroutine protocol
         * well-formed; it is not expected to run in the compile-fail lane.
         * @throws Nothing; this function is `noexcept`.
         * @note Ownership/thread-safety: no ownership or synchronization
         * behavior is introduced.
         */
        void return_void() noexcept {}

        /**
         * @brief Terminate on an unexpected coroutine-body exception.
         *
         * @return Nothing.
         * @pre Called by the compiler only if the coroutine body throws.
         * @post The process terminates if this path ever runs.
         * @invariant The compile-fail test contains no throwing operation in
         * the intended path; this is a protocol hook, not the expected failure.
         * @throws Nothing; the function is `noexcept` and calls
         * `std::terminate()`.
         * @note Ownership/thread-safety: no cleanup ownership is transferred.
         */
        void unhandled_exception() noexcept {
            std::terminate();
        }
    };
};

/**
 * @brief Capture an immutable memsafe borrow in a coroutine frame and then use
 * it after a suspension point.
 *
 * @param borrowed Immutable borrow intentionally passed by reference so the
 * coroutine frame would contain a reference to the caller's `Ref<int>` object.
 * @return A `cf_ref_across_coawait_task` marker; conforming Clang backend lanes
 * reject the call that tries to create it from a temporary borrow.
 * @pre `borrowed` refers to a live `memsafe::Ref<int>` whose owner remains
 * alive for ordinary non-coroutine use.
 * @post In a conforming Clang backend lane, no executable is produced because
 * the temporary borrow at the call site cannot be captured across suspension.
 * @invariant The function suspends before dereferencing `borrowed`, so the
 * borrow must be represented in the coroutine frame across `co_await`.
 * @throws No exception is intentionally thrown. If an unexpected exception
 * occurred, the promise would terminate.
 * @note Ownership/thread-safety: `borrowed` is non-owning and single-threaded.
 * The function intentionally violates the documented rule that `Ref<T>` is not
 * held across `co_await`.
 *
 * Example:
 * @code
 * memsafe::Owner<int> owner(7);
 * auto task = cf_hold_ref_across_coawait(owner.borrow()); // expected failure
 * (void)task;
 * @endcode
 */
#  line 101 "cf_ref_across_coawait_body_probe.cpp"
cf_ref_across_coawait_task
cf_hold_ref_across_coawait(const memsafe::Ref<int>& borrowed) {
    /*
     * Spec rationale: F1 Slice 7 and F2 Ownership case 5 prohibit a Ref/MutRef
     * borrow from becoming coroutine-frame state across suspension. The use
     * after `co_await` forces `borrowed` to remain live across the suspension
     * instead of being a dead parameter that an optimizer could ignore.
     */
    co_await std::suspend_always{};

    const int observed = *borrowed;
    (void)observed;
    co_return;
}

} // namespace

/**
 * @brief Intentionally pass a temporary `Ref<int>` into a coroutine that keeps
 * it live across `co_await`.
 *
 * @retval 0 Unreachable in conforming Clang backend validation lanes.
 * @pre Built by the F4 compile-fail harness in a Clang C++20 lane with
 * `MEMSAFE_BACKEND_CLANG` active and Clang coroutine lifetime attributes
 * available.
 * @post The translation unit fails before an executable is produced.
 * @invariant The source consumes the `owner.borrow()` result by passing it as
 * an argument, so nodiscard warnings are not the accepted failure reason. The
 * expected failure is the Clang lifetime diagnostic for capturing that
 * temporary borrow in a coroutine frame.
 * @throws Nothing intentionally; compilation is expected to fail before
 * runtime.
 * @note Ownership/thread-safety: `owner` is a single-threaded unique owner. The
 * temporary `Ref<int>` is non-owning and must not survive a coroutine
 * suspension, which is exactly what the call below attempts.
 *
 * Example:
 * @code
 * memsafe::Owner<int> owner(7);
 * auto task = cf_hold_ref_across_coawait(owner.borrow());
 * (void)task;
 * @endcode
 */
#  line 301 "cf_ref_across_coawait_call_probe.cpp"
int main() {
    memsafe::Owner<int> owner(7);

    /*
     * The `#line` probe name above gives the generic F4 regex a stable anchor
     * for Clang's lifetime diagnostic while keeping the real code path minimal:
     * one owner, one temporary Ref, and one coroutine call that would carry that
     * Ref across a suspension point.
     */
    cf_ref_across_coawait_task task =
        cf_hold_ref_across_coawait(owner.borrow());

    (void)task;
    return 0;
}

#endif
