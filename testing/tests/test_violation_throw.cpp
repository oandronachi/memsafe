/**
 * @file test_violation_throw.cpp
 * @brief Runtime test for the CPP_MEMSAFE-0040-TEST THROW policy lane.
 *
 * @details
 * Work package: CPP_MEMSAFE-0040-TEST.
 *
 * Purpose:
 * - Compile this translation unit with the canonical `MEMSAFE_VIOLATION_THROW`
 *   policy selected before including the memsafe violation header.
 * - Trigger representative borrow-exclusivity and generation-mismatch reports
 *   through the Slice 0 policy hook.
 * - Verify that each report throws `memsafe::violation` with a non-empty
 *   diagnostic message and populated source-location fields.
 *
 * Key invariants:
 * - The test uses only `testing/test_harness.hpp` CHECK primitives for verdicts.
 * - Each report is routed through `MEMSAFE_DETAIL_VIOLATE`, so the same call
 *   shape exercised here is available to later owner and handle packages.
 * - Source-location checks require non-empty file and function strings plus a
 *   positive line number in both the C++20 and C++17-compatible lanes.
 *
 * Ownership and thread-safety:
 * - The test owns no heap state and starts no worker threads.
 * - Diagnostic string literals have static storage duration and remain valid
 *   for exception inspection.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the canonical THROW violation policy for this translation unit.
 *
 * @retval MEMSAFE_VIOLATION_THROW Directs every Slice 0 violation report in
 * this file to throw `memsafe::violation`.
 * @pre This macro must be defined before including `<memsafe/violation.hpp>`.
 * @post `memsafe::detail::report_violation` follows the THROW lane in this
 * translation unit.
 * @invariant The test names only canonical policy tokens from the Slice 0
 * public configuration surface.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: this preprocessor selection owns no storage
 * and modifies no runtime state.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW

#include "../test_harness.hpp"

#include <memsafe/violation.hpp>

#include <cstring>

namespace {

/**
 * @struct expected_violation
 * @brief Describes one THROW-lane violation case.
 *
 * @pre `message` points to static storage containing a valid C string.
 * @post Instances are read-only test data passed to `trigger_violation`.
 * @invariant `kind` is the exact violation class expected from the thrown
 * `memsafe::violation`.
 * @throws Nothing; this aggregate owns no resources.
 * @note Ownership/thread-safety: instances borrow static message literals and
 * are read only after initialization.
 */
struct expected_violation {
    /// Violation class expected from the exception payload.
    memsafe::violation_kind kind;
    /// Diagnostic message expected from `what()` and `violation_info::message`.
    const char* message;
};

/**
 * @brief Return whether a C string carries visible diagnostic content.
 *
 * @param text Pointer to inspect; null is accepted.
 * @return `true` when `text` is non-null and `text[0] != '\0'`; otherwise
 * `false`.
 * @pre No precondition.
 * @post No state is modified.
 * @invariant The function never dereferences a null pointer.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the pointer is borrowed only for the duration
 * of the call and no shared state is touched.
 */
bool has_text(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

/**
 * @brief Compare two borrowed C strings for exact equality.
 *
 * @param lhs Left-hand string pointer; null is accepted.
 * @param rhs Right-hand string pointer; null is accepted.
 * @return `true` when both pointers are non-null and the pointed-to strings are
 * byte-for-byte equal; otherwise `false`.
 * @pre Non-null pointers must refer to valid C strings.
 * @post No state is modified.
 * @invariant Null input is converted to a failed comparison before
 * `std::strcmp` is called.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: both strings are borrowed temporarily and no
 * shared state is touched.
 */
bool same_text(const char* lhs, const char* rhs) noexcept {
    return lhs != nullptr && rhs != nullptr && std::strcmp(lhs, rhs) == 0;
}

/**
 * @brief Trigger a Slice 0 violation for one THROW-lane case.
 *
 * @param test_case Violation kind and diagnostic message to report.
 * @return This function returns only if the configured policy fails to throw.
 * @pre `test_case.message` points to a valid string literal.
 * @post Under `MEMSAFE_VIOLATION_THROW`, control leaves by throwing
 * `memsafe::violation`.
 * @invariant The violation kind and message are forwarded unchanged into the
 * policy hook.
 * @throws `memsafe::violation` when the THROW policy is active.
 * @note Ownership/thread-safety: all payload strings are borrowed from static
 * storage. No handler state is installed or read by this lane.
 */
void trigger_violation(const expected_violation& test_case) {
    /*
     * F2 "Violation Policy and Diagnostics" case 3 requires the THROW lane to
     * carry message plus source-location metadata. Calling the Slice 0 macro
     * directly keeps this package independent from later owner and handle APIs.
     */
    MEMSAFE_DETAIL_VIOLATE(test_case.kind, test_case.message);
}

/**
 * @brief Verify the exception payload for one expected violation.
 *
 * @param test_case Violation kind and message expected from the thrown object.
 * @return Nothing.
 * @pre `test_case.message` is non-null and points to static storage.
 * @post The project harness records any mismatch as a failed CHECK.
 * @invariant Both the convenience accessors and the stored `violation_info`
 * agree on kind, message, file, line, and function population.
 * @throws Nothing intentionally; unexpected exception types are converted into
 * failed CHECK results.
 * @note Ownership/thread-safety: the caught exception is inspected
 * synchronously in this single-threaded test process.
 */
void verify_throw_case(const expected_violation& test_case) {
    CHECK_THROWS(trigger_violation(test_case), memsafe::violation);

    try {
        trigger_violation(test_case);
        CHECK(false);
    } catch (const memsafe::violation& caught) {
        const memsafe::violation_info& info = caught.info();

        CHECK(caught.kind() == test_case.kind);
        CHECK(info.kind == test_case.kind);

        CHECK(has_text(caught.what()));
        CHECK(has_text(caught.message()));
        CHECK(has_text(info.message));
        CHECK(same_text(caught.what(), test_case.message));
        CHECK(same_text(caught.message(), test_case.message));
        CHECK(same_text(info.message, test_case.message));

        CHECK(has_text(caught.file()));
        CHECK(caught.line() > 0);
        CHECK(has_text(caught.function()));
        CHECK(has_text(info.file));
        CHECK(info.line > 0);
        CHECK(has_text(info.function));
    } catch (...) {
        CHECK(false);
    }
}

} // namespace

/**
 * @brief Run the THROW policy violation diagnostics test.
 *
 * @return Zero when all THROW-lane checks pass; nonzero when the harness
 * recorded any failure.
 * @pre The executable is compiled as a standalone F4 runtime test with the
 * project include directory available.
 * @post The test harness prints a summary and returns its status code.
 * @invariant Exactly the borrow-exclusivity and generation-mismatch classes
 * named by CPP_MEMSAFE-0040-TEST acceptance are covered here.
 * @throws Nothing intentionally; unexpected exceptions are allowed to surface
 * as process failures under the runner's exit-code model.
 * @note Ownership/thread-safety: all test data is immutable and the process
 * starts no worker threads.
 */
int main() {
    const expected_violation cases[] = {
        {memsafe::violation_kind::borrow_exclusivity,
         "mutable borrow requested while shared borrow exists"},
        {memsafe::violation_kind::generation_mismatch,
         "stale handle generation observed"}
    };

    for (const expected_violation& test_case : cases) {
        verify_throw_case(test_case);
    }

    RUN_TESTS("test_violation_throw");
}
