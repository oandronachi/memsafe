/**
 * @file test_violation_handler.cpp
 * @brief Runtime test for the CPP_MEMSAFE-0040-TEST HANDLER policy lane.
 *
 * @details
 * Work package: CPP_MEMSAFE-0040-TEST.
 *
 * Purpose:
 * - Compile this translation unit with the canonical `MEMSAFE_VIOLATION_HANDLER`
 *   policy selected before including the memsafe violation header.
 * - Install a deterministic process-local handler.
 * - Trigger each representative Slice 0 violation class and verify that the
 *   handler receives kind, diagnostic message, and source-location fields.
 *
 * Key invariants:
 * - The test uses only `testing/test_harness.hpp` CHECK primitives for verdicts.
 * - The handler is installed before any report and cleared after all checks.
 * - Every `memsafe::violation_kind` enumerator present in Slice 0 is exercised
 *   once, so later packages can rely on the same handler lane for their own
 *   policy reports.
 *
 * Ownership and thread-safety:
 * - Captured payload pointers borrow string literals or implementation-managed
 *   source-location strings and are inspected synchronously.
 * - The test is single-threaded; the global capture slot is intentionally not a
 *   synchronization primitive.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the canonical HANDLER violation policy for this translation unit.
 *
 * @retval MEMSAFE_VIOLATION_HANDLER Directs every Slice 0 violation report in
 * this file to the installed `memsafe::violation_handler`.
 * @pre This macro must be defined before including `<memsafe/violation.hpp>`.
 * @post `memsafe::detail::report_violation` follows the HANDLER lane in this
 * translation unit.
 * @invariant The test names only canonical policy tokens from the Slice 0
 * public configuration surface.
 * @throws Nothing directly; invoked handlers determine runtime behavior after
 * policy dispatch.
 * @note Ownership/thread-safety: this preprocessor selection owns no storage
 * and modifies no runtime state.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_HANDLER

#include "../test_harness.hpp"

#include <memsafe/violation.hpp>

#include <cstring>

namespace {

/**
 * @struct handler_case
 * @brief Describes one HANDLER-lane violation report to emit.
 *
 * @pre `message` points to static storage containing a valid C string.
 * @post Instances are read-only test data passed to `trigger_violation`.
 * @invariant `kind` is the exact violation class expected by the installed
 * handler.
 * @throws Nothing; this aggregate owns no resources.
 * @note Ownership/thread-safety: instances borrow static message literals and
 * are read only after initialization.
 */
struct handler_case {
    /// Violation class expected in the handler payload.
    memsafe::violation_kind kind;
    /// Diagnostic message expected in the handler payload.
    const char* message;
};

/**
 * @struct captured_violation
 * @brief Stores the most recent payload observed by the deterministic handler.
 *
 * @pre Pointer members either are null or point to storage valid through the
 * immediate post-report CHECK sequence.
 * @post `capture_handler` overwrites every field on each report.
 * @invariant `called == true` means all payload fields were copied from one
 * synchronous handler invocation.
 * @throws Nothing; this aggregate owns no resources.
 * @note Ownership/thread-safety: the storage is process-global for the C-style
 * handler ABI but is used only by the single test thread.
 */
struct captured_violation {
    /// Whether the handler has been invoked since the last reset.
    bool called;
    /// Violation class copied from `memsafe::violation_info::kind`.
    memsafe::violation_kind kind;
    /// Diagnostic message copied from `memsafe::violation_info::message`.
    const char* message;
    /// Source file copied from `memsafe::violation_info::file`.
    const char* file;
    /// Source line copied from `memsafe::violation_info::line`.
    int line;
    /// Function name copied from `memsafe::violation_info::function`.
    const char* function;
};

captured_violation g_capture = {
    false,
    memsafe::violation_kind::null_access,
    nullptr,
    nullptr,
    0,
    nullptr
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
 * @brief Clear the global capture before one report is emitted.
 *
 * @return Nothing.
 * @pre No handler invocation is active.
 * @post `g_capture.called` is false and pointer fields are null.
 * @invariant The default kind value is ignored until `called` becomes true.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper mutates process-global test state
 * in the single test thread.
 */
void reset_capture() noexcept {
    g_capture = {false,
                 memsafe::violation_kind::null_access,
                 nullptr,
                 nullptr,
                 0,
                 nullptr};
}

/**
 * @brief Deterministic handler installed for the HANDLER lane.
 *
 * @param info Borrowed violation payload supplied by the memsafe policy hook.
 * @return Nothing.
 * @pre `info` remains valid for the duration of this synchronous callback.
 * @post `g_capture` contains a field-by-field copy of the observed payload.
 * @invariant The handler performs no filtering; one report produces one
 * captured payload.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: copied pointer fields remain borrowed. The
 * callback touches only single-threaded test state.
 */
void capture_handler(const memsafe::violation_info& info) noexcept {
    g_capture = {true, info.kind, info.message, info.file, info.line, info.function};
}

/**
 * @brief Trigger a Slice 0 violation for one HANDLER-lane case.
 *
 * @param test_case Violation kind and diagnostic message to report.
 * @return Nothing when the installed handler returns.
 * @pre `test_case.message` points to a valid string literal and a handler is
 * installed.
 * @post `capture_handler` has received the policy payload.
 * @invariant The violation kind and message are forwarded unchanged into the
 * policy hook.
 * @throws Nothing when `capture_handler` remains installed.
 * @note Ownership/thread-safety: all payload strings are borrowed from static
 * storage. The source-location payload is inspected before the next report.
 */
void trigger_violation(const handler_case& test_case) {
    /*
     * F2 "Violation Policy and Diagnostics" case 4 requires the HANDLER lane to
     * deliver kind, diagnostic text, and source-location metadata. Direct use
     * of the Slice 0 macro avoids depending on downstream type implementations.
     */
    MEMSAFE_DETAIL_VIOLATE(test_case.kind, test_case.message);
}

/**
 * @brief Verify one handler callback payload.
 *
 * @param test_case Violation kind and message expected from the handler.
 * @return Nothing.
 * @pre `capture_handler` is installed as the current memsafe handler.
 * @post The project harness records any mismatch as a failed CHECK.
 * @invariant Each report must produce exactly one observed payload before the
 * next case resets the capture.
 * @throws Nothing intentionally; the installed handler is `noexcept`.
 * @note Ownership/thread-safety: the capture is checked immediately after the
 * synchronous report in the same thread.
 */
void verify_handler_case(const handler_case& test_case) {
    reset_capture();

    trigger_violation(test_case);

    CHECK(g_capture.called);
    CHECK(g_capture.kind == test_case.kind);
    CHECK(has_text(g_capture.message));
    CHECK(same_text(g_capture.message, test_case.message));
    CHECK(has_text(g_capture.file));
    CHECK(g_capture.line > 0);
    CHECK(has_text(g_capture.function));
}

} // namespace

/**
 * @brief Run the HANDLER policy violation diagnostics test.
 *
 * @return Zero when all HANDLER-lane checks pass; nonzero when the harness
 * recorded any failure.
 * @pre The executable is compiled as a standalone F4 runtime test with the
 * project include directory available.
 * @post The test harness prints a summary and returns its status code.
 * @invariant Every current Slice 0 `memsafe::violation_kind` enumerator is
 * represented once in the handler payload matrix.
 * @throws Nothing intentionally; unexpected exceptions are allowed to surface
 * as process failures under the runner's exit-code model.
 * @note Ownership/thread-safety: the handler slot is process-local and the test
 * starts no worker threads.
 */
int main() {
    const handler_case cases[] = {
        {memsafe::violation_kind::borrow_exclusivity,
         "mutable borrow requested while shared borrow exists"},
        {memsafe::violation_kind::use_after_free,
         "checked handle used after slot release"},
        {memsafe::violation_kind::generation_mismatch,
         "stale handle generation observed"},
        {memsafe::violation_kind::double_free,
         "resource released twice"},
        {memsafe::violation_kind::capacity_exhausted,
         "fixed capacity exhausted"},
        {memsafe::violation_kind::null_access,
         "null control block access"},
        {memsafe::violation_kind::owner_destroyed_while_borrowed,
         "owner destroyed while borrow remains"}
    };

    memsafe::set_violation_handler(&capture_handler);
    CHECK(memsafe::get_violation_handler() == &capture_handler);

    for (const handler_case& test_case : cases) {
        verify_handler_case(test_case);
    }

    memsafe::set_violation_handler(nullptr);
    CHECK(memsafe::get_violation_handler() == nullptr);

    RUN_TESTS("test_violation_handler");
}
