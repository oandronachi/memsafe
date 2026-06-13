/**
 * @file cf_nodiscard_type.cpp
 * @brief Negative compile test for type-level `MEMSAFE_NODISCARD` discardability.
 *
 * @details
 * Work package: CPP_MEMSAFE-0060-TEST.
 *
 * Purpose:
 * - Validate that `MEMSAFE_NODISCARD` marks a by-value result type so discarding
 *   a returned object is diagnosed under `-Wunused-result -Werror` or the MSVC
 *   equivalent warning-as-error lane.
 * - Provide the F2 compile-fail coverage required for PRD discardability before
 *   later ownership types add their own nodiscard borrow and handle tests.
 *
 * Key invariants:
 * - The returned object is discarded directly in `main`, with no other
 *   ill-formed code in the translation unit, so the compile-fail driver can pin
 *   the result to the intended nodiscard diagnostic.
 * - The test uses a by-value type result rather than a function-only annotation,
 *   matching the PRD requirement that borrow and handle wrapper types are
 *   themselves `[[nodiscard]]`.
 *
 * Ownership and thread-safety:
 * - The file is expected not to produce an executable. If compiled without
 *   warning-as-error flags, all values would be automatic by-value objects with
 *   no allocation and no shared state.
 *
 * Traceability:
 * - F1 Slice 0 (iv) `MEMSAFE_NODISCARD` always expands to `[[nodiscard]]`.
 * - F2 compile-time static tests require nodiscard behavior to be verified by a
 *   `-Wunused-result` compile-fail test.
 * - F3 `Backend_Detect` supplies the macro included by this test.
 */
// INFRA_EXPECT_FAIL: (nodiscard|unused-result|C4834|C4858)

#include <memsafe/backend.hpp>

/**
 * @brief By-value result type that must not be silently discarded.
 *
 * @pre Construct with an integer payload when a concrete result value is
 * required.
 * @post Construction stores the supplied payload and performs no allocation.
 * @invariant The type contains only an integer payload and has no ownership,
 * lifetime, or synchronization side effects beyond the compile-time
 * `MEMSAFE_NODISCARD` annotation.
 * @throws Nothing.
 * @note Ownership/thread-safety: trivially owned value object; no shared state.
 */
struct MEMSAFE_NODISCARD memsafe_compile_fail_token {
    /// Payload kept solely to make the returned type concrete and non-empty.
    int value;
};

/**
 * @brief Create a nodiscard token for the negative discard test.
 *
 * @return A `memsafe_compile_fail_token` with `value == 42`.
 * @pre None.
 * @post No global state is modified.
 * @invariant Returning the nodiscard type by value is the only diagnostic
 * trigger intended by this file.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns an independent value with no shared
 * ownership or synchronization.
 *
 * Example:
 * @code
 * const memsafe_compile_fail_token token = memsafe_make_compile_fail_token();
 * @endcode
 */
memsafe_compile_fail_token memsafe_make_compile_fail_token() noexcept {
    return memsafe_compile_fail_token{42};
}

/**
 * @brief Intentionally discard a nodiscard by-value result.
 *
 * @return Zero only in builds that do not promote the required nodiscard
 * diagnostic to an error; the F4 compile-fail lane must reject this file before
 * execution is possible.
 * @pre The compile-fail target is built with `-Wunused-result -Werror` on
 * GCC/Clang or `/WX` on MSVC.
 * @post No executable should be produced by a conforming package validation
 * lane.
 * @invariant The call expression discards the result directly so the diagnostic
 * is attributable to type-level `MEMSAFE_NODISCARD`.
 * @throws Nothing intentionally; compilation is expected to fail first.
 * @note Ownership/thread-safety: no runtime ownership or synchronization exists
 * when the expected compile-time failure occurs.
 */
int main() {
    /*
     * PRD discardability and the CPP_MEMSAFE-0060-TEST acceptance criterion
     * require a by-value discard to fail under the compile-fail warning policy;
     * keeping the call as a bare expression avoids masking the diagnostic with
     * unrelated syntax or type errors.
     */
    memsafe_make_compile_fail_token();
    return 0;
}
