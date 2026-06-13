/**
 * @file test_scope_uaf_sanitizer.cpp
 * @brief Sanitizer/death-test oracle for raw pointers returned by `memsafe::Scope`.
 *
 * @details
 * Work package: CPP_MEMSAFE-0320-TEST.
 *
 * Purpose:
 * - Exercise the finalized CPP_MEMSAFE-0300-FUNC `memsafe::Scope` artifact in
 *   the only lane where a raw post-scope pointer access can be asserted safely:
 *   an external memory diagnostic oracle.
 * - Use `testing/tests/support/death_test.hpp` from CPP_MEMSAFE-0035-TEST to
 *   re-spawn this test executable as a child process.
 * - In the child, preserve a raw pointer returned by `Scope::create`, destroy
 *   the owning scope, then read and write the stale pointer so ASan, HWASan, or
 *   Valgrind memcheck reports the lifetime error.
 * - In the parent, accept only the expected abnormal sanitizer termination or
 *   the configured Valgrind memcheck error code.
 *
 * Key invariants:
 * - `Scope::create<T>` returns an unchecked raw `T*` in v1. This test never
 *   expects a `MEMSAFE_ON_VIOLATION` path for stale raw-pointer access.
 * - Non-sanitized lanes execute only a normal in-scope raw-pointer smoke path
 *   and then report a skip for the negative stale-pointer oracle.
 * - UBSan by itself is not a sufficient oracle for this case; the compile-time
 *   sanitizer branch is selected only for ASan or HWASan instrumentation.
 * - The Valgrind branch requires the child to exit with the F4 lane's
 *   `--error-exitcode=125` memcheck status.
 *
 * Ownership and thread-safety:
 * - `memsafe::Scope` owns every object produced by `create`. Raw pointers in
 *   this file are non-owning and are used on a single thread.
 * - The death-test spawn occurs from `main` before any worker threads exist,
 *   matching the helper's POSIX `fork`/`exec` requirements.
 */
#include "../test_harness.hpp"
#include "support/death_test.hpp"

#include <memsafe/scope.hpp>

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>

#if defined(__has_include)
#  if __has_include(<valgrind/valgrind.h>)
#    include <valgrind/valgrind.h>
#  endif
#endif

namespace {

/**
 * @brief Compile-time selector for ASan or HWASan instrumented builds.
 *
 * @pre Evaluated after compiler sanitizer feature macros are available.
 * @post The value is `true` only when AddressSanitizer or
 * HardwareAddressSanitizer is enabled for this translation unit.
 * @invariant UndefinedBehaviorSanitizer alone deliberately leaves this value
 * false because F2 Scope case 1 says UBSan is not a sufficient pass/fail
 * oracle for raw post-scope pointer access.
 * @throws Nothing; this is a constant expression.
 * @note Ownership/thread-safety: the constant owns no storage that can be
 * mutated at runtime.
 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(hwaddress_sanitizer)
constexpr bool k_compiled_with_memory_sanitizer_oracle = true;
#  elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_HWADDRESS__)
constexpr bool k_compiled_with_memory_sanitizer_oracle = true;
#  else
constexpr bool k_compiled_with_memory_sanitizer_oracle = false;
#  endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_HWADDRESS__)
constexpr bool k_compiled_with_memory_sanitizer_oracle = true;
#else
constexpr bool k_compiled_with_memory_sanitizer_oracle = false;
#endif

/// Stable death-test case name used by parent and child routing.
const char* const k_death_case = "scope-raw-pointer-use-after-scope";

/// Environment variable consumed by `death_test.hpp` for launcher preservation.
const char* const k_death_test_launcher_env = "MEMSAFE_DEATH_TEST_LAUNCHER";

/// F4 Valgrind lane launcher encoded as the helper's semicolon-list format.
const char* const k_valgrind_launcher = "valgrind;--error-exitcode=125;--leak-check=full";

/// Valgrind memcheck error status configured in `testing/infra_lanes.json`.
constexpr int k_valgrind_error_exit_code = 125;

/// Volatile sink that makes the stale access a required runtime memory access.
volatile int g_scope_uaf_observation = 0;

/**
 * @brief Scope-owned payload used by the stale raw-pointer oracle.
 *
 * @details
 * The type is intentionally small and trivially destructible so the child
 * failure is dominated by the raw pointer lifetime error rather than by any
 * destructor or allocation side effect. The non-static data fields give ASan
 * and Valgrind concrete bytes to read and write after `Scope` releases the
 * object's storage.
 *
 * Example:
 * @code
 * memsafe::Scope scope;
 * scope_payload* payload = scope.create<scope_payload>(17);
 * payload->mix(5);
 * @endcode
 *
 * @pre Objects are constructed only through `memsafe::Scope::create`.
 * @post Construction initializes both public payload fields deterministically.
 * @invariant `mirror` is derived from `value` and a fixed mask until `mix`
 * updates both fields together.
 * @throws Nothing; construction and member functions are `noexcept`.
 * @note Ownership/thread-safety: instances are owned by one `memsafe::Scope`;
 * raw pointers are non-owning and this test uses them on one thread.
 */
struct scope_payload final {
    /**
     * @brief Construct a payload with a deterministic mirrored field.
     *
     * @param initial Initial value stored in `value`.
     * @return Constructors do not return a value.
     * @pre No precondition; all integer values are accepted.
     * @post `value == initial` and `mirror == (initial ^ 0x5A5A)`.
     * @invariant Both fields are initialized before any raw pointer is exposed
     * by `Scope::create`.
     * @throws Nothing.
     * @note Ownership/thread-safety: construction occurs in scope-owned storage
     * before the returned raw pointer reaches the caller.
     */
    explicit scope_payload(int initial) noexcept
        : value(initial),
          mirror(initial ^ 0x5A5A) {}

    /**
     * @brief Mutate both payload fields while the object is still alive.
     *
     * @param delta Amount to add to `value`.
     * @return No value.
     * @pre The object is alive and the test inputs keep signed arithmetic in
     * range.
     * @post `value` is increased by `delta` and `mirror` is updated from the
     * new value.
     * @invariant The field relationship remains deterministic for the
     * in-scope smoke checks.
     * @throws Nothing.
     * @note Ownership/thread-safety: the mutation is unsynchronized and is used
     * only from the single test thread.
     */
    void mix(int delta) noexcept {
        value += delta;
        mirror = value ^ 0x5A5A;
    }

    /**
     * @brief Return a deterministic checksum of the live payload fields.
     *
     * @return XOR of `value` and `mirror`.
     * @pre The object is alive.
     * @post The object is unchanged.
     * @invariant For freshly mixed objects, the checksum remains the fixed
     * mask used by the constructor and `mix`.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; callers must respect the
     * owning `memsafe::Scope` lifetime.
     */
    [[nodiscard]] int checksum() const noexcept {
        return value ^ mirror;
    }

    /// Primary integer payload read after scope destruction by the child.
    int value;
    /// Mirrored payload used by the safe in-scope smoke path.
    int mirror;
};

/**
 * @brief RAII guard that temporarily sets one process environment variable.
 *
 * @details
 * The Valgrind lane is expressed by the F4 harness as a runtime launcher. This
 * guard lets the parent explicitly tell `death_test.hpp` to re-use that
 * launcher for the child, which keeps the stale-pointer access under memcheck
 * observation even on systems where `/proc/self/cmdline` hides the original
 * launcher prefix from the instrumented process.
 *
 * Example:
 * @code
 * scoped_environment_value launcher("MEMSAFE_DEATH_TEST_LAUNCHER",
 *                                   "valgrind;--error-exitcode=125");
 * CHECK(launcher.applied());
 * @endcode
 *
 * @pre `env_name` and `value` are non-null pointers to null-terminated strings.
 * @post When `applied()` is true, destruction restores the previous value or
 * removes the variable if it was initially absent.
 * @invariant The guard mutates exactly one environment variable.
 * @throws `std::bad_alloc` if saving the previous environment value requires
 * allocation and that allocation fails.
 * @note Ownership/thread-safety: process environment mutation is process-global
 * and not thread-safe; this test performs it before starting any threads.
 */
class scoped_environment_value final {
public:
    /**
     * @brief Set an environment variable and remember its previous value.
     *
     * @param env_name Name of the environment variable to update.
     * @param value New value to assign while this guard is alive.
     * @return Constructors do not return a value.
     * @pre `env_name != nullptr`, `value != nullptr`, and no other thread is
     * concurrently mutating the process environment.
     * @post `applied()` reports whether the requested assignment succeeded.
     * @invariant The saved old value corresponds to `env_name`.
     * @throws `std::bad_alloc` if copying the prior value fails.
     * @note Ownership/thread-safety: the guard owns the saved old value string
     * and mutates process-global environment state only in this thread.
     */
    scoped_environment_value(const char* env_name, const char* value)
        : name_(env_name),
          had_old_value_(false),
          old_value_(),
          applied_(false) {
        const char* const old_value = std::getenv(env_name);
        had_old_value_ = old_value != nullptr;
        if (had_old_value_) {
            old_value_ = old_value;
        }
        applied_ = set_value(env_name, value);
    }

    /**
     * @brief Copy construction is disabled to keep restoration single-owner.
     *
     * @param other Guard that would otherwise be copied.
     * @return No value; this overload is deleted.
     * @pre Not available. Copying is a compile-time error.
     * @post No second guard is created for the same saved value.
     * @invariant Exactly one guard instance restores its environment mutation.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: disabling copies avoids double restoration
     * of process-global environment state.
     */
    scoped_environment_value(const scoped_environment_value& other) = delete;

    /**
     * @brief Copy assignment is disabled to keep restoration single-owner.
     *
     * @param other Guard that would otherwise be assigned.
     * @return No value; this overload is deleted.
     * @pre Not available. Copy assignment is a compile-time error.
     * @post Existing guard ownership is not replaced.
     * @invariant Exactly one guard instance restores its environment mutation.
     * @throws Nothing at runtime because overload resolution rejects this
     * deleted function.
     * @note Ownership/thread-safety: disabling assignment avoids ambiguous
     * restoration of process-global environment state.
     */
    scoped_environment_value& operator=(const scoped_environment_value& other) = delete;

    /**
     * @brief Restore the environment variable changed by this guard.
     *
     * @return Destructors do not return a value.
     * @pre No other thread is concurrently mutating the same environment
     * variable.
     * @post If the constructor assignment succeeded, the previous value is
     * restored or the variable is removed when it was initially absent.
     * @invariant A failed constructor assignment is not restored because it did
     * not establish a temporary value.
     * @throws Nothing; restoration failures are intentionally ignored so the
     * test verdict can be reported through CHECKs already recorded by the
     * parent.
     * @note Ownership/thread-safety: process environment mutation is
     * process-global and this test remains single-threaded.
     */
    ~scoped_environment_value() noexcept {
        if (!applied_) {
            return;
        }

        if (had_old_value_) {
            (void)set_value(name_, old_value_.c_str());
        } else {
            (void)unset_value(name_);
        }
    }

    /**
     * @brief Report whether the temporary assignment succeeded.
     *
     * @return `true` when the constructor set the requested value; `false`
     * otherwise.
     * @pre The guard object is alive.
     * @post The guard is unchanged.
     * @invariant The result remains stable for the lifetime of the guard.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; no environment state is
     * accessed.
     */
    [[nodiscard]] bool applied() const noexcept {
        return applied_;
    }

private:
    /**
     * @brief Assign one environment variable.
     *
     * @param env_name Name of the variable to set.
     * @param value New value to assign.
     * @return `true` when the platform call succeeds; otherwise `false`.
     * @pre Both pointers are non-null and name a valid environment assignment.
     * @post On success, future `std::getenv(env_name)` calls observe `value`.
     * @invariant Windows and POSIX use their native environment mutation APIs.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutates process-global environment state.
     */
    static bool set_value(const char* env_name, const char* value) noexcept {
#if defined(_WIN32)
        return _putenv_s(env_name, value) == 0;
#else
        return ::setenv(env_name, value, 1) == 0;
#endif
    }

    /**
     * @brief Remove one environment variable.
     *
     * @param env_name Name of the variable to remove.
     * @return `true` when the platform call succeeds; otherwise `false`.
     * @pre `env_name` is non-null and names a valid environment variable.
     * @post On success, future `std::getenv(env_name)` calls return null until
     * another value is assigned.
     * @invariant Windows receives an empty value because `_putenv_s` uses that
     * form to remove a variable.
     * @throws Nothing.
     * @note Ownership/thread-safety: mutates process-global environment state.
     */
    static bool unset_value(const char* env_name) noexcept {
#if defined(_WIN32)
        return _putenv_s(env_name, "") == 0;
#else
        return ::unsetenv(env_name) == 0;
#endif
    }

    /// Name of the single environment variable managed by this guard.
    const char* name_;
    /// Whether `name_` had a value before this guard changed it.
    bool had_old_value_;
    /// Owned copy of the previous value, used only when `had_old_value_` is true.
    std::string old_value_;
    /// Whether the constructor successfully installed the temporary value.
    bool applied_;
};

/**
 * @brief Return a printable name for a child termination classification.
 *
 * @param kind Termination classification returned by `death_test.hpp`.
 * @return Static text naming `kind`.
 * @pre `kind` may be any value representable by the enum.
 * @post No state is modified.
 * @invariant Unknown future enum values produce `"unknown"` rather than
 * falling through to undefined behavior.
 * @throws Nothing.
 * @note Ownership/thread-safety: returned strings have static storage duration
 * and are immutable.
 */
const char* termination_name(memsafe::test_support::death::termination_kind kind) noexcept {
    using memsafe::test_support::death::termination_kind;

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
 * @brief Emit diagnostic context for a parent-observed death-test result.
 *
 * @param oracle_name Human-readable oracle label, such as `"ASan/HWASan"` or
 * `"Valgrind"`.
 * @param result Result returned to the parent by `expect_abnormal_child`.
 * @return No value.
 * @pre `result` was produced in the parent process.
 * @post One diagnostic line is written to stderr.
 * @invariant Diagnostics are emitted before CHECKs so runner logs show the
 * child status even when an assertion fails.
 * @throws Nothing intentionally; C stdio failures are ignored.
 * @note Ownership/thread-safety: reads immutable result fields and writes to
 * the process stderr stream from one thread.
 */
void emit_child_result(
    const char* oracle_name,
    const memsafe::test_support::death::death_result& result) noexcept {
    std::fprintf(stderr,
                 "%s scope-UAF child result: launched=%d abnormal=%d "
                 "kind=%s exit_code=%d signal=%d message=%s\n",
                 oracle_name == nullptr ? "unknown" : oracle_name,
                 result.launched ? 1 : 0,
                 result.abnormal ? 1 : 0,
                 termination_name(result.kind),
                 result.exit_code,
                 result.signal_number,
                 result.message.empty() ? "(none)" : result.message.c_str());
}

/**
 * @brief Inspect Linux process mappings for Valgrind preload artifacts.
 *
 * @return `true` when `/proc/self/maps` contains common Valgrind mapping
 * tokens; otherwise `false`.
 * @pre No precondition. Non-Linux platforms return false without opening a
 * file.
 * @post No state is modified and any file handle is closed before return.
 * @invariant This fallback is advisory only; the official `RUNNING_ON_VALGRIND`
 * client request wins when the Valgrind header is available.
 * @throws `std::bad_alloc` if line-buffer growth fails while reading maps.
 * @note Ownership/thread-safety: owns only local file and string objects.
 */
bool proc_maps_show_valgrind() {
#if defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("vgpreload") != std::string::npos ||
            line.find("/valgrind/") != std::string::npos) {
            return true;
        }
    }
#endif
    return false;
}

/**
 * @brief Detect whether the current process is already under Valgrind.
 *
 * @return `true` when the Valgrind client request or Linux mapping fallback
 * indicates memcheck instrumentation; otherwise `false`.
 * @pre No precondition.
 * @post No process state is modified.
 * @invariant The result is used only to decide whether the Valgrind-specific
 * death-test oracle should run; non-Valgrind C++17 lanes must skip it.
 * @throws `std::bad_alloc` if the mapping fallback allocates and fails.
 * @note Ownership/thread-safety: reads process diagnostic state and uses no
 * shared mutable test state.
 */
bool running_under_valgrind() {
#if defined(RUNNING_ON_VALGRIND)
    if (RUNNING_ON_VALGRIND != 0) {
        return true;
    }
#endif

    return proc_maps_show_valgrind();
}

/**
 * @brief Perform the intentional stale raw-pointer access in the child.
 *
 * @return No value when no external oracle terminates the process.
 * @pre The caller is the death-test child in a lane with ASan, HWASan, or
 * Valgrind memcheck active.
 * @post Under the expected oracle, the process terminates before returning to
 * the death-test helper. If it returns, the parent observes a normal child exit
 * and fails the test.
 * @invariant No memsafe violation hook is called; F2 Scope case 1 assigns this
 * raw-pointer error to external memory diagnostics only.
 * @throws `std::bad_alloc` if the tiny scope allocation fails; the death-test
 * helper converts escaping exceptions to a distinct abnormal exit that the
 * parent does not accept as a sanitizer pass.
 * @note Ownership/thread-safety: the raw pointer is non-owning and intentionally
 * used after the owning `Scope` has destroyed and deallocated the object.
 */
void access_raw_pointer_after_scope_destroyed() {
    volatile scope_payload* volatile dangling = nullptr;

    {
        memsafe::Scope scope;
        scope_payload* const created = scope.create<scope_payload>(0x5C0E);
        created->mix(7);
        g_scope_uaf_observation = created->checksum();
        dangling = created;
    }

    /*
     * F2 Scope case 1 requires the raw pointer to outlive the scope only in the
     * negative child. The volatile-qualified pointer and sink force real memory
     * accesses so optimizing builds cannot erase the sanitizer/memcheck oracle.
     */
    const int observed = dangling->value;
    dangling->value = observed + 1;
    g_scope_uaf_observation = observed;

    std::fprintf(stderr,
                 "post-scope raw-pointer access returned without oracle failure: %d\n",
                 observed);
}

/**
 * @brief Execute the stale-pointer child body immediately when child mode is
 * requested.
 *
 * @param argc Argument count received by `main`.
 * @param argv Argument vector received by `main`.
 * @return No value when the process is the parent. In the selected child, this
 * function does not return because `expect_abnormal_child` exits the child.
 * @pre Pass the original platform arguments before running parent-only smoke
 * checks.
 * @post Child-mode executions either terminate through the external oracle or
 * through the death helper's normal/exception child exit path; parent
 * executions are unchanged.
 * @invariant The child reaches the intentional stale access before any
 * parent-only CHECK state can influence the death-test verdict.
 * @throws `std::bad_alloc` if the death helper's child dispatch allocates and
 * fails before it exits; such failures surface as process failures rather than
 * accepted sanitizer diagnostics.
 * @note Ownership/thread-safety: dispatch happens at program start on one
 * thread and owns no persistent resources.
 */
void run_child_body_if_requested(int argc, char** argv) {
    if (memsafe::test_support::death::child_requested(argc, argv, k_death_case)) {
        (void)memsafe::test_support::death::expect_abnormal_child(
            argc, argv, k_death_case, &access_raw_pointer_after_scope_destroyed);
    }
}

/**
 * @brief Exercise ordinary in-scope raw pointer use in all lanes.
 *
 * @return No value.
 * @pre The executable is running in the parent process.
 * @post Harness failures are recorded if basic in-scope access regresses.
 * @invariant This function never uses a pointer after its owning `Scope` has
 * been destroyed, so it remains valid in non-sanitized lanes.
 * @throws `std::bad_alloc` if scope storage allocation fails unexpectedly.
 * @note Ownership/thread-safety: all state is automatic storage and used on one
 * thread.
 */
void exercise_in_scope_raw_pointer_path() {
    memsafe::Scope scope;
    CHECK(scope.size() == 0U);

    scope_payload* const first = scope.create<scope_payload>(11);
    scope_payload* const second = scope.create<scope_payload>(29);

    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(first != second);
    CHECK(scope.size() == 2U);

    CHECK(first->value == 11);
    CHECK(second->value == 29);
    CHECK(first->checksum() == 0x5A5A);
    CHECK(second->checksum() == 0x5A5A);

    first->mix(3);
    second->mix(5);

    CHECK(first->value == 14);
    CHECK(second->value == 34);
    CHECK(first->checksum() == 0x5A5A);
    CHECK(second->checksum() == 0x5A5A);
}

/**
 * @brief Return whether a sanitizer child result matches the expected oracle.
 *
 * @param result Parent-side death-test result.
 * @return `true` when the child launched and terminated abnormally for a
 * reason other than the helper's exception escape code; otherwise `false`.
 * @pre `result` was returned by `expect_abnormal_child`.
 * @post The result object is unchanged.
 * @invariant A normal child exit means the stale access was not diagnosed and
 * is never accepted.
 * @throws Nothing.
 * @note Ownership/thread-safety: observer only.
 */
bool expected_sanitizer_result(
    const memsafe::test_support::death::death_result& result) noexcept {
    using memsafe::test_support::death::termination_kind;

    if (!result.launched || !result.abnormal) {
        return false;
    }

    return !(result.kind == termination_kind::nonzero_exit &&
             result.exit_code == 101);
}

/**
 * @brief Return whether a Valgrind child result matches the memcheck oracle.
 *
 * @param result Parent-side death-test result.
 * @return `true` when the child launched and exited with Valgrind's configured
 * memcheck error code; otherwise `false`.
 * @pre `result` was returned by `expect_abnormal_child` while the launcher was
 * set to the Valgrind lane command.
 * @post The result object is unchanged.
 * @invariant Signals and arbitrary nonzero exits are not accepted for this
 * branch because the F4 lane pins `--error-exitcode=125`.
 * @throws Nothing.
 * @note Ownership/thread-safety: observer only.
 */
bool expected_valgrind_result(
    const memsafe::test_support::death::death_result& result) noexcept {
    return result.launched && result.abnormal &&
           result.kind == memsafe::test_support::death::termination_kind::nonzero_exit &&
           result.exit_code == k_valgrind_error_exit_code;
}

/**
 * @brief Run the ASan/HWASan death-test oracle from the parent.
 *
 * @param argc Argument count received by parent `main`.
 * @param argv Argument vector received by parent `main`.
 * @return No value.
 * @pre The translation unit is compiled with ASan or HWASan instrumentation.
 * @post Harness failures are recorded if the child does not terminate through
 * the external sanitizer diagnostic path.
 * @invariant The parent is the only process that records CHECK verdicts.
 * @throws `std::bad_alloc` if death-test argument construction fails.
 * @note Ownership/thread-safety: spawns the child before any worker threads are
 * created.
 */
void run_sanitizer_oracle(int argc, char** argv) {
    const memsafe::test_support::death::death_result result =
        memsafe::test_support::death::expect_abnormal_child(
            argc, argv, k_death_case, &access_raw_pointer_after_scope_destroyed);

    emit_child_result("ASan/HWASan", result);
    CHECK(result.launched);
    CHECK(expected_sanitizer_result(result));
}

/**
 * @brief Run the Valgrind memcheck death-test oracle from the parent.
 *
 * @param argc Argument count received by parent `main`.
 * @param argv Argument vector received by parent `main`.
 * @return No value.
 * @pre The parent process is already running under Valgrind, proving the
 * optional `valgrind-cxx17` lane is active.
 * @post Harness failures are recorded if the child does not exit with the
 * configured memcheck error code.
 * @invariant The launcher environment is restored before this function returns.
 * @throws `std::bad_alloc` if saving the previous launcher environment value or
 * building death-test arguments fails.
 * @note Ownership/thread-safety: mutates the process environment and spawns the
 * child while the test remains single-threaded.
 */
void run_valgrind_oracle(int argc, char** argv) {
    scoped_environment_value launcher(k_death_test_launcher_env,
                                      k_valgrind_launcher);
    CHECK(launcher.applied());
    if (!launcher.applied()) {
        return;
    }

    const memsafe::test_support::death::death_result result =
        memsafe::test_support::death::expect_abnormal_child(
            argc, argv, k_death_case, &access_raw_pointer_after_scope_destroyed);

    emit_child_result("Valgrind", result);
    CHECK(result.launched);
    CHECK(expected_valgrind_result(result));
}

/**
 * @brief Report that the stale-pointer negative oracle is unavailable.
 *
 * @return No value.
 * @pre The parent process is not compiled with ASan/HWASan and is not running
 * under Valgrind.
 * @post One skip line is written to stdout and no harness failure is recorded.
 * @invariant The skip path does not access a post-scope raw pointer and does
 * not assert any `MEMSAFE_ON_VIOLATION` behavior.
 * @throws Nothing intentionally; C stdio failures are ignored.
 * @note Ownership/thread-safety: writes to stdout from the single test thread.
 */
void report_no_external_oracle() noexcept {
    std::fprintf(stdout,
                 "SKIP: no ASan/HWASan/Valgrind oracle; raw Scope pointers "
                 "are not a MEMSAFE_ON_VIOLATION path in v1.\n");
}

} // namespace

/**
 * @brief Run the Scope raw-pointer use-after-scope sanitizer/death test.
 *
 * @param argc Argument count supplied by the platform runtime.
 * @param argv Argument vector supplied by the platform runtime.
 * @retval 0 The in-scope smoke path passed, and the sanitizer/memcheck oracle
 * either produced the expected child diagnostic path or was correctly skipped
 * because no external oracle was present.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built as a standalone F4 runtime test with
 * `memsafe::Scope` and `testing/tests/support/death_test.hpp` available.
 * @post The parent process reports the final test verdict through the normal
 * exit-code contract. Child-mode executions never reach `RUN_TESTS`.
 * @invariant Only ASan/HWASan or Valgrind lanes intentionally access the stale
 * raw pointer; all other lanes avoid false `MEMSAFE_ON_VIOLATION` assertions.
 * @throws Nothing intentionally; unexpected exceptions surface as process
 * failures under the runner's exit-code model.
 * @note Ownership/thread-safety: all test state is process-local and
 * single-threaded.
 */
int main(int argc, char** argv) {
    run_child_body_if_requested(argc, argv);

    exercise_in_scope_raw_pointer_path();

    if (k_compiled_with_memory_sanitizer_oracle) {
        run_sanitizer_oracle(argc, argv);
    } else if (running_under_valgrind()) {
        run_valgrind_oracle(argc, argv);
    } else {
        report_no_external_oracle();
    }

    RUN_TESTS("test_scope_uaf_sanitizer");
}
