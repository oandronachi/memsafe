/**
 * @file test_backend_macros.cpp
 * @brief Compile-time and runtime checks for Slice 0 backend annotation macros.
 *
 * @details
 * Work package: CPP_MEMSAFE-0060-TEST.
 *
 * Purpose:
 * - Verify that `memsafe/backend.hpp` selects exactly one Slice 0 compiler
 *   backend on supported Clang, GCC, and MSVC-family compilers.
 * - Verify that `MEMSAFE_NODISCARD` expands unconditionally to the standard
 *   `[[nodiscard]]` spelling required by the PRD discardability contract.
 * - Verify that `MEMSAFE_LIFETIMEBOUND` and `MEMSAFE_BORROWS(x)` expand to the
 *   expected Clang attributes only for an actual Clang backend with reported
 *   attribute support, and otherwise degrade to empty expansions.
 *
 * Key invariants:
 * - The active backend count is exactly one for the supported compiler families
 *   used by the F4 lanes.
 * - Clang detection takes precedence over GCC and MSVC compatibility macros.
 * - `MEMSAFE_NODISCARD` is not backend-gated.
 * - The borrow annotation test stringifies macro expansions instead of relying
 *   on runtime behavior because Slice 0 backend hooks are preprocessor-only
 *   static-analysis metadata.
 *
 * Ownership and thread-safety:
 * - The test owns only automatic scalar values, performs no allocation, and
 *   starts no worker threads. All macro checks are translation-unit local.
 *
 * Traceability:
 * - F1 Slice 0 (iii) backend detection macros.
 * - F1 Slice 0 (iv) `MEMSAFE_BORROWS(x)` and `MEMSAFE_NODISCARD`.
 * - F2 compile-time static tests and optional backend macro expansion checks.
 * - F3 `Backend_Detect`.
 */
#include "../test_harness.hpp"

#include <memsafe/backend.hpp>

/**
 * @def MEMSAFE_TEST_STRINGIFY_IMPL
 * @brief Stringize an already-expanded token sequence for macro-spelling tests.
 *
 * @param tokens Token sequence supplied by `MEMSAFE_TEST_STRINGIFY`.
 * @return A string literal containing the canonical preprocessor spelling of
 * the expanded token sequence.
 * @pre Use only in this test translation unit after any macro under test has
 * been included.
 * @post No runtime state is modified; the result is a string literal usable in
 * `constexpr` comparisons.
 * @invariant The macro evaluates @p tokens zero times.
 * @throws Nothing; malformed token sequences remain compile-time errors.
 * @note Ownership/thread-safety: produces no storage beyond the compiler-owned
 * string literal and has no synchronization behavior.
 *
 * Example:
 * @code
 * static_assert(memsafe_test_c_string_equal(
 *     MEMSAFE_TEST_STRINGIFY(MEMSAFE_NODISCARD), "[[nodiscard]]"));
 * @endcode
 */
#define MEMSAFE_TEST_STRINGIFY_IMPL(tokens) #tokens

/**
 * @def MEMSAFE_TEST_STRINGIFY
 * @brief Expand then stringize a macro under test.
 *
 * @param tokens Token sequence to expand before stringification.
 * @return A string literal containing the post-expansion token spelling.
 * @pre Use only with macros whose expansion is valid as a preprocessor macro
 * argument.
 * @post No runtime state is modified; the result is suitable for compile-time
 * spelling assertions.
 * @invariant The macro evaluates @p tokens zero times and preserves the Slice 0
 * contract that annotation macros are compile-time metadata only.
 * @throws Nothing; invalid macro arguments are diagnosed by the preprocessor.
 * @note Ownership/thread-safety: produces no owned runtime resources.
 *
 * Example:
 * @code
 * constexpr const char* spelling = MEMSAFE_TEST_STRINGIFY(MEMSAFE_BORROWS(x));
 * @endcode
 */
#define MEMSAFE_TEST_STRINGIFY(tokens) MEMSAFE_TEST_STRINGIFY_IMPL(tokens)

namespace {

/**
 * @brief Compare two null-terminated strings during constant evaluation.
 *
 * @param lhs Left-hand null-terminated string. Must not be null.
 * @param rhs Right-hand null-terminated string. Must not be null.
 * @return `true` when both strings contain the same character sequence,
 * otherwise `false`.
 * @pre `lhs` and `rhs` point to valid null-terminated byte strings.
 * @post No storage is modified and no ownership is transferred.
 * @invariant The comparison is byte-for-byte and locale-independent, which
 * keeps macro-spelling checks stable across the F4 compiler lanes.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows both string literals for the duration
 * of the call and is reentrant.
 *
 * Example:
 * @code
 * static_assert(memsafe_test_c_string_equal("a", "a"));
 * @endcode
 */
constexpr bool memsafe_test_c_string_equal(const char* lhs,
                                           const char* rhs) noexcept {
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

constexpr int active_backend_count = MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT;

static_assert(active_backend_count == 1,
              "exactly one Slice 0 MEMSAFE_BACKEND_* macro must be active");

#if defined(__clang__)
static_assert(MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE == 1,
              "Clang must select MEMSAFE_BACKEND_CLANG");
static_assert(MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE == 0,
              "Clang must not also select MEMSAFE_BACKEND_GCC");
static_assert(MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE == 0,
              "Clang must not also select MEMSAFE_BACKEND_MSVC");
#elif defined(__GNUC__)
static_assert(MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE == 1,
              "GCC must select MEMSAFE_BACKEND_GCC");
static_assert(MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE == 0,
              "GCC must not also select MEMSAFE_BACKEND_CLANG");
static_assert(MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE == 0,
              "GCC must not also select MEMSAFE_BACKEND_MSVC");
#elif defined(_MSC_VER)
static_assert(MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE == 1,
              "MSVC must select MEMSAFE_BACKEND_MSVC");
static_assert(MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE == 0,
              "MSVC must not also select MEMSAFE_BACKEND_CLANG");
static_assert(MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE == 0,
              "MSVC must not also select MEMSAFE_BACKEND_GCC");
#else
#  error "test_backend_macros.cpp requires Clang, GCC, or MSVC backend detection"
#endif

constexpr const char nodiscard_expansion[] =
    MEMSAFE_TEST_STRINGIFY(MEMSAFE_NODISCARD);
static_assert(memsafe_test_c_string_equal(nodiscard_expansion,
                                          "[[nodiscard]]"),
              "MEMSAFE_NODISCARD must always expand to [[nodiscard]]");

#if defined(__clang__) && MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE && \
    defined(__has_cpp_attribute)
#  if __has_cpp_attribute(clang::lifetimebound)
constexpr const char expected_lifetimebound_expansion[] =
    "[[clang::lifetimebound]]";
#  else
constexpr const char expected_lifetimebound_expansion[] = "";
#  endif
#  if __has_cpp_attribute(clang::lifetimebound) && \
      __has_cpp_attribute(clang::coro_lifetimebound)
constexpr const char expected_borrows_expansion[] =
    "[[clang::lifetimebound]] [[clang::coro_lifetimebound]]";
#  elif __has_cpp_attribute(clang::lifetimebound)
constexpr const char expected_borrows_expansion[] =
    "[[clang::lifetimebound]]";
#  elif __has_cpp_attribute(clang::coro_lifetimebound)
constexpr const char expected_borrows_expansion[] =
    "[[clang::coro_lifetimebound]]";
#  else
constexpr const char expected_borrows_expansion[] = "";
#  endif
#else
constexpr const char expected_lifetimebound_expansion[] = "";
constexpr const char expected_borrows_expansion[] = "";
#endif

constexpr const char lifetimebound_expansion[] =
    MEMSAFE_TEST_STRINGIFY(MEMSAFE_LIFETIMEBOUND);
static_assert(memsafe_test_c_string_equal(lifetimebound_expansion,
                                          expected_lifetimebound_expansion),
              "MEMSAFE_LIFETIMEBOUND must match the active compiler support");

constexpr const char borrows_expansion[] =
    MEMSAFE_TEST_STRINGIFY(MEMSAFE_BORROWS(source));
static_assert(memsafe_test_c_string_equal(borrows_expansion,
                                          expected_borrows_expansion),
              "MEMSAFE_BORROWS(x) must match the active compiler support");

/**
 * @brief Small by-value token whose type-level discardability exercises
 * `MEMSAFE_NODISCARD` in a normal positive test.
 *
 * @pre Construct with an integer payload when a concrete token value is needed.
 * @post Construction stores the supplied payload and performs no allocation.
 * @invariant The type is trivially copyable and contains only a diagnostic test
 * payload; it does not own memory or synchronize with other threads.
 * @throws Nothing.
 * @note Ownership/thread-safety: value type with no shared state.
 */
struct MEMSAFE_NODISCARD memsafe_test_token {
    /// Integer payload used by runtime checks to ensure the token is consumed.
    int value;
};

/**
 * @brief Return a nodiscard scalar result through the macro under test.
 *
 * @return The integer sentinel `7`.
 * @pre None.
 * @post No global state is modified.
 * @invariant The function-level `MEMSAFE_NODISCARD` spelling must be accepted
 * by every supported compiler lane.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns by value and touches no shared state.
 *
 * Example:
 * @code
 * const int value = memsafe_test_nodiscard_integer();
 * @endcode
 */
MEMSAFE_NODISCARD int memsafe_test_nodiscard_integer() noexcept {
    return 7;
}

/**
 * @brief Construct a nodiscard token whose result is intentionally consumed.
 *
 * @return A `memsafe_test_token` with `value == 11`.
 * @pre None.
 * @post No global state is modified.
 * @invariant The positive runtime test consumes the return value, leaving the
 * discard diagnostic itself to the paired compile-fail artifact.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns an independent value with no shared
 * ownership.
 *
 * Example:
 * @code
 * const memsafe_test_token token = memsafe_test_make_token();
 * @endcode
 */
memsafe_test_token memsafe_test_make_token() noexcept {
    return memsafe_test_token{11};
}

} // namespace

/**
 * @brief Execute the backend macro checks as a standalone F4 runtime test.
 *
 * @return Zero when compile-time assertions and runtime sanity checks pass;
 * nonzero when a runtime `CHECK` fails.
 * @pre The executable is built through the generic testing harness with
 * `include/` on the compiler include path.
 * @post Prints the shared test-harness summary and exits with that status.
 * @invariant Runtime checks mirror the compile-time assertions so the produced
 * executable validates the active compiler lane without inspecting external
 * files.
 * @throws Nothing intentionally; unexpected exceptions are allowed to terminate
 * the process as a test failure under the harness exit-code contract.
 * @note Ownership/thread-safety: owns only local automatic values and starts no
 * threads.
 */
int main() {
    CHECK(active_backend_count == 1);

#if defined(__clang__)
    CHECK(MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE == 1);
#elif defined(__GNUC__)
    CHECK(MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE == 1);
#elif defined(_MSC_VER)
    CHECK(MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE == 1);
#endif

    CHECK(memsafe_test_c_string_equal(nodiscard_expansion, "[[nodiscard]]"));
    CHECK(memsafe_test_c_string_equal(lifetimebound_expansion,
                                      expected_lifetimebound_expansion));
    CHECK(memsafe_test_c_string_equal(borrows_expansion,
                                      expected_borrows_expansion));
    CHECK(memsafe_test_nodiscard_integer() == 7);

    const memsafe_test_token token = memsafe_test_make_token();
    CHECK(token.value == 11);

    RUN_TESTS("test_backend_macros");
}
