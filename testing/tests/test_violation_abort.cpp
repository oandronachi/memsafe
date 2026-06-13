/**
 * @file test_violation_abort.cpp
 * @brief Parent/child death test for the Slice 0 ABORT violation policy.
 *
 * @details
 * Work package: CPP_MEMSAFE-0050-TEST.
 *
 * Purpose:
 * - Compile this translation unit with the canonical `MEMSAFE_VIOLATION_ABORT`
 *   policy selected before any memsafe header is included.
 * - Re-spawn the current executable with a child-mode argument flag.
 * - Trigger the Slice 0 borrow-exclusivity violation hook in the child to model
 *   an invalid mutable borrow while the ABORT policy is active.
 * - Verify in the parent that the child ended abnormally, then return success
 *   through the normal F4 runtime-test exit-code contract.
 *
 * Key invariants:
 * - The parent process is the only process that reports the test verdict.
 * - The child process reports the violation diagnostic through the inherited
 *   stderr stream and is expected not to return from `std::abort`.
 * - POSIX builds use `fork`, `execvp`, and `waitpid` status inspection.
 * - Windows builds use `CreateProcessA`, a process wait, and exit-code
 *   inspection; no CTest `WILL_FAIL` mode or F4 harness edit is required.
 *
 * Ownership and thread-safety:
 * - The test owns only temporary command-line strings and process handles.
 * - The death-test spawn happens before any worker thread is started, which is
 *   required for deterministic POSIX `fork`/`exec` behavior.
 */

/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Select the canonical ABORT violation policy for this translation unit.
 *
 * @retval MEMSAFE_VIOLATION_ABORT Directs every Slice 0 violation report in
 * this file to print a diagnostic and terminate with `std::abort`.
 * @pre This macro must be defined before including any memsafe public header.
 * @post `memsafe::detail::report_violation` follows the ABORT lane in this
 * translation unit.
 * @invariant The test uses the canonical policy token required by F2
 * "Violation Policy and Diagnostics" case 2 and F3
 * `On_Violation Violation_Policy => Abort`.
 * @throws Nothing directly; the selected policy affects later violation
 * reports.
 * @note Ownership/thread-safety: this preprocessor selection owns no storage
 * and modifies no runtime state.
 */
#define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_ABORT

#include "../test_harness.hpp"
#include "support/death_test.hpp"

#include <memsafe/violation.hpp>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

static_assert(MEMSAFE_ON_VIOLATION == MEMSAFE_VIOLATION_ABORT,
              "test_violation_abort must compile under the ABORT policy");

namespace {

/**
 * @brief Stable case name used in the child-mode argument.
 *
 * @pre The string has static storage duration.
 * @post The value is never modified.
 * @invariant Parent argument construction and child detection use this exact
 * name so unrelated future death-test cases cannot trigger this child body.
 * @throws Nothing; this is immutable static storage.
 * @note Ownership/thread-safety: all callers borrow read-only storage.
 */
const char* const k_abort_case = "invalid-mutable-borrow-abort";

/**
 * @brief Prefix that marks the re-spawned process as the death-test child.
 *
 * @pre The string has static storage duration.
 * @post The value is never modified.
 * @invariant The flag is package-specific rather than a F4 runner option, so
 * the parent can route child mode without changing CMake or CTest metadata.
 * @throws Nothing; this is immutable static storage.
 * @note Ownership/thread-safety: all callers borrow read-only storage.
 */
const char* const k_child_flag_prefix = "--memsafe-abort-policy-child=";

/**
 * @brief Alias the shared death-test result type from CPP_MEMSAFE-0035-TEST.
 *
 * @return This alias names a reusable value type and is not evaluated at
 * runtime.
 * @pre Include `testing/tests/support/death_test.hpp` before using the alias.
 * @post Parent-side spawn helpers return the common result contract.
 * @invariant The parent checks `death_result::passed()` rather than duplicating
 * a second verdict definition for this package.
 * @throws Nothing; aliases do not execute.
 * @note Ownership/thread-safety: aliased result instances own their diagnostic
 * string and no live process handle.
 */
using death_result = memsafe::test_support::death::death_result;

/**
 * @brief Alias the shared child-termination classification.
 *
 * @return This alias names a reusable enum type and is not evaluated at
 * runtime.
 * @pre Include `testing/tests/support/death_test.hpp` before using the alias.
 * @post Platform-specific spawn helpers classify results consistently.
 * @invariant Launch failures never pass as expected abnormal child exits.
 * @throws Nothing; aliases do not execute.
 * @note Ownership/thread-safety: enum values own no resources.
 */
using termination_kind = memsafe::test_support::death::termination_kind;

/**
 * @brief Return whether a byte string begins with a byte-string prefix.
 *
 * @param value Candidate string to inspect; null is accepted.
 * @param prefix Prefix to compare; null is accepted.
 * @return `true` when both inputs are non-null and `value` starts with
 * `prefix`; otherwise `false`.
 * @pre Non-null inputs must point to null-terminated byte strings.
 * @post No state is modified.
 * @invariant Null arguments are treated as non-matches so malformed `argv`
 * entries cannot accidentally select child mode.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: both strings are borrowed only for the
 * duration of the call.
 */
bool starts_with(const char* value, const char* prefix) noexcept {
    if (value == nullptr || prefix == nullptr) {
        return false;
    }

    while (*prefix != '\0') {
        if (*value != *prefix) {
            return false;
        }
        ++value;
        ++prefix;
    }
    return true;
}

/**
 * @brief Build the child-mode argument for this abort-policy case.
 *
 * @return Owned command-line flag selecting the abort child body.
 * @pre `k_child_flag_prefix` and `k_abort_case` point to valid static strings.
 * @post No global state is modified.
 * @invariant The returned flag has exactly one prefix and one case name.
 * @throws `std::bad_alloc` if the returned string allocation fails.
 * @note Ownership/thread-safety: the returned string owns its character
 * storage and can be passed safely to process-spawn APIs while it remains
 * alive.
 *
 * Example:
 * @code
 * const std::string flag = child_flag();
 * @endcode
 */
std::string child_flag() {
    return std::string(k_child_flag_prefix) + k_abort_case;
}

/**
 * @brief Detect whether `main` is running in the abort death-test child.
 *
 * @param argc Argument count received by `main`.
 * @param argv Argument vector received by `main`; entries are inspected but
 * not modified.
 * @return `true` when the package-specific child flag for `k_abort_case` is
 * present; otherwise `false`.
 * @pre Pass the original `argc` and `argv` from `main`.
 * @post No argument storage is modified.
 * @invariant Matching is exact after the prefix, so stale flags for other
 * death-test cases do not execute this child body.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the function borrows `argv` only while it
 * scans the current process arguments.
 */
bool child_requested(int argc, char* const argv[]) noexcept {
    for (int index = 1; index < argc; ++index) {
        const char* const arg = argv == nullptr ? nullptr : argv[index];
        if (starts_with(arg, k_child_flag_prefix)) {
            const char* const name = arg;
            const char* cursor = k_child_flag_prefix;
            const char* suffix = name;
            while (*cursor != '\0') {
                ++cursor;
                ++suffix;
            }

            const char* expected = k_abort_case;
            while (*suffix != '\0' && *expected != '\0' && *suffix == *expected) {
                ++suffix;
                ++expected;
            }
            return *suffix == '\0' && *expected == '\0';
        }
    }
    return false;
}

/**
 * @brief Build owned arguments for re-spawning the current executable.
 *
 * @param argc Argument count received by parent `main`.
 * @param argv Argument vector received by parent `main`.
 * @return Argument vector beginning with `argv[0]` and ending with the
 * child-mode flag; returns an empty vector when the executable path is absent.
 * @pre Pass the original parent arguments before any mutation.
 * @post No caller-owned argument storage is modified.
 * @invariant Existing abort child flags are removed before appending the fresh
 * flag, preventing nested or stale child routing.
 * @throws `std::bad_alloc` if argument copying or vector growth fails.
 * @note Ownership/thread-safety: the returned vector owns every string needed
 * until the platform spawn call has copied or consumed the command line.
 */
std::vector<std::string> child_arguments(int argc, char* const argv[]) {
    std::vector<std::string> args;
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return args;
    }

    args.reserve(static_cast<std::size_t>(argc) + 1u);
    args.push_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && !starts_with(argv[index], k_child_flag_prefix)) {
            args.push_back(argv[index]);
        }
    }
    args.push_back(child_flag());
    return args;
}

/**
 * @brief Construct a result for a platform launch or wait failure.
 *
 * @param message Human-readable reason for the infrastructure failure.
 * @param code Platform error code associated with the failure.
 * @return `death_result` classified as `termination_kind::launch_error`.
 * @pre `message` should point to stable diagnostic text.
 * @post The returned result reports `passed() == false`.
 * @invariant Infrastructure failures are never counted as expected deaths.
 * @throws `std::bad_alloc` if copying the diagnostic message fails.
 * @note Ownership/thread-safety: the returned object owns its message and no
 * process handle.
 */
death_result make_launch_error(const char* message, int code) {
    death_result result;
    result.launched = false;
    result.abnormal = false;
    result.kind = termination_kind::launch_error;
    result.exit_code = code;
    result.signal_number = 0;
    result.message = message == nullptr ? "death-test launch failed" : message;
    return result;
}

/**
 * @brief Return a printable name for a termination classification.
 *
 * @param kind Termination classification to convert.
 * @return Static text naming `kind`.
 * @pre `kind` may be any value representable by `termination_kind`.
 * @post No state is modified.
 * @invariant Unknown values are rendered as `"unknown"` so diagnostics remain
 * total even if the enum grows later.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: returned strings are immutable static storage.
 */
const char* termination_name(termination_kind kind) noexcept {
    switch (kind) {
        case termination_kind::launch_error:
            return "launch_error";
        case termination_kind::normal_exit:
            return "normal_exit";
        case termination_kind::nonzero_exit:
            return "nonzero_exit";
        case termination_kind::signaled:
            return "signaled";
    }
    return "unknown";
}

/**
 * @brief Trigger the invalid mutable-borrow violation in the child process.
 *
 * @return This function returns only if the ABORT policy fails to terminate.
 * @pre The translation unit is compiled with `MEMSAFE_ON_VIOLATION` set to
 * `MEMSAFE_VIOLATION_ABORT`.
 * @post Under the expected policy, the process prints the configured
 * diagnostic and terminates abnormally through `std::abort`.
 * @invariant The violation kind is `borrow_exclusivity`, the Slice 0
 * diagnostic vocabulary for an invalid mutable borrow.
 * @throws Nothing intentionally under ABORT; the selected policy terminates
 * instead of throwing.
 * @note Ownership/thread-safety: the diagnostic string has static storage and
 * the child starts no worker threads.
 */
void trigger_invalid_mutable_borrow() {
    /*
     * F2 "Violation Policy and Diagnostics" case 2 names an invalid mutable
     * borrow as the abort-path stimulus. Slice 0 has not introduced Owner<T>
     * yet, so the accepted CPP_MEMSAFE-0010-FUNC hook is the authoritative way
     * to emit that borrow-exclusivity violation in this package.
     */
    MEMSAFE_DETAIL_VIOLATE(
        memsafe::violation_kind::borrow_exclusivity,
        "invalid mutable borrow while shared borrows are live");
}

#if defined(_WIN32)

/**
 * @brief Quote one Windows command-line argument for `CreateProcessA`.
 *
 * @param argument Argument to quote.
 * @return Command-line-safe representation of `argument`.
 * @pre `argument` contains bytes accepted by the active Windows narrow
 * execution encoding.
 * @post The input string is not modified.
 * @invariant Quotes and trailing backslashes are escaped according to the C
 * runtime command-line parsing rules used by normal C++ `argv` construction.
 * @throws `std::bad_alloc` if the returned string allocation fails.
 * @note Ownership/thread-safety: the returned string owns its character
 * storage; no shared state is touched.
 *
 * Example:
 * @code
 * const std::string token = quote_windows_argument("C:\\Program Files\\test.exe");
 * @endcode
 */
std::string quote_windows_argument(const std::string& argument) {
    if (!argument.empty() &&
        argument.find_first_of(" \t\n\v\"") == std::string::npos) {
        return argument;
    }

    std::string quoted;
    quoted.push_back('"');
    std::size_t backslash_count = 0u;
    for (std::string::const_iterator it = argument.begin(); it != argument.end(); ++it) {
        const char ch = *it;
        if (ch == '\\') {
            ++backslash_count;
        } else if (ch == '"') {
            quoted.append(backslash_count * 2u + 1u, '\\');
            quoted.push_back('"');
            backslash_count = 0u;
        } else {
            quoted.append(backslash_count, '\\');
            backslash_count = 0u;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslash_count * 2u, '\\');
    quoted.push_back('"');
    return quoted;
}

/**
 * @brief Join owned Windows arguments into one mutable command line.
 *
 * @param args Argument strings beginning with the executable path.
 * @return Command line suitable for `CreateProcessA`.
 * @pre `args` is non-empty and every entry is an intended single argument.
 * @post The input vector is not modified.
 * @invariant Arguments are separated by one space after individual quoting.
 * @throws `std::bad_alloc` if the returned command line allocation fails.
 * @note Ownership/thread-safety: the returned string owns its storage and no
 * shared state is touched.
 */
std::string join_windows_command_line(const std::vector<std::string>& args) {
    std::string command_line;
    for (std::vector<std::string>::const_iterator it = args.begin();
         it != args.end();
         ++it) {
        if (!command_line.empty()) {
            command_line.push_back(' ');
        }
        command_line += quote_windows_argument(*it);
    }
    return command_line;
}

/**
 * @brief Re-spawn the current executable on Windows and wait for the child.
 *
 * @param argc Argument count received by parent `main`.
 * @param argv Argument vector received by parent `main`.
 * @return Parent-side result describing the child exit code or launch failure.
 * @pre `argv[0]` names the current executable in a form accepted by
 * `CreateProcessA`.
 * @post The child has completed or a launch error is reported; all process and
 * thread handles opened by `CreateProcessA` are closed.
 * @invariant Any nonzero child exit code is abnormal for this ABORT-policy
 * death test, and a zero exit code means the expected abort did not occur.
 * @throws `std::bad_alloc` if temporary argument or diagnostic storage
 * allocation fails before the process is created.
 * @note Ownership/thread-safety: standard handles are inherited so the child's
 * abort diagnostic remains visible to the parent test log.
 */
death_result spawn_abort_child(int argc, char* const argv[]) {
    const std::vector<std::string> args = child_arguments(argc, argv);
    if (args.empty()) {
        return make_launch_error("Windows death test has no executable path",
                                 ERROR_INVALID_PARAMETER);
    }

    std::string command_line = join_windows_command_line(args);
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    ZeroMemory(&startup_info, sizeof(startup_info));
    ZeroMemory(&process_info, sizeof(process_info));
    startup_info.cb = sizeof(startup_info);

    /*
     * The test plan requires CreateProcess plus explicit process wait and
     * exit-code inspection. Inheriting handles keeps the ABORT diagnostic in
     * the same test log without adding platform-specific pipe capture here.
     */
    const BOOL created = CreateProcessA(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        0,
                                        nullptr,
                                        nullptr,
                                        &startup_info,
                                        &process_info);
    if (!created) {
        return make_launch_error("Windows CreateProcessA failed",
                                 static_cast<int>(GetLastError()));
    }

    const DWORD wait_code = WaitForSingleObject(process_info.hProcess, INFINITE);
    if (wait_code == WAIT_FAILED) {
        const DWORD error = GetLastError();
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return make_launch_error("Windows WaitForSingleObject failed",
                                 static_cast<int>(error));
    }

    DWORD child_exit = 0u;
    if (!GetExitCodeProcess(process_info.hProcess, &child_exit)) {
        const DWORD error = GetLastError();
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return make_launch_error("Windows GetExitCodeProcess failed",
                                 static_cast<int>(error));
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    death_result result;
    result.launched = true;
    result.abnormal = child_exit != 0u;
    result.kind = result.abnormal ? termination_kind::nonzero_exit
                                  : termination_kind::normal_exit;
    result.exit_code = static_cast<int>(child_exit);
    result.signal_number = 0;
    result.message = result.abnormal ? "child exited with nonzero status"
                                     : "child exited normally";
    return result;
}

#else

/**
 * @brief Wait for one POSIX child while retrying interrupted waits.
 *
 * @param pid Process id returned by `fork`.
 * @param status Output pointer receiving the raw wait status word.
 * @return The final `waitpid` return value.
 * @pre `pid` identifies a child process of the caller and `status` is non-null.
 * @post On success, `*status` contains the child's termination status.
 * @invariant `EINTR` cannot convert a valid abnormal child termination into a
 * launch failure.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the function operates on one caller-owned
 * status word and owns no shared state.
 */
int wait_for_abort_child(pid_t pid, int* status) noexcept {
    int wait_result = 0;
    do {
        wait_result = ::waitpid(pid, status, 0);
    } while (wait_result < 0 && errno == EINTR);
    return wait_result;
}

/**
 * @brief Re-spawn the current executable on POSIX and wait for the child.
 *
 * @param argc Argument count received by parent `main`.
 * @param argv Argument vector received by parent `main`.
 * @return Parent-side result describing signal termination, nonzero exit,
 * normal exit, or launch failure.
 * @pre `argv[0]` names the current executable in a form accepted by `execvp`.
 * @post The parent has waited for the child and closed the exec-status pipe.
 * @invariant `execvp` failure is detected with a close-on-exec pipe and is not
 * mistaken for the expected ABORT-policy child death.
 * @throws `std::bad_alloc` if argument or diagnostic storage allocation fails
 * before `fork`.
 * @note Ownership/thread-safety: call from `main` before starting worker
 * threads because the implementation uses the POSIX `fork`/`exec` pattern.
 */
death_result spawn_abort_child(int argc, char* const argv[]) {
    const std::vector<std::string> args = child_arguments(argc, argv);
    if (args.empty()) {
        return make_launch_error("POSIX death test has no executable path", EINVAL);
    }

    int exec_pipe[2] = {-1, -1};
    if (::pipe(exec_pipe) != 0) {
        return make_launch_error("POSIX pipe failed before fork", errno);
    }

    const int flags = ::fcntl(exec_pipe[1], F_GETFD);
    if (flags >= 0) {
        const int ignored = ::fcntl(exec_pipe[1], F_SETFD, flags | FD_CLOEXEC);
        (void)ignored;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(exec_pipe[0]);
        ::close(exec_pipe[1]);
        return make_launch_error("POSIX fork failed", saved_errno);
    }

    if (pid == 0) {
        ::close(exec_pipe[0]);
        std::vector<char*> raw_args;
        raw_args.reserve(args.size() + 1u);
        for (std::vector<std::string>::const_iterator it = args.begin();
             it != args.end();
             ++it) {
            raw_args.push_back(const_cast<char*>(it->c_str()));
        }
        raw_args.push_back(nullptr);

        ::execvp(raw_args[0], raw_args.data());

        /*
         * F2 case 2 only passes when the child reaches the ABORT policy path.
         * Bytes on this close-on-exec pipe prove that exec failed before the
         * child-mode test process ran, so the parent reports launch_error.
         */
        const int saved_errno = errno;
        const ssize_t ignored =
            ::write(exec_pipe[1], &saved_errno, sizeof(saved_errno));
        (void)ignored;
        ::_exit(127);
    }

    ::close(exec_pipe[1]);
    int exec_errno = 0;
    ssize_t read_count = 0;
    do {
        read_count = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
    } while (read_count < 0 && errno == EINTR);
    const int read_errno = read_count < 0 ? errno : 0;
    ::close(exec_pipe[0]);

    int status = 0;
    if (wait_for_abort_child(pid, &status) < 0) {
        return make_launch_error("POSIX waitpid failed", errno);
    }

    if (read_count < 0) {
        return make_launch_error("POSIX exec-status pipe read failed", read_errno);
    }

    if (read_count > 0) {
        return make_launch_error("POSIX execvp failed in child", exec_errno);
    }

    death_result result;
    result.launched = true;
    result.exit_code = 0;
    result.signal_number = 0;
    if (WIFSIGNALED(status)) {
        result.kind = termination_kind::signaled;
        result.abnormal = true;
        result.signal_number = WTERMSIG(status);
        result.message = "child terminated by POSIX signal";
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.abnormal = result.exit_code != 0;
        result.kind = result.abnormal ? termination_kind::nonzero_exit
                                      : termination_kind::normal_exit;
        result.message = result.abnormal ? "child exited with nonzero status"
                                         : "child exited normally";
    } else {
        result.kind = termination_kind::launch_error;
        result.abnormal = false;
        result.message = "child ended in an unrecognized wait status";
    }
    return result;
}

#endif

/**
 * @brief Emit the parent-side death-test diagnostic context.
 *
 * @param result Child-process result returned by `spawn_abort_child`.
 * @return Nothing.
 * @pre `result` was produced in the parent process.
 * @post A single diagnostic line is written to stderr.
 * @invariant The parent always emits the launch/termination classification even
 * when CHECK assertions fail, giving the F4 log enough context to diagnose the
 * child outcome.
 * @throws Nothing intentionally; C stdio failures are ignored by the test.
 * @note Ownership/thread-safety: the function only reads `result` and writes to
 * the process stderr stream.
 */
void emit_child_result(const death_result& result) {
    std::fprintf(stderr,
                 "ABORT death-test child result: launched=%d abnormal=%d "
                 "kind=%s exit_code=%d signal=%d message=%s\n",
                 result.launched ? 1 : 0,
                 result.abnormal ? 1 : 0,
                 termination_name(result.kind),
                 result.exit_code,
                 result.signal_number,
                 result.message.empty() ? "(none)" : result.message.c_str());
}

} // namespace

/**
 * @brief Run the ABORT-policy death test.
 *
 * @param argc Argument count supplied by the platform runtime.
 * @param argv Argument vector supplied by the platform runtime.
 * @return Zero when the parent observes an abnormal child termination; nonzero
 * when launch fails or the child exits normally.
 * @pre The executable is built as a standalone F4 runtime test with the project
 * include directory and `testing/tests/support/death_test.hpp` available.
 * @post The child-mode process either aborts or exits 0 if the violation policy
 * unexpectedly returns; the parent reports the final test verdict.
 * @invariant The parent process is the only process that calls `RUN_TESTS`.
 * @throws Nothing intentionally; unexpected exceptions are allowed to surface
 * as process failures under the runner's exit-code model.
 * @note Ownership/thread-safety: the process starts no worker threads, and all
 * child-spawn resources are released before the parent verdict is reported.
 */
int main(int argc, char** argv) {
    if (child_requested(argc, argv)) {
        trigger_invalid_mutable_borrow();
        return 0;
    }

    const death_result result = spawn_abort_child(argc, argv);
    emit_child_result(result);

    CHECK(result.launched);
    CHECK(result.abnormal);
    CHECK(result.passed());

    RUN_TESTS("test_violation_abort");
}
