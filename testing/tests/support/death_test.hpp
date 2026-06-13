/**
 * @file death_test.hpp
 * @brief Header-only child-process death-test support for F4-style tests.
 *
 * @details
 * Work package: CPP_MEMSAFE-0035-TEST.
 *
 * Purpose:
 * - Let a standalone `testing/tests/*.cpp` executable re-spawn itself as a
 *   child process and run a single negative-path body in that child.
 * - Treat POSIX signal termination and Windows/POSIX nonzero child exits as
 *   abnormal termination, which is the expected oracle for violation-abort and
 *   sanitizer-negative tests.
 * - Preserve F4 runtime `test_launcher` prefixes, such as the optional
 *   Valgrind C++17 lane, so sanitizer-negative children run under the same
 *   observation tool as the parent test.
 * - Keep the helper dependency-free so the F4 CMake harness can build the
 *   default lanes without GoogleTest or platform-specific test runners.
 *
 * Key invariants:
 * - The parent process is the only process that reports the test verdict.
 * - The child process never returns from `expect_abnormal_child`; if the child
 *   body returns normally the helper exits the child with status 0 so the
 *   parent can report that the expected death did not happen.
 * - Launch failures are not counted as successful deaths.
 * - The command-line sentinel is case-specific, so multiple death tests can
 *   share one executable when they use distinct case names.
 * - A launcher prefix is accepted from `MEMSAFE_DEATH_TEST_LAUNCHER_ARGC` plus
 *   `MEMSAFE_DEATH_TEST_LAUNCHER_ARG0..N`, from the semicolon/quoted text in
 *   `MEMSAFE_DEATH_TEST_LAUNCHER`, or, on POSIX hosts with `/proc`, inferred
 *   from the current process command line before `argv[0]`.
 *
 * Ownership and thread-safety:
 * - The helper owns only temporary argument vectors during the spawn call.
 * - Call it from `main()` before starting worker threads. POSIX `fork()` is
 *   intentionally used only for the short exec handoff.
 */
#ifndef MEMSAFE_TEST_SUPPORT_DEATH_TEST_HPP
#define MEMSAFE_TEST_SUPPORT_DEATH_TEST_HPP

#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace memsafe {
namespace test_support {
namespace death {

/**
 * @brief Classify how the child process finished.
 *
 * @details
 * The classification separates a successful launch from the child verdict so
 * callers can fail loudly when the executable could not be spawned. POSIX
 * signals are represented explicitly; Windows abnormal termination is observed
 * as a nonzero exit status through `_spawnvp(_P_WAIT, ...)`.
 *
 * @pre Values are produced by `expect_abnormal_child` or by test code that is
 * constructing an expected result for assertions.
 * @post No resources are owned by the enum value.
 * @invariant `launch_error` never represents a valid death-test pass.
 * @throws Nothing; enum operations are trivial.
 * @note Thread-safety: enum values are immutable once stored by the caller.
 *
 * Example:
 * @code
 * auto result = memsafe::test_support::death::expect_abnormal_child(
 *     argc, argv, "abort-case", [] { std::abort(); });
 * if (result.kind == memsafe::test_support::death::termination_kind::signaled) {
 *     return 0;
 * }
 * @endcode
 */
enum class termination_kind {
    /// The child executable could not be launched or waited for reliably.
    launch_error,
    /// The child exited normally with status 0.
    normal_exit,
    /// The child exited normally with a nonzero status.
    nonzero_exit,
    /// The POSIX child was terminated by a signal.
    signaled
};

/**
 * @brief Result returned to the parent process after a death-test child exits.
 *
 * @details
 * `passed()` is true only when the executable was launched and the child ended
 * abnormally. This is intentionally stricter than "child returned nonzero"
 * because an exec/spawn failure would otherwise masquerade as a successful
 * negative-path assertion.
 *
 * @pre Construct directly only for tests of this helper; production test code
 * should obtain instances from `expect_abnormal_child`.
 * @post The result owns its `message` string and no operating-system handles.
 * @invariant `passed()` is equivalent to `launched && abnormal`.
 * @throws Copying or assigning the result can throw `std::bad_alloc` because
 * the diagnostic string owns memory.
 * @note Thread-safety: independent result objects can be read concurrently.
 *
 * Example:
 * @code
 * auto result = memsafe::test_support::death::expect_abnormal_child(
 *     argc, argv, "violation-aborts", [] { std::abort(); });
 * CHECK(result.passed());
 * @endcode
 */
struct death_result {
    /// True when the helper successfully created a child process.
    bool launched = false;
    /// True when the child terminated by signal or exited with nonzero status.
    bool abnormal = false;
    /// Platform-neutral child termination classification.
    termination_kind kind = termination_kind::launch_error;
    /// Child exit status when the platform reported one; otherwise zero.
    int exit_code = 0;
    /// POSIX signal number when `kind == signaled`; otherwise zero.
    int signal_number = 0;
    /// Human-readable diagnostic for launch failures or unexpected normal exit.
    std::string message;

    /**
     * @brief Report whether the parent should treat the death test as passed.
     *
     * @return `true` when the child was launched and ended abnormally; `false`
     * otherwise.
     * @pre The object must contain a result from the parent process.
     * @post The object is not modified.
     * @invariant Launch failures and normal child exits never pass.
     * @throws Nothing.
     * @note Ownership/thread-safety: the method reads immutable fields only.
     */
    bool passed() const noexcept {
        return launched && abnormal;
    }
};

namespace detail {

/**
 * @brief Return the command-line prefix that marks a respawned child process.
 *
 * @return Stable null-terminated sentinel prefix.
 * @pre None.
 * @post No state is modified.
 * @invariant The returned storage has static lifetime and is identical for the
 * lifetime of the process, so parent and child argument parsing agree.
 * @throws Nothing.
 * @note Ownership/thread-safety: the returned pointer is borrowed static
 * storage; concurrent callers only read immutable data.
 */
inline const char* child_prefix() noexcept {
    return "--memsafe-death-test-child=";
}

/**
 * @brief Normalize a possibly null death-test case name.
 *
 * @param case_name Caller-supplied case name, or null.
 * @return `case_name` when non-null; otherwise a pointer to the empty string.
 * @pre None.
 * @post No state is modified.
 * @invariant Null case names are represented consistently in parent argument
 * construction and child sentinel matching.
 * @throws Nothing.
 * @note Ownership/thread-safety: the returned pointer is borrowed from the
 * caller or from immutable static storage.
 */
inline const char* safe_case_name(const char* case_name) noexcept {
    return case_name == nullptr ? "" : case_name;
}

/**
 * @brief Test whether a C string begins with a C string prefix.
 *
 * @param value Candidate string to inspect. Null never matches.
 * @param prefix Prefix to compare. Null never matches.
 * @return `true` when `value` starts with `prefix`; `false` otherwise.
 * @pre Non-null inputs must point to null-terminated byte strings.
 * @post No state is modified.
 * @invariant Null inputs are treated as non-matches so malformed `argv`
 * entries cannot be mistaken for death-test sentinels.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows both strings for the duration of the
 * call and owns no shared state.
 */
inline bool starts_with(const char* value, const char* prefix) noexcept {
    if (value == nullptr || prefix == nullptr) {
        return false;
    }
    const std::size_t prefix_len = std::strlen(prefix);
    return std::strncmp(value, prefix, prefix_len) == 0;
}

/**
 * @brief Test whether a byte is command-line whitespace.
 *
 * @param value Byte to classify.
 * @return `true` for ASCII/locale whitespace as reported by `std::isspace`;
 * `false` otherwise.
 * @pre `value` may be any `char` value.
 * @post No state is modified.
 * @invariant The cast to `unsigned char` avoids undefined behavior for
 * negative signed-`char` values.
 * @throws Nothing.
 * @note Ownership/thread-safety: uses only value parameters.
 */
inline bool is_space_char(char value) noexcept {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

/**
 * @brief Return a copy of a string without leading or trailing whitespace.
 *
 * @param value Text to trim.
 * @return Trimmed copy of `value`.
 * @pre None.
 * @post The input string is not modified.
 * @invariant Interior whitespace is preserved so launcher arguments such as
 * option values remain unchanged.
 * @throws `std::bad_alloc` if constructing the returned string fails.
 * @note Ownership/thread-safety: owns only local string storage.
 */
inline std::string trimmed_copy(const std::string& value) {
    std::size_t first = 0u;
    while (first < value.size() && is_space_char(value[first])) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && is_space_char(value[last - 1u])) {
        --last;
    }
    return value.substr(first, last - first);
}

/**
 * @brief Append a launcher argument when it is not empty.
 *
 * @param args Argument vector to update.
 * @param value Candidate argument text.
 * @pre `args` must be caller-owned and mutable.
 * @post `value` is copied into `args` only when it is not empty.
 * @invariant Empty fields in semicolon launcher lists cannot become empty
 * executable names or empty tool arguments.
 * @throws `std::bad_alloc` if vector growth or string copying fails.
 * @note Ownership/thread-safety: mutates only caller-owned storage.
 */
inline void append_nonempty(std::vector<std::string>& args,
                            const std::string& value) {
    if (!value.empty()) {
        args.push_back(value);
    }
}

/**
 * @brief Split a CMake-style semicolon launcher list.
 *
 * @param text Null-terminated launcher text, usually from
 * `MEMSAFE_DEATH_TEST_LAUNCHER`.
 * @return Launcher arguments with surrounding whitespace removed from each
 * field; empty fields are omitted.
 * @pre `text` may be null. Non-null input must be a null-terminated byte
 * string.
 * @post No environment state is modified.
 * @invariant Semicolon splitting mirrors the F4 `INFRA_TEST_LAUNCHER`
 * representation (`valgrind;--error-exitcode=125;--leak-check=full`).
 * @throws `std::bad_alloc` if argument storage allocation fails.
 * @note Ownership/thread-safety: borrows `text` during parsing and returns
 * owned strings.
 */
inline std::vector<std::string> split_semicolon_list(const char* text) {
    std::vector<std::string> args;
    if (text == nullptr) {
        return args;
    }

    std::string current;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ';') {
            append_nonempty(args, trimmed_copy(current));
            current.clear();
        } else {
            current.push_back(*cursor);
        }
    }
    append_nonempty(args, trimmed_copy(current));
    return args;
}

/**
 * @brief Split a small quoted launcher command line.
 *
 * @param text Null-terminated launcher text.
 * @return Whitespace-delimited arguments with single and double quotes removed.
 * @pre `text` may be null. Non-null input must be a null-terminated byte
 * string.
 * @post No environment state is modified.
 * @invariant This parser is intentionally minimal because the semicolon form
 * is the lossless F4 path; quoted text exists for convenient local overrides.
 * @throws `std::bad_alloc` if argument storage allocation fails.
 * @note Ownership/thread-safety: borrows `text` during parsing and returns
 * owned strings.
 *
 * Example:
 * @code
 * auto args = split_command_line("valgrind --error-exitcode=125");
 * @endcode
 */
inline std::vector<std::string> split_command_line(const char* text) {
    std::vector<std::string> args;
    if (text == nullptr) {
        return args;
    }

    bool in_single_quote = false;
    bool in_double_quote = false;
    std::string current;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (is_space_char(ch) && !in_single_quote && !in_double_quote) {
            append_nonempty(args, current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    append_nonempty(args, current);
    return args;
}

/**
 * @brief Parse a bounded launcher argument count from environment text.
 *
 * @param text Null-terminated decimal count.
 * @param count Output pointer receiving the parsed count.
 * @return `true` when parsing succeeds and the value is in the supported
 * range; `false` otherwise.
 * @pre `count` must be non-null.
 * @post `*count` is updated only on success.
 * @invariant Counts above 64 are rejected to keep accidental environment
 * corruption from allocating an unbounded argument vector.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads only caller-provided text.
 */
inline bool parse_launcher_count(const char* text, std::size_t* count) noexcept {
    if (text == nullptr || *text == '\0' || count == nullptr) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0ul ||
        parsed > 64ul) {
        return false;
    }
    *count = static_cast<std::size_t>(parsed);
    return true;
}

/**
 * @brief Read a launcher from indexed environment variables.
 *
 * @return Launcher arguments from `MEMSAFE_DEATH_TEST_LAUNCHER_ARGC` and
 * `MEMSAFE_DEATH_TEST_LAUNCHER_ARG0..N`; empty when the indexed form is not
 * configured completely.
 * @pre Environment variables, when present, must contain UTF-8 or native
 * narrow execution-encoding bytes accepted by the platform process launcher.
 * @post The process environment is not modified.
 * @invariant The indexed form preserves spaces and semicolons inside launcher
 * arguments, making it the most precise override for future F4 launchers.
 * @throws `std::bad_alloc` if argument storage allocation fails.
 * @note Ownership/thread-safety: `std::getenv` returns borrowed process
 * storage; this helper immediately copies values into owned strings.
 */
inline std::vector<std::string> launcher_from_indexed_env() {
    std::size_t count = 0u;
    if (!parse_launcher_count(std::getenv("MEMSAFE_DEATH_TEST_LAUNCHER_ARGC"),
                              &count)) {
        return std::vector<std::string>();
    }

    std::vector<std::string> args;
    args.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        const std::string name =
            std::string("MEMSAFE_DEATH_TEST_LAUNCHER_ARG") +
            std::to_string(index);
        const char* const value = std::getenv(name.c_str());
        if (value == nullptr || *value == '\0') {
            return std::vector<std::string>();
        }
        args.push_back(value);
    }
    return args;
}

/**
 * @brief Read a launcher from a single environment variable.
 *
 * @return Launcher arguments parsed from `MEMSAFE_DEATH_TEST_LAUNCHER`; empty
 * when the variable is unset or empty.
 * @pre If semicolons are used, the variable must contain a CMake-style list.
 * Without semicolons, the variable may use simple single or double quotes for
 * arguments containing spaces.
 * @post The process environment is not modified.
 * @invariant The semicolon path matches F4's `test_launcher` list syntax and
 * is preferred over ad-hoc shell parsing.
 * @throws `std::bad_alloc` if argument storage allocation fails.
 * @note Ownership/thread-safety: copies environment bytes before returning.
 */
inline std::vector<std::string> launcher_from_text_env() {
    const char* const text = std::getenv("MEMSAFE_DEATH_TEST_LAUNCHER");
    if (text == nullptr || *text == '\0') {
        return std::vector<std::string>();
    }
    return std::strchr(text, ';') == nullptr ? split_command_line(text)
                                             : split_semicolon_list(text);
}

/**
 * @brief Return the filename component of a path-like token.
 *
 * @param path Null-terminated path-like token.
 * @return Pointer into `path` naming the component after the final slash or
 * backslash; returns an empty static string for null input.
 * @pre Non-null input must be a null-terminated byte string.
 * @post No state is modified.
 * @invariant Both POSIX and Windows separators are accepted because Windows
 * paths can appear in launcher overrides and POSIX runners can mount Windows
 * worktrees through WSL.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns borrowed storage with the same
 * lifetime as `path`, or immutable static storage for null input.
 */
inline const char* path_basename(const char* path) noexcept {
    if (path == nullptr) {
        return "";
    }

    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    return base;
}

/**
 * @brief Compare a process-command token with the current test executable.
 *
 * @param candidate Token from `/proc/self/cmdline`.
 * @param executable_path Original `argv[0]` from `main`.
 * @return `true` when the tokens match exactly or by basename.
 * @pre `executable_path` may be null only when `argc`/`argv` are malformed.
 * @post No state is modified.
 * @invariant Basename matching is used only to identify the split point between
 * an F4 launcher prefix and the test executable in diagnostic `/proc` data.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows both strings for the duration of the
 * call.
 */
inline bool same_executable_token(const std::string& candidate,
                                  const char* executable_path) noexcept {
    if (candidate.empty() || executable_path == nullptr ||
        *executable_path == '\0') {
        return false;
    }
    if (candidate == executable_path) {
        return true;
    }

    const char* const candidate_base = path_basename(candidate.c_str());
    const char* const executable_base = path_basename(executable_path);
    return *candidate_base != '\0' && *executable_base != '\0' &&
           std::strcmp(candidate_base, executable_base) == 0;
}

/**
 * @brief Infer a POSIX launcher prefix from `/proc/self/cmdline`.
 *
 * @param argc Argument count originally received by `main`.
 * @param argv Argument vector originally received by `main`.
 * @return Tokens before the current executable in the kernel-visible command
 * line, or empty when no launcher can be inferred.
 * @pre Pass the original `argc` and `argv` from the parent process. `/proc`
 * may be absent on non-Linux POSIX systems.
 * @post No files remain open after the function returns.
 * @invariant The F4 Valgrind lane is a runtime launcher, not a compile-time
 * sanitizer flag; preserving the prefix keeps sanitizer-negative children
 * under memcheck observation.
 * @throws `std::bad_alloc` if command-line token storage allocation fails.
 * @note Ownership/thread-safety: reads process diagnostic state and returns
 * owned strings. If `/proc` is unavailable the helper quietly returns empty.
 */
inline std::vector<std::string> launcher_from_proc_cmdline(int argc,
                                                           char* const argv[]) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return std::vector<std::string>();
    }

#if defined(_WIN32)
    (void)argc;
    (void)argv;
    return std::vector<std::string>();
#else
    std::ifstream input("/proc/self/cmdline", std::ios::binary);
    if (!input) {
        return std::vector<std::string>();
    }

    std::vector<std::string> tokens;
    std::string token;
    char ch = '\0';
    while (input.get(ch)) {
        if (ch == '\0') {
            append_nonempty(tokens, token);
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    append_nonempty(tokens, token);

    for (std::size_t index = 0u; index < tokens.size(); ++index) {
        if (same_executable_token(tokens[index], argv[0])) {
            if (index == 0u) {
                return std::vector<std::string>();
            }
            return std::vector<std::string>(tokens.begin(),
                                            tokens.begin() +
                                                static_cast<std::ptrdiff_t>(index));
        }
    }
    return std::vector<std::string>();
#endif
}

/**
 * @brief Resolve the launcher prefix, if any, for a death-test respawn.
 *
 * @param argc Argument count originally received by `main`.
 * @param argv Argument vector originally received by `main`.
 * @return Launcher arguments to prepend before the child executable; empty for
 * direct respawn.
 * @pre Pass the original parent `argc`/`argv`.
 * @post No process is spawned and no environment variables are modified.
 * @invariant Explicit environment configuration wins over `/proc` inference so
 * CI can pin a launcher even when a platform hides the parent command line.
 * @throws `std::bad_alloc` if argument storage allocation fails.
 * @note Ownership/thread-safety: copies any discovered launcher tokens into
 * local storage before returning.
 */
inline std::vector<std::string> launcher_arguments(int argc,
                                                   char* const argv[]) {
    std::vector<std::string> launcher = launcher_from_indexed_env();
    if (!launcher.empty()) {
        return launcher;
    }

    launcher = launcher_from_text_env();
    if (!launcher.empty()) {
        return launcher;
    }

    return launcher_from_proc_cmdline(argc, argv);
}

/**
 * @brief Build the argument vector used to respawn the current executable.
 *
 * @param argc Argument count originally received by `main`.
 * @param argv Argument vector originally received by `main`.
 * @param case_name Death-test case name to encode in the child sentinel.
 * @return Owned command strings for the child process; empty when the current
 * executable path is unavailable. When an F4 launcher is detected, the returned
 * vector begins with that launcher and its options followed by the executable
 * and child arguments.
 * @pre Pass the original `argc` and `argv` from the parent process. Existing
 * death-test sentinels are allowed and are removed.
 * @post The returned vector owns all argument storage needed until
 * `spawn_and_wait` creates platform-specific raw pointers.
 * @invariant At most one sentinel for this helper appears in the returned
 * vector, preventing nested death-test routing from stale command-line state.
 * @throws `std::bad_alloc` if vector or string allocation fails.
 * @note Ownership/thread-safety: copies `argv` contents into local storage and
 * does not mutate caller-owned argument memory.
 */
inline std::vector<std::string> child_arguments(int argc,
                                                char* const argv[],
                                                const char* case_name) {
    std::vector<std::string> target_args;
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return target_args;
    }

    target_args.reserve(static_cast<std::size_t>(argc) + 1u);
    target_args.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && !starts_with(argv[i], child_prefix())) {
            target_args.push_back(argv[i]);
        }
    }
    target_args.push_back(std::string(child_prefix()) + safe_case_name(case_name));

    std::vector<std::string> launcher = launcher_arguments(argc, argv);
    if (launcher.empty()) {
        return target_args;
    }

    /*
     * F4's optional Valgrind lane is expressed as a runtime `test_launcher`.
     * The test executable sees only its own argv, so the death helper must
     * re-create the prefix before the target executable to keep sanitizer-only
     * negative tests under memcheck observation.
     */
    std::vector<std::string> args;
    args.reserve(launcher.size() + target_args.size());
    args.insert(args.end(), launcher.begin(), launcher.end());
    args.insert(args.end(), target_args.begin(), target_args.end());
    return args;
}

/**
 * @brief Construct a launch-failure result with a diagnostic and platform code.
 *
 * @param message Human-readable launch or wait failure reason.
 * @param code Platform error code, commonly `errno`.
 * @return `death_result` classified as `termination_kind::launch_error`.
 * @pre `message` should describe an infrastructure failure, not an expected
 * abnormal child termination.
 * @post The returned result owns its diagnostic string and reports
 * `passed() == false`.
 * @invariant Launch errors never satisfy the F4 death-test oracle because the
 * child body did not run to the requested abnormal path.
 * @throws `std::bad_alloc` if copying `message` fails.
 * @note Ownership/thread-safety: uses only local state.
 */
inline death_result launch_error_result(const std::string& message, int code) {
    death_result result;
    result.launched = false;
    result.abnormal = false;
    result.kind = termination_kind::launch_error;
    result.exit_code = code;
    result.message = message;
    return result;
}

#if defined(_WIN32)

/**
 * @brief Spawn the selected child process on Windows and wait for completion.
 *
 * @param args Owned command strings beginning with the executable path or a
 * launcher command.
 * @return Parent-side `death_result` describing the child exit status.
 * @pre `args[0]` must name an executable path or PATH-searchable command
 * accepted by `_spawnvp`.
 * @post The child has completed or a launch error is reported; no Windows
 * process handle is retained by the helper.
 * @invariant Nonzero child exit status is classified as abnormal, matching
 * F2's death-test and sanitizer-negative oracle under the F4 exit-code model.
 * @throws `std::bad_alloc` if building the transient raw argument vector fails.
 * @note Ownership/thread-safety: borrows `args` during `_spawnvp`; call from
 * `main()` before starting worker threads for deterministic test behavior.
 */
inline death_result spawn_and_wait(const std::vector<std::string>& args) {
    if (args.empty()) {
        return launch_error_result("death test has no executable path", EINVAL);
    }

    std::vector<const char*> raw_args;
    raw_args.reserve(args.size() + 1u);
    for (std::vector<std::string>::const_iterator it = args.begin();
         it != args.end(); ++it) {
        raw_args.push_back(it->c_str());
    }
    raw_args.push_back(nullptr);

    const intptr_t code = _spawnvp(_P_WAIT, raw_args[0], raw_args.data());
    if (code == static_cast<intptr_t>(-1)) {
        return launch_error_result("Windows _spawnvp failed", errno);
    }

    death_result result;
    result.launched = true;
    result.exit_code = static_cast<int>(code);
    result.abnormal = code != static_cast<intptr_t>(0);
    result.kind = result.abnormal ? termination_kind::nonzero_exit
                                  : termination_kind::normal_exit;
    result.message = result.abnormal ? "child exited with nonzero status"
                                     : "child exited normally";
    return result;
}

#else

/**
 * @brief Wait for a POSIX child process while retrying interrupted waits.
 *
 * @param pid Child process id returned by `fork`.
 * @param status Output pointer receiving the `waitpid` status word.
 * @return The `waitpid` return value after retrying `EINTR`.
 * @pre `pid` must identify a child process of the caller and `status` must be
 * non-null.
 * @post On success, `*status` contains the child termination status.
 * @invariant Signal interruption cannot cause a false death-test launch error.
 * @throws Nothing.
 * @note Ownership/thread-safety: operates on one caller-owned status word and
 * owns no shared state.
 */
inline int wait_for_child(pid_t pid, int* status) noexcept {
    int wait_result = 0;
    do {
        wait_result = ::waitpid(pid, status, 0);
    } while (wait_result < 0 && errno == EINTR);
    return wait_result;
}

/**
 * @brief Fork, exec, and wait for the selected POSIX death-test child.
 *
 * @param args Owned command strings beginning with the current executable path
 * or a launcher command.
 * @return Parent-side `death_result` describing signal termination, nonzero
 * exit, normal exit, or launch failure.
 * @pre `args[0]` must name an executable path or PATH-searchable command
 * accepted by `execvp`.
 * @post The parent has waited for the child and closed the exec-status pipe.
 * @invariant `execvp` failure is reported as `launch_error` through a
 * close-on-exec pipe rather than being misclassified as an expected abnormal
 * child death.
 * @throws `std::bad_alloc` before `fork` if diagnostic construction fails; the
 * child path exits directly after an `execvp` failure.
 * @note Ownership/thread-safety: uses process-local descriptors and should be
 * called before worker threads are started.
 */
inline death_result spawn_and_wait(const std::vector<std::string>& args) {
    if (args.empty()) {
        return launch_error_result("death test has no executable path", EINVAL);
    }

    int exec_pipe[2] = {-1, -1};
    if (::pipe(exec_pipe) != 0) {
        return launch_error_result("POSIX pipe failed before fork", errno);
    }

    const int flags = ::fcntl(exec_pipe[1], F_GETFD);
    if (flags >= 0) {
        const int set_flags = ::fcntl(exec_pipe[1], F_SETFD, flags | FD_CLOEXEC);
        (void)set_flags;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(exec_pipe[0]);
        ::close(exec_pipe[1]);
        return launch_error_result("POSIX fork failed", saved_errno);
    }

    if (pid == 0) {
        ::close(exec_pipe[0]);
        std::vector<char*> raw_args;
        raw_args.reserve(args.size() + 1u);
        for (std::vector<std::string>::const_iterator it = args.begin();
             it != args.end(); ++it) {
            raw_args.push_back(const_cast<char*>(it->c_str()));
        }
        raw_args.push_back(nullptr);

        ::execvp(raw_args[0], raw_args.data());

        /*
         * F2 sanitizer-negative tests must not pass because exec failed. The
         * pipe is close-on-exec, so any bytes observed by the parent mean this
         * branch ran rather than the requested test executable.
         */
        const int saved_errno = errno;
        const ssize_t ignored = ::write(exec_pipe[1], &saved_errno, sizeof(saved_errno));
        (void)ignored;
        ::_exit(127);
    }

    ::close(exec_pipe[1]);
    int exec_errno = 0;
    ssize_t read_count = 0;
    do {
        read_count = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
    } while (read_count < 0 && errno == EINTR);
    ::close(exec_pipe[0]);

    int status = 0;
    if (wait_for_child(pid, &status) < 0) {
        return launch_error_result("POSIX waitpid failed", errno);
    }

    if (read_count > 0) {
        return launch_error_result("POSIX execvp failed in child", exec_errno);
    }

    death_result result;
    result.launched = true;
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

} // namespace detail

/**
 * @brief Detect whether the current process is the selected death-test child.
 *
 * @param argc Argument count received by `main`.
 * @param argv Argument vector received by `main`; entries are inspected but
 * not modified.
 * @param case_name Stable case name associated with the child body.
 * @return `true` when `argv` contains this helper's child sentinel for
 * `case_name`; `false` otherwise.
 *
 * @pre Pass the original `argc` and `argv` from `main`. `case_name` may be
 * null, in which case the empty case name is used.
 * @post No process is spawned and no argument storage is modified.
 * @invariant The match is exact after the sentinel prefix, preventing one
 * death-test case from accidentally running another case's child body.
 * @throws Nothing.
 * @note Ownership/thread-safety: the function borrows `argv` only for the
 * duration of the call and has no shared state.
 *
 * Example:
 * @code
 * if (memsafe::test_support::death::child_requested(argc, argv, "abort")) {
 *     std::abort();
 * }
 * @endcode
 */
inline bool child_requested(int argc,
                            char* const argv[],
                            const char* case_name) noexcept {
    const char* const expected_name = detail::safe_case_name(case_name);
    const char* const prefix = detail::child_prefix();
    const std::size_t prefix_len = std::strlen(prefix);
    for (int i = 1; i < argc; ++i) {
        const char* const arg = argv == nullptr ? nullptr : argv[i];
        if (detail::starts_with(arg, prefix) &&
            std::strcmp(arg + prefix_len, expected_name) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Re-spawn the current executable and expect the selected child body to
 * terminate abnormally.
 *
 * @tparam ChildBody Callable with signature compatible with `void()`.
 * @param argc Argument count received by the parent or child `main`.
 * @param argv Argument vector received by the parent or child `main`.
 * @param case_name Stable case name used to route the child invocation.
 * @param child_body Callable executed only inside the child process.
 * @return In the parent, a `death_result` describing the child termination. In
 * the child, this function does not return.
 *
 * @pre Call from a standalone F4 test executable. `argv[0]` must name the
 * current executable in a form the platform can execute. `child_body` must be
 * safe to run in a freshly exec-spawned copy of the test process.
 * @post The parent has waited for the child and owns no remaining process
 * handles. The child exits with status 0 if `child_body` returns normally, so
 * a normal return is reported as a failed death test by the parent.
 * @invariant Launch errors never satisfy `death_result::passed()`.
 * @throws `std::bad_alloc` if temporary argument construction fails in the
 * parent. Exceptions escaping `child_body` are converted into an abnormal child
 * exit with code 101.
 * @note Ownership/thread-safety: the function owns temporary argument copies
 * in the parent. Call it before starting threads because POSIX uses `fork()`
 * for the exec handoff.
 *
 * Example:
 * @code
 * int main(int argc, char** argv) {
 *     auto result = memsafe::test_support::death::expect_abnormal_child(
 *         argc, argv, "abort-path", [] { std::abort(); });
 *     CHECK(result.passed());
 *     RUN_TESTS("death_example");
 * }
 * @endcode
 */
template <typename ChildBody>
death_result expect_abnormal_child(int argc,
                                   char* const argv[],
                                   const char* case_name,
                                   ChildBody&& child_body) {
    if (child_requested(argc, argv, case_name)) {
        try {
            std::forward<ChildBody>(child_body)();
        } catch (...) {
            std::_Exit(101);
        }
        std::_Exit(0);
    }

    const std::vector<std::string> args =
        detail::child_arguments(argc, argv, case_name);
    return detail::spawn_and_wait(args);
}

} // namespace death
} // namespace test_support
} // namespace memsafe

#endif /* MEMSAFE_TEST_SUPPORT_DEATH_TEST_HPP */
