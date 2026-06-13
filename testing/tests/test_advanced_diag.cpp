/**
 * @file test_advanced_diag.cpp
 * @brief HANDLER-lane unit test for Slice 6 advanced diagnostics.
 *
 * @details
 * Work package: CPP_MEMSAFE-0610-TEST.
 *
 * Purpose:
 * - Compile this translation unit with `MEMSAFE_VIOLATION_HANDLER` selected
 *   before including the memsafe violation header.
 * - Trigger one representative borrow-exclusivity violation through the Slice 6
 *   reporting macro that carries extended context and a structured borrow
 *   chain.
 * - Assert that the deterministic handler receives every extended payload field
 *   required by the advanced-diagnostics slice.
 * - Assert that the optional logging hook observes the same populated payload,
 *   exercising the logging-framework hook without depending on any external
 *   logging library.
 *
 * Key invariants:
 * - The test uses only `testing/test_harness.hpp` CHECK primitives for verdicts.
 * - The handler and log hook are installed before the report and cleared before
 *   process exit.
 * - Borrow-chain frame storage outlives the synchronous handler and log-hook
 *   callbacks, matching the non-owning lifetime contract of
 *   `memsafe::violation_info`.
 * - The test emits a violation only after the HANDLER callback is installed, so
 *   the no-handler fail-safe abort path is not exercised by this package.
 *
 * Ownership and thread-safety:
 * - All captured payload pointers borrow string literals, source-location
 *   strings, static sentinel objects, or stack borrow-chain frames that are
 *   inspected before their lifetime ends.
 * - The test is single-threaded; global capture slots are process-local test
 *   state and are not synchronization primitives.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the canonical HANDLER violation policy for this translation unit.
 *
 * @retval MEMSAFE_VIOLATION_HANDLER Directs Slice 6 violation reports in this
 * file to the installed `memsafe::violation_handler`.
 * @pre This macro must be defined before including any memsafe header.
 * @post `memsafe::detail::report_violation` follows the HANDLER lane in this
 * translation unit.
 * @invariant The test names only canonical policy tokens from the public
 * configuration surface.
 * @throws Nothing directly; invoked callbacks determine runtime behavior after
 * policy dispatch.
 * @note Ownership/thread-safety: this preprocessor selection owns no storage
 * and modifies no runtime state.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_HANDLER

#include "../test_harness.hpp"

#include <memsafe/violation.hpp>

#include <cstddef>
#include <cstring>

namespace {

const char* const k_message = "exclusive mutable borrow blocked by live shared borrow";
const char* const k_component = "owner";
const char* const k_operation = "borrow_mut";
const char* const k_object_type = "memsafe::Owner<int>";
const char* const k_note = "shared borrow chain retained for diagnostics";
const char* const k_chain_read_operation = "Owner<int>::borrow";
const char* const k_chain_write_operation = "Owner<int>::borrow_mut";
const char* const k_chain_owner_type = "Owner<int>";
const char* const k_chain_mut_type = "MutRef<int>";
const int k_owner_sentinel = 17;
const int k_borrow_sentinel = 23;
const int k_user_context_sentinel = 42;

/**
 * @struct captured_advanced_violation
 * @brief Stores one extended violation payload observed by a callback.
 *
 * @pre Pointer members either are null or point to storage valid through the
 * immediate post-report CHECK sequence.
 * @post `copy_payload` overwrites every field from one synchronous callback
 * invocation.
 * @invariant `called == true` means all fields were copied from the same
 * `memsafe::violation_info` object.
 * @throws Nothing; this aggregate owns no resources.
 * @note Ownership/thread-safety: pointer fields are borrowed. The test mutates
 * instances only on the single main thread while callbacks execute
 * synchronously.
 */
struct captured_advanced_violation {
    /// Whether the callback has been invoked since the last reset.
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
    /// Active policy copied from `memsafe::violation_info::policy`.
    memsafe::violation_policy policy;
    /// Reporting component copied from `memsafe::violation_info::component`.
    const char* component;
    /// Reporting operation copied from `memsafe::violation_info::operation`.
    const char* operation;
    /// Object type copied from `memsafe::violation_info::object_type`.
    const char* object_type;
    /// Diagnostic object address copied from `memsafe::violation_info::object_address`.
    const void* object_address;
    /// Extra diagnostic note copied from `memsafe::violation_info::diagnostic_note`.
    const char* diagnostic_note;
    /// Borrow-chain pointer copied from `memsafe::violation_info::borrow_chain`.
    const memsafe::borrow_chain_frame* borrow_chain;
    /// Borrow-chain frame count copied from `memsafe::violation_info::borrow_chain_size`.
    std::size_t borrow_chain_size;
    /// Opaque caller context copied from `memsafe::violation_info::user_context`.
    const void* user_context;
};

captured_advanced_violation g_handler_capture = {
    false,
    memsafe::violation_kind::null_access,
    nullptr,
    nullptr,
    0,
    nullptr,
    memsafe::violation_policy::abort,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    0U,
    nullptr
};

captured_advanced_violation g_log_capture = {
    false,
    memsafe::violation_kind::null_access,
    nullptr,
    nullptr,
    0,
    nullptr,
    memsafe::violation_policy::abort,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    0U,
    nullptr
};

/**
 * @brief Return whether a borrowed C string carries visible diagnostic content.
 *
 * @param text Pointer to inspect; null is accepted.
 * @return `true` when `text` is non-null and not empty; otherwise `false`.
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
 * @pre Non-null pointers must refer to valid null-terminated strings.
 * @post No state is modified.
 * @invariant Null input fails before `std::strcmp` is called.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: both strings are borrowed temporarily and no
 * shared state is touched.
 */
bool same_text(const char* lhs, const char* rhs) noexcept {
    return lhs != nullptr && rhs != nullptr && std::strcmp(lhs, rhs) == 0;
}

/**
 * @brief Return whether a borrowed C string contains a non-empty token.
 *
 * @param text String to search; null is accepted.
 * @param token Token to find; null is accepted.
 * @return `true` when both inputs are non-null, `token` is not empty, and the
 * token appears inside `text`; otherwise `false`.
 * @pre Non-null pointers must refer to valid null-terminated strings.
 * @post No state is modified.
 * @invariant The function never calls `std::strstr` with a null pointer.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: both strings are borrowed temporarily and no
 * shared state is touched.
 */
bool contains_text(const char* text, const char* token) noexcept {
    return has_text(text) && has_text(token) && std::strstr(text, token) != nullptr;
}

/**
 * @brief Clear one callback capture to its neutral state.
 *
 * @param capture Capture slot to reset.
 * @return Nothing.
 * @pre No callback is currently writing `capture`.
 * @post `capture.called` is false and every borrowed pointer field is null.
 * @invariant The default enum values are ignored until `called` becomes true.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper mutates single-threaded test state.
 */
void reset_capture(captured_advanced_violation& capture) noexcept {
    capture = {false,
               memsafe::violation_kind::null_access,
               nullptr,
               nullptr,
               0,
               nullptr,
               memsafe::violation_policy::abort,
               nullptr,
               nullptr,
               nullptr,
               nullptr,
               nullptr,
               nullptr,
               0U,
               nullptr};
}

/**
 * @brief Clear every callback capture before one violation report is emitted.
 *
 * @return Nothing.
 * @pre No handler or log-hook invocation is active.
 * @post Both global capture slots are reset.
 * @invariant Each test report starts with no stale callback state.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: this helper mutates process-global test state
 * in the single test thread.
 */
void reset_captures() noexcept {
    reset_capture(g_handler_capture);
    reset_capture(g_log_capture);
}

/**
 * @brief Copy an extended memsafe payload into a stable test capture slot.
 *
 * @param destination Capture slot that receives field-by-field payload values.
 * @param info Borrowed violation payload supplied by the memsafe policy hook.
 * @return Nothing.
 * @pre `info` remains valid for the duration of this synchronous callback.
 * Pointer fields inside `info` remain valid through the immediate verification
 * step.
 * @post `destination.called` is true and all fields mirror `info`.
 * @invariant The helper performs a shallow pointer copy only, matching the
 * documented non-owning `violation_info` contract.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: copied pointer fields remain borrowed. The
 * destination is single-threaded test state.
 */
void copy_payload(captured_advanced_violation& destination,
                  const memsafe::violation_info& info) noexcept {
    destination = {true,
                   info.kind,
                   info.message,
                   info.file,
                   info.line,
                   info.function,
                   info.policy,
                   info.component,
                   info.operation,
                   info.object_type,
                   info.object_address,
                   info.diagnostic_note,
                   info.borrow_chain,
                   info.borrow_chain_size,
                   info.user_context};
}

/**
 * @brief Deterministic violation handler installed for the HANDLER lane.
 *
 * @param info Borrowed violation payload supplied by the memsafe policy hook.
 * @return Nothing.
 * @pre `info` remains valid for the duration of this synchronous callback.
 * @post `g_handler_capture` contains a field-by-field copy of the payload.
 * @invariant The handler performs no filtering; one report produces one
 * captured handler payload.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: copied pointer fields remain borrowed. The
 * callback touches only single-threaded test state.
 */
void capture_handler(const memsafe::violation_info& info) noexcept {
    copy_payload(g_handler_capture, info);
}

/**
 * @brief Deterministic logging hook installed for the advanced diagnostics test.
 *
 * @param info Borrowed violation payload supplied before policy dispatch.
 * @return Nothing.
 * @pre `info` remains valid for the duration of this synchronous callback.
 * @post `g_log_capture` contains a field-by-field copy of the payload.
 * @invariant The hook never throws, so the test validates the normal logging
 * integration path without engaging the library's hook-exception suppression.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: copied pointer fields remain borrowed. The
 * callback touches only single-threaded test state.
 */
void capture_log_hook(const memsafe::violation_info& info) noexcept {
    copy_payload(g_log_capture, info);
}

/**
 * @brief Verify one structured borrow-chain frame.
 *
 * @param frame Frame copied through the extended violation payload.
 * @param expected_operation Operation string expected for the frame.
 * @param expected_object_type Object type string expected for the frame.
 * @param expected_object_address Diagnostic object address expected for the frame.
 * @return Nothing.
 * @pre `frame` is a valid element from the borrow-chain array supplied to the
 * report macro. Expected strings point to static storage.
 * @post The project harness records any mismatch as a failed CHECK.
 * @invariant A present borrow-chain diagnostic frame must include source
 * metadata as well as caller-supplied context fields.
 * @throws Nothing intentionally; CHECK records failures by process-local count.
 * @note Ownership/thread-safety: all pointers are borrowed and inspected
 * synchronously on the single main thread.
 */
void verify_chain_frame(const memsafe::borrow_chain_frame& frame,
                        const char* expected_operation,
                        const char* expected_object_type,
                        const void* expected_object_address) {
    CHECK(same_text(frame.operation, expected_operation));
    CHECK(same_text(frame.object_type, expected_object_type));
    CHECK(frame.object_address == expected_object_address);
    CHECK(has_text(frame.file));
    CHECK(frame.line > 0);
    CHECK(has_text(frame.function));
}

/**
 * @brief Verify a captured extended diagnostics payload.
 *
 * @param capture Callback capture populated by either the handler or log hook.
 * @param expected_chain Borrow-chain array supplied to the report macro.
 * @param expected_chain_size Number of frames in `expected_chain`.
 * @return Nothing.
 * @pre `capture.called` should be true after a synchronous violation report.
 * `expected_chain` remains alive while the capture is checked.
 * @post The project harness records any mismatch as a failed CHECK.
 * @invariant The Slice 6 handler payload must contain the active policy,
 * reporting context, object identity, user context, and structured borrow chain
 * in addition to the Slice 0 kind/message/source fields.
 * @throws Nothing intentionally; CHECK records failures by process-local count.
 * @note Ownership/thread-safety: the capture is checked immediately after the
 * synchronous callbacks return in the same thread.
 */
void verify_extended_payload(const captured_advanced_violation& capture,
                             const memsafe::borrow_chain_frame* expected_chain,
                             std::size_t expected_chain_size) {
    CHECK(capture.called);
    CHECK(capture.kind == memsafe::violation_kind::borrow_exclusivity);
    CHECK(same_text(capture.message, k_message));
    CHECK(has_text(capture.file));
    CHECK(contains_text(capture.file, "test_advanced_diag.cpp"));
    CHECK(capture.line > 0);
    CHECK(has_text(capture.function));
    CHECK(capture.policy == memsafe::violation_policy::handler);
    CHECK(same_text(memsafe::to_string(capture.policy), "handler"));
    CHECK(same_text(capture.component, k_component));
    CHECK(same_text(capture.operation, k_operation));
    CHECK(same_text(capture.object_type, k_object_type));
    CHECK(capture.object_address == &k_owner_sentinel);
    CHECK(same_text(capture.diagnostic_note, k_note));
    CHECK(capture.user_context == &k_user_context_sentinel);
    CHECK(capture.borrow_chain == expected_chain);
    CHECK(capture.borrow_chain_size == expected_chain_size);
    CHECK(capture.borrow_chain != nullptr);
    CHECK(capture.borrow_chain_size == 2U);

    if (capture.borrow_chain != nullptr && capture.borrow_chain_size == 2U) {
        verify_chain_frame(capture.borrow_chain[0],
                           k_chain_read_operation,
                           k_chain_owner_type,
                           &k_owner_sentinel);
        verify_chain_frame(capture.borrow_chain[1],
                           k_chain_write_operation,
                           k_chain_mut_type,
                           &k_borrow_sentinel);
    }
}

/**
 * @brief Emit and verify one Slice 6 HANDLER-lane advanced diagnostic report.
 *
 * @return Nothing.
 * @pre `capture_handler` is installed as the current memsafe handler and
 * `capture_log_hook` is installed as the current memsafe logging hook.
 * @post Both callback captures contain the same populated extended payload and
 * every field has been asserted.
 * @invariant The borrow-chain array is local to this function and is verified
 * before returning, preserving the non-owning synchronous lifetime contract.
 * @throws Nothing intentionally; unexpected callback exceptions would surface
 * as process failures under the runner's exit-code model.
 * @note Ownership/thread-safety: context strings and sentinel objects have
 * static storage; borrow-chain frames are stack objects consumed synchronously.
 *
 * @code
 * memsafe::set_violation_handler(&capture_handler);
 * emit_and_verify_extended_report();
 * @endcode
 */
void emit_and_verify_extended_report() {
    const memsafe::borrow_chain_frame chain[] = {
        MEMSAFE_DETAIL_BORROW_CHAIN_FRAME(
            k_chain_read_operation, k_chain_owner_type, &k_owner_sentinel),
        MEMSAFE_DETAIL_BORROW_CHAIN_FRAME(
            k_chain_write_operation, k_chain_mut_type, &k_borrow_sentinel)
    };
    constexpr std::size_t chain_size = sizeof(chain) / sizeof(chain[0]);
    const memsafe::violation_context context = {k_component,
                                               k_operation,
                                               k_object_type,
                                               &k_owner_sentinel,
                                               k_note,
                                               &k_user_context_sentinel};

    reset_captures();

    /*
     * F1 Slice 6 and F2's extended handler case require richer handler
     * payloads and a structured borrow-check call-chain diagnostic. The
     * advanced macro is used directly so the test targets the finalized
     * CPP_MEMSAFE-0600-FUNC reporting surface rather than any later owner
     * implementation detail.
     */
    MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(
        memsafe::violation_kind::borrow_exclusivity, k_message, context, chain, chain_size);

    verify_extended_payload(g_handler_capture, chain, chain_size);
    verify_extended_payload(g_log_capture, chain, chain_size);
}

} // namespace

/**
 * @brief Run the Slice 6 advanced diagnostics HANDLER-lane unit test.
 *
 * @return Zero when all extended payload and borrow-chain checks pass; nonzero
 * when the harness recorded any failure.
 * @pre The executable is compiled as a standalone F4 runtime test with the
 * project include directory available.
 * @post Handler and logging hook slots are cleared, the test harness prints a
 * summary, and the process returns the harness status code.
 * @invariant Exactly one advanced violation is reported, and it must be routed
 * through the installed HANDLER callback rather than aborting.
 * @throws Nothing intentionally; unexpected exceptions are allowed to surface
 * as process failures under the runner's exit-code model.
 * @note Ownership/thread-safety: the handler and logging hook slots are
 * process-local; the test starts no worker threads.
 */
int main() {
    memsafe::set_violation_handler(&capture_handler);
    memsafe::set_violation_log_hook(&capture_log_hook);
    CHECK(memsafe::get_violation_handler() == &capture_handler);
    CHECK(memsafe::get_violation_log_hook() == &capture_log_hook);

    emit_and_verify_extended_report();

    memsafe::set_violation_log_hook(nullptr);
    memsafe::set_violation_handler(nullptr);
    CHECK(memsafe::get_violation_log_hook() == nullptr);
    CHECK(memsafe::get_violation_handler() == nullptr);

    RUN_TESTS("test_advanced_diag");
}
