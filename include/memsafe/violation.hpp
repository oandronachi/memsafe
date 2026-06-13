/**
 * @file violation.hpp
 * @brief Slice 0 violation-policy core plus Slice 6 advanced diagnostics.
 *
 * @details
 * Work package: CPP_MEMSAFE-0600-FUNC, extending the finalized
 * CPP_MEMSAFE-0010-FUNC violation backbone.
 *
 * Purpose:
 * - Provide the canonical `MEMSAFE_ON_VIOLATION` policy constants and default.
 * - Provide `memsafe::violation_kind`, `memsafe::violation_info`, and the
 *   `memsafe::violation` exception used by THROW-policy builds.
 * - Extend `memsafe::violation_info` with richer Slice 6 handler payload data:
 *   active policy, component/operation context, object identity, user context,
 *   and a structured borrow-check call chain.
 * - Provide an optional logging hook that receives the same extended payload
 *   before the configured violation policy is applied.
 * - Provide a thread-safe process-local handler slot used by HANDLER-policy
 *   builds.
 * - Provide `memsafe::detail::report_violation` and
 *   `MEMSAFE_DETAIL_VIOLATE(KIND, MSG)` so later Slice 0/1/2/4 work can route
 *   detected invariant failures through the same policy hook.
 *
 * Key invariants:
 * - `MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW`, and
 *   `MEMSAFE_VIOLATION_HANDLER` keep stable numeric values for preprocessor
 *   comparisons.
 * - The default policy is fail-safe abort, matching F1 configuration-macro
 *   requirements and F3 `Violation_Policy => Abort`.
 * - A HANDLER policy with no installed handler falls back to abort instead of
 *   returning silently.
 * - `MEMSAFE_RELEASE_CHECKS` controls whether later feature headers compile
 *   check call sites; it does not remove this violation machinery.
 * - The first five `violation_info` members remain `kind`, `message`, `file`,
 *   `line`, and `function` in that order so Slice 0 source expectations and
 *   handler signatures continue to compile unchanged.
 * - `std::source_location` is used only when available outside
 *   `MEMSAFE_CXX17_COMPAT`; otherwise the macro records `__FILE__`, `__LINE__`,
 *   and `__func__`.
 *
 * Ownership and thread-safety:
 * - Diagnostic strings and borrow-chain arrays in `violation_info` are
 *   borrowed, non-owning pointers. Reporters must keep them valid for the
 *   synchronous report call; string literals and stack arrays used immediately
 *   with a report macro satisfy that requirement.
 * - Handler installation and lookup use an atomic function pointer. The slot is
 *   thread-safe; handler function bodies own any synchronization for their
 *   captured or global state.
 * - Logging hook installation and lookup also use an atomic function pointer.
 *   Hook exceptions are swallowed so optional logging integrations cannot
 *   change the selected violation policy's fail-safe behavior.
 */
#ifndef MEMSAFE_VIOLATION_HPP
#define MEMSAFE_VIOLATION_HPP

/**
 * @def MEMSAFE_VIOLATION_ABORT
 * @brief Select process termination as the violation policy.
 *
 * @retval 1 Stable preprocessor token accepted by `MEMSAFE_ON_VIOLATION`.
 * @pre Use only in preprocessor or compile-time policy selection contexts.
 * @post `memsafe::detail::report_violation` prints a diagnostic and terminates
 * the process with `std::abort` when this policy is active.
 * @invariant The numeric value is part of the Slice 0 policy ABI and must not
 * change.
 * @throws Nothing directly; it expands to an integer literal.
 * @note Ownership/thread-safety: the macro owns no storage and touches no
 * runtime state.
 */
#define MEMSAFE_VIOLATION_ABORT 1

/**
 * @def MEMSAFE_VIOLATION_THROW
 * @brief Select exception throwing as the violation policy.
 *
 * @retval 2 Stable preprocessor token accepted by `MEMSAFE_ON_VIOLATION`.
 * @pre Use only in preprocessor or compile-time policy selection contexts.
 * @post `memsafe::detail::report_violation` throws `memsafe::violation` carrying
 * the message and source-location payload when this policy is active.
 * @invariant The numeric value is part of the Slice 0 policy ABI and must not
 * change.
 * @throws Nothing directly; `memsafe::detail::report_violation` performs the
 * throw when this policy is active.
 * @note Ownership/thread-safety: the macro owns no storage and touches no
 * runtime state.
 */
#define MEMSAFE_VIOLATION_THROW 2

/**
 * @def MEMSAFE_VIOLATION_HANDLER
 * @brief Select user-handler dispatch as the violation policy.
 *
 * @retval 3 Stable preprocessor token accepted by `MEMSAFE_ON_VIOLATION`.
 * @pre Use only in preprocessor or compile-time policy selection contexts.
 * @post `memsafe::detail::report_violation` invokes the installed
 * `memsafe::violation_handler`, or aborts if none is installed.
 * @invariant Missing handlers are fail-safe and cannot make violations return
 * silently.
 * @throws Nothing directly; a user handler may throw if its implementation
 * chooses to do so.
 * @note Ownership/thread-safety: the macro owns no storage and touches no
 * runtime state.
 */
#define MEMSAFE_VIOLATION_HANDLER 3

#ifndef MEMSAFE_ON_VIOLATION
/**
 * @def MEMSAFE_ON_VIOLATION
 * @brief Compile-time selector for the active violation policy.
 *
 * @retval MEMSAFE_VIOLATION_ABORT The default value when a translation unit does
 * not define the macro before including a memsafe header.
 * @pre If supplied by the user, define it before including this header and use
 * one of `MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW`, or
 * `MEMSAFE_VIOLATION_HANDLER`.
 * @post All inline policy dispatch in this translation unit follows the chosen
 * branch.
 * @invariant Abort remains the default so detected safety violations do not
 * continue by accident.
 * @throws Nothing directly; runtime behavior is selected by the active policy.
 * @note Ownership/thread-safety: this macro is compile-time configuration only.
 */
#  define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_ABORT
#endif

#ifndef MEMSAFE_RELEASE_CHECKS
/**
 * @def MEMSAFE_RELEASE_CHECKS
 * @brief Configure whether later feature headers emit runtime check call sites.
 *
 * @retval 1 Runtime checks are enabled by default.
 * @pre Define to 0 before including memsafe feature headers when a release lane
 * requires check call sites to compile away.
 * @post This header still exposes violation types and policy dispatch even when
 * the macro is 0.
 * @invariant The violation machinery remains present in every lane; only
 * guarded check call sites in dependent headers are removed.
 * @throws Nothing directly; it expands to an integer literal.
 * @note Ownership/thread-safety: this macro is compile-time configuration only.
 */
#  define MEMSAFE_RELEASE_CHECKS 1
#endif

#ifndef MEMSAFE_CXX17_COMPAT
/**
 * @def MEMSAFE_CXX17_COMPAT
 * @brief Request the C++17-compatible diagnostic capture path.
 *
 * @retval 0 C++20 `std::source_location` may be used when the compiler and
 * standard library provide it.
 * @pre Define to a nonzero value before including this header in the C++17 lane.
 * @post No declaration or include of `std::source_location` is reachable when
 * the macro is nonzero.
 * @invariant The fallback path still captures file, line, and function with
 * `__FILE__`, `__LINE__`, and `__func__`.
 * @throws Nothing directly; it expands to an integer literal.
 * @note Ownership/thread-safety: this macro is compile-time configuration only.
 */
#  define MEMSAFE_CXX17_COMPAT 0
#endif

#if MEMSAFE_ON_VIOLATION != MEMSAFE_VIOLATION_ABORT && \
    MEMSAFE_ON_VIOLATION != MEMSAFE_VIOLATION_THROW && \
    MEMSAFE_ON_VIOLATION != MEMSAFE_VIOLATION_HANDLER
#  error "MEMSAFE_ON_VIOLATION must be MEMSAFE_VIOLATION_ABORT, MEMSAFE_VIOLATION_THROW, or MEMSAFE_VIOLATION_HANDLER"
#endif

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

#if defined(_MSVC_LANG)
#  define MEMSAFE_DETAIL_LANG_VERSION _MSVC_LANG
#else
#  define MEMSAFE_DETAIL_LANG_VERSION __cplusplus
#endif

#if !MEMSAFE_CXX17_COMPAT && MEMSAFE_DETAIL_LANG_VERSION >= 202002L
#  if defined(__has_include)
#    if __has_include(<source_location>)
#      include <source_location>
#      define MEMSAFE_DETAIL_HAS_SOURCE_LOCATION 1
#    endif
#  endif
#endif

#ifndef MEMSAFE_DETAIL_HAS_SOURCE_LOCATION
#  define MEMSAFE_DETAIL_HAS_SOURCE_LOCATION 0
#endif

namespace memsafe {

/**
 * @enum violation_kind
 * @brief Classifies the memory-safety invariant that was violated.
 *
 * @return Values are consumed as ordinary enum values and converted to text with
 * `memsafe::to_string`.
 * @pre Reporters should select the enumerator that most closely describes the
 * failed invariant.
 * @post Handlers, exceptions, and diagnostics can branch on the violation class.
 * @invariant Enumerator names are the canonical Slice 0 diagnostic vocabulary
 * used by downstream tests and feature headers.
 * @throws Nothing; this is a value type with no operations that throw.
 * @note Ownership/thread-safety: enum values own no resources and are safe to
 * copy between threads.
 */
enum class violation_kind {
    /// A shared/mutable borrow exclusivity rule was broken.
    borrow_exclusivity,
    /// A checked handle or reference observed storage after release.
    use_after_free,
    /// A generational handle no longer matches the live slot generation.
    generation_mismatch,
    /// A resource release path attempted to destroy the same object twice.
    double_free,
    /// A bounded owner, map, or arena could not satisfy an allocation request.
    capacity_exhausted,
    /// A checked access path observed a null owner or control pointer.
    null_access,
    /// An owner was destroyed while borrow counters still indicated live borrows.
    owner_destroyed_while_borrowed
};

/**
 * @brief Return the canonical string name for a violation kind.
 *
 * @param kind Violation class to convert.
 * @return A static null-terminated string naming `kind`, or `"unknown"` for an
 * out-of-range value.
 * @pre `kind` may be any value representable by `memsafe::violation_kind`.
 * @post No state is modified.
 * @invariant The returned pointer is never null and has static storage duration.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: returned strings are immutable shared storage.
 */
inline const char* to_string(violation_kind kind) noexcept {
    switch (kind) {
        case violation_kind::borrow_exclusivity:
            return "borrow_exclusivity";
        case violation_kind::use_after_free:
            return "use_after_free";
        case violation_kind::generation_mismatch:
            return "generation_mismatch";
        case violation_kind::double_free:
            return "double_free";
        case violation_kind::capacity_exhausted:
            return "capacity_exhausted";
        case violation_kind::null_access:
            return "null_access";
        case violation_kind::owner_destroyed_while_borrowed:
            return "owner_destroyed_while_borrowed";
    }
    return "unknown";
}

/**
 * @enum violation_policy
 * @brief Names the active runtime policy branch used for one violation report.
 *
 * @return Values are ordinary scoped enum values and can be converted to text
 * with `memsafe::to_string`.
 * @pre Use values only as diagnostic metadata; preprocessor selection still
 * uses the `MEMSAFE_VIOLATION_*` integer macros.
 * @post Handlers, log hooks, and thrown exceptions can inspect which policy
 * produced a payload without re-reading preprocessor state.
 * @invariant Enumerator numeric values mirror the public
 * `MEMSAFE_VIOLATION_ABORT`, `MEMSAFE_VIOLATION_THROW`, and
 * `MEMSAFE_VIOLATION_HANDLER` constants.
 * @throws Nothing; this is a resource-free value type.
 * @note Ownership/thread-safety: enum values own no resources and are safe to
 * copy between threads.
 */
enum class violation_policy {
    /// Process termination policy selected by `MEMSAFE_VIOLATION_ABORT`.
    abort = MEMSAFE_VIOLATION_ABORT,
    /// Exception policy selected by `MEMSAFE_VIOLATION_THROW`.
    throw_exception = MEMSAFE_VIOLATION_THROW,
    /// User callback policy selected by `MEMSAFE_VIOLATION_HANDLER`.
    handler = MEMSAFE_VIOLATION_HANDLER
};

/**
 * @brief Return the canonical string name for a violation policy.
 *
 * @param policy Policy value to convert.
 * @return A static null-terminated string naming `policy`, or `"unknown"` for
 * an out-of-range value.
 * @pre `policy` may be any value representable by
 * `memsafe::violation_policy`.
 * @post No state is modified.
 * @invariant The returned pointer is never null and has static storage
 * duration.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: returned strings are immutable shared storage.
 */
inline const char* to_string(violation_policy policy) noexcept {
    switch (policy) {
        case violation_policy::abort:
            return "abort";
        case violation_policy::throw_exception:
            return "throw";
        case violation_policy::handler:
            return "handler";
    }
    return "unknown";
}

/**
 * @brief Return the policy branch compiled into this translation unit.
 *
 * @return `memsafe::violation_policy` corresponding to
 * `MEMSAFE_ON_VIOLATION`.
 * @pre `MEMSAFE_ON_VIOLATION` has passed this header's policy validation.
 * @post No state is modified.
 * @invariant The result is a compile-time constant for the current translation
 * unit and is copied into every `violation_info` built by this header.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the result owns no resources and reads no
 * mutable state.
 */
inline constexpr violation_policy active_violation_policy() noexcept {
#if MEMSAFE_ON_VIOLATION == MEMSAFE_VIOLATION_THROW
    return violation_policy::throw_exception;
#elif MEMSAFE_ON_VIOLATION == MEMSAFE_VIOLATION_HANDLER
    return violation_policy::handler;
#else
    return violation_policy::abort;
#endif
}

/**
 * @struct borrow_chain_frame
 * @brief One structured frame in a borrow-check diagnostic chain.
 *
 * @pre `operation`, `object_type`, `file`, and `function` may be null only when
 * a reporter cannot provide that field. `object_address` is diagnostic-only
 * and must never be dereferenced by handlers or log hooks.
 * @post Instances can be stored in arrays and referenced by
 * `memsafe::violation_info::borrow_chain`.
 * @invariant The frame is a plain non-owning aggregate and never allocates.
 * @throws Nothing; the struct owns no resources and uses implicit special
 * members.
 * @note Ownership/thread-safety: pointer fields borrow storage from the report
 * site. Concurrent reads are safe when the pointed-to storage is immutable.
 *
 * @code
 * const memsafe::borrow_chain_frame chain[] = {
 *     MEMSAFE_DETAIL_BORROW_CHAIN_FRAME("Owner<T>::borrow_mut", "Owner<T>", this)
 * };
 * @endcode
 */
struct borrow_chain_frame {
    /// Borrow operation or API step represented by this frame.
    const char* operation;
    /// Logical object or wrapper type participating in the borrow check.
    const char* object_type;
    /// Optional address used only to correlate diagnostics with an object.
    const void* object_address;
    /// Source file where this chain frame was captured, or null.
    const char* file;
    /// One-based source line where this chain frame was captured, or 0.
    int line;
    /// Enclosing function where this chain frame was captured, or null.
    const char* function;
};

/**
 * @struct violation_context
 * @brief Optional Slice 6 metadata attached to a violation report.
 *
 * @pre Pointer fields may be null when the caller has no additional context.
 * `object_address` and `user_context` are diagnostic-only and are not owned or
 * dereferenced by the library.
 * @post `memsafe::detail::make_violation_info` copies the context into the
 * extended `violation_info` payload.
 * @invariant The context is a non-owning aggregate; default construction
 * represents the generic memsafe component with no operation-specific fields.
 * @throws Nothing; the struct owns no resources and uses implicit special
 * members.
 * @note Ownership/thread-safety: pointer fields borrow storage from the caller.
 * Use immutable strings or otherwise synchronize externally when shared across
 * threads.
 *
 * @code
 * const memsafe::violation_context context{
 *     "owner", "borrow_mut", "Owner<int>", &owner, "shared borrow is live", nullptr
 * };
 * @endcode
 */
struct violation_context {
    /// Logical component reporting the violation, for example "owner".
    const char* component = "memsafe";
    /// Operation being attempted when the violation was detected.
    const char* operation = nullptr;
    /// Logical type or wrapper associated with `object_address`.
    const char* object_type = nullptr;
    /// Optional object address used only for diagnostic correlation.
    const void* object_address = nullptr;
    /// Additional non-owning diagnostic detail beyond `message`.
    const char* diagnostic_note = nullptr;
    /// Opaque caller-owned value forwarded unchanged to handlers and log hooks.
    const void* user_context = nullptr;
};

/**
 * @struct violation_info
 * @brief Synchronous diagnostic payload for every violation policy.
 *
 * @pre `kind` should identify the failed invariant. `message`, `file`, and
 * `function` may be null only when the caller cannot provide that field.
 * `borrow_chain` is either null with `borrow_chain_size == 0`, or points to an
 * array containing at least `borrow_chain_size` frames valid for synchronous
 * policy dispatch.
 * @post The payload can be copied into `memsafe::violation` or passed by const
 * reference to a `memsafe::violation_handler`.
 * @invariant The struct is a plain non-owning aggregate and never allocates.
 * The Slice 0 members stay first and in the original order:
 * `kind`, `message`, `file`, `line`, `function`.
 * @throws Nothing; the struct owns no resources and uses implicit special
 * members.
 * @note Ownership/thread-safety: pointer fields borrow storage from the
 * reporting site. Concurrent reads are safe when the pointed-to storage is
 * immutable. `object_address` and `user_context` are opaque diagnostics and are
 * never dereferenced by this header.
 */
struct violation_info {
    /// Violated invariant classification.
    violation_kind kind;
    /// Human-readable diagnostic message, or null for the default message.
    const char* message;
    /// Source file where the violation was reported, or null when unavailable.
    const char* file;
    /// One-based source line where the violation was reported, or 0.
    int line;
    /// Enclosing function where the violation was reported, or null.
    const char* function;
    /// Policy branch compiled into the reporting translation unit.
    violation_policy policy = active_violation_policy();
    /// Logical component reporting the violation, defaulting to "memsafe".
    const char* component = "memsafe";
    /// Operation being attempted when the violation was detected, or null.
    const char* operation = nullptr;
    /// Logical type or wrapper associated with `object_address`, or null.
    const char* object_type = nullptr;
    /// Optional address used only to correlate diagnostics with an object.
    const void* object_address = nullptr;
    /// Additional diagnostic detail beyond `message`, or null.
    const char* diagnostic_note = nullptr;
    /// Structured borrow-check chain, or null when no chain is available.
    const borrow_chain_frame* borrow_chain = nullptr;
    /// Number of frames in `borrow_chain`.
    std::size_t borrow_chain_size = 0U;
    /// Opaque caller-owned value forwarded unchanged to handlers and log hooks.
    const void* user_context = nullptr;
};

/**
 * @class violation
 * @brief Exception type thrown by the `MEMSAFE_VIOLATION_THROW` policy.
 *
 * @pre Construct with a `violation_info` payload whose borrowed pointers remain
 * valid while the exception is inspected.
 * @post The exception stores a by-value copy of the payload and exposes stable
 * accessors for tests and callers.
 * @invariant `what()` and `message()` return the same fallback-normalized
 * message pointer.
 * @throws Construction and accessors do not throw; the object is thrown by
 * `memsafe::detail::report_violation` when THROW policy is selected.
 * @note Ownership/thread-safety: the exception owns the aggregate payload only;
 * string storage remains borrowed. Concurrent reads of a caught exception are
 * safe when the borrowed strings are immutable.
 *
 * @code
 * #define MEMSAFE_ON_VIOLATION MEMSAFE_VIOLATION_THROW
 * #include <memsafe/violation.hpp>
 *
 * try {
 *     MEMSAFE_DETAIL_VIOLATE(memsafe::violation_kind::null_access, "empty owner");
 * } catch (const memsafe::violation& ex) {
 *     std::fprintf(stderr, "%s\n", ex.what());
 * }
 * @endcode
 */
class violation : public std::exception {
public:
    /**
     * @brief Construct an exception from a violation payload.
     *
     * @param info Payload copied into the exception.
     * @return Constructed `memsafe::violation` object.
     * @pre Borrowed pointer fields inside `info` remain valid while accessors or
     * `what()` are used.
     * @post `this->info()` returns the copied payload.
     * @invariant Construction does not allocate and therefore preserves the
     * header-only, low-dependency Slice 0 path.
     * @throws Nothing; this constructor is `noexcept`.
     * @note Ownership/thread-safety: only the aggregate is copied; strings remain
     * borrowed from the report site.
     */
    explicit violation(violation_info info) noexcept : info_(info) {}

    /**
     * @brief Return the diagnostic message for standard exception consumers.
     *
     * @return The same non-null pointer returned by `message()`.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The return value is never null.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned string is borrowed storage.
     */
    const char* what() const noexcept override {
        return message();
    }

    /**
     * @brief Return the complete copied diagnostic payload.
     *
     * @return Const reference to the stored `memsafe::violation_info`.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The returned reference remains valid for the lifetime of the
     * exception object.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the aggregate is owned by the exception;
     * pointer members remain borrowed.
     */
    const violation_info& info() const noexcept {
        return info_;
    }

    /**
     * @brief Return the violated invariant kind.
     *
     * @return The `violation_kind` stored in the payload.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The value equals `info().kind`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: returns a resource-free enum value.
     */
    violation_kind kind() const noexcept {
        return info_.kind;
    }

    /**
     * @brief Return the diagnostic message with a safe fallback.
     *
     * @return Payload message when non-null; otherwise `"memsafe violation"`.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The return value is never null.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned pointer is borrowed or static
     * immutable storage.
     */
    const char* message() const noexcept {
        return info_.message ? info_.message : "memsafe violation";
    }

    /**
     * @brief Return the source file associated with the violation.
     *
     * @return Payload file when non-null; otherwise an empty string.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The return value is never null.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned pointer is borrowed or static
     * immutable storage.
     */
    const char* file() const noexcept {
        return info_.file ? info_.file : "";
    }

    /**
     * @brief Return the source line associated with the violation.
     *
     * @return One-based source line, or 0 when unavailable.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The value equals `info().line`.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: returns a resource-free integer value.
     */
    int line() const noexcept {
        return info_.line;
    }

    /**
     * @brief Return the function associated with the violation.
     *
     * @return Payload function when non-null; otherwise an empty string.
     * @pre The exception object is alive.
     * @post No state is modified.
     * @invariant The return value is never null.
     * @throws Nothing; this function is `noexcept`.
     * @note Ownership/thread-safety: the returned pointer is borrowed or static
     * immutable storage.
     */
    const char* function() const noexcept {
        return info_.function ? info_.function : "";
    }

private:
    violation_info info_;
};

/**
 * @typedef violation_handler
 * @brief Function-pointer type installed for `MEMSAFE_VIOLATION_HANDLER`.
 *
 * @return Type alias naming functions that receive `const violation_info&` and
 * return `void`.
 * @pre The pointed-to function, when installed, must remain callable until it is
 * replaced or cleared with `set_violation_handler(nullptr)`.
 * @post `memsafe::detail::report_violation` calls the installed handler once
 * for each violation in HANDLER policy.
 * @invariant The handler signature is plain C++ function pointer ABI: no
 * captured lambdas or ownership transfer are required.
 * @throws The alias itself throws nothing; invoked handlers may throw according
 * to their own implementation.
 * @note Ownership/thread-safety: the atomic slot stores only the pointer.
 * Handler code owns synchronization for any state it touches.
 *
 * @code
 * void handler(const memsafe::violation_info& info) {
 *     std::fprintf(stderr, "%s\n", info.message);
 * }
 * memsafe::set_violation_handler(&handler);
 * @endcode
 */
using violation_handler = void (*)(const violation_info&);

/**
 * @typedef violation_log_hook
 * @brief Function-pointer type for optional logging-framework integration.
 *
 * @return Type alias naming functions that receive `const violation_info&` and
 * return `void`.
 * @pre The pointed-to function, when installed, must remain callable until it is
 * replaced or cleared with `set_violation_log_hook(nullptr)`.
 * @post `memsafe::detail::report_violation` offers every payload to the hook
 * before applying ABORT, THROW, or HANDLER policy dispatch.
 * @invariant Hook failures must not alter violation behavior; exceptions thrown
 * by a hook are caught and discarded by the library.
 * @throws The alias itself throws nothing; invoked hooks may throw, but the
 * report path swallows those exceptions.
 * @note Ownership/thread-safety: the atomic slot stores only the pointer.
 * Hook code owns synchronization for any logging backend or shared state it
 * touches.
 *
 * @code
 * void log_to_framework(const memsafe::violation_info& info) {
 *     framework_log(info.message, info.file, info.line);
 * }
 * memsafe::set_violation_log_hook(&log_to_framework);
 * @endcode
 */
using violation_log_hook = void (*)(const violation_info&);

namespace detail {

/**
 * @brief Return the process-local atomic handler slot.
 *
 * @return Reference to the function-pointer slot used by handler accessors.
 * @pre No precondition; the function-local static is initialized on first use.
 * @post The same slot is returned for every call in the translation unit.
 * @invariant The slot starts as null so HANDLER policy remains fail-safe until a
 * handler is explicitly installed.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: C++ guarantees thread-safe initialization of
 * function-local statics, and subsequent access uses atomic operations.
 */
inline std::atomic<violation_handler>& handler_slot() noexcept {
    static std::atomic<violation_handler> handler{nullptr};
    return handler;
}

/**
 * @brief Return the process-local atomic logging hook slot.
 *
 * @return Reference to the function-pointer slot used by logging accessors.
 * @pre No precondition; the function-local static is initialized on first use.
 * @post The same slot is returned for every call in the translation unit.
 * @invariant The slot starts as null so logging integration is strictly
 * optional and has no effect unless explicitly installed.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: C++ guarantees thread-safe initialization of
 * function-local statics, and subsequent access uses atomic operations.
 */
inline std::atomic<violation_log_hook>& log_hook_slot() noexcept {
    static std::atomic<violation_log_hook> hook{nullptr};
    return hook;
}

} // namespace detail

/**
 * @brief Install, replace, or clear the handler used by HANDLER policy.
 *
 * @param handler Function pointer to install, or null to clear the handler.
 * @return Nothing.
 * @pre A non-null pointer must remain callable until replaced or cleared.
 * @post `get_violation_handler()` returns `handler` after the release-store is
 * observed.
 * @invariant Storing null restores fail-safe abort behavior for HANDLER policy.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: installation is atomic. The library does not
 * own the pointed-to function or synchronize inside it.
 */
inline void set_violation_handler(violation_handler handler) noexcept {
    detail::handler_slot().store(handler, std::memory_order_release);
}

/**
 * @brief Return the currently installed violation handler.
 *
 * @return Installed `memsafe::violation_handler`, or null when none is
 * installed.
 * @pre No precondition.
 * @post No state is modified.
 * @invariant A null result means HANDLER-policy reports must abort.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: lookup is atomic and does not transfer
 * ownership of the pointed-to function.
 */
inline violation_handler get_violation_handler() noexcept {
    return detail::handler_slot().load(std::memory_order_acquire);
}

/**
 * @brief Install, replace, or clear the optional violation logging hook.
 *
 * @param hook Function pointer to install, or null to clear the hook.
 * @return Nothing.
 * @pre A non-null pointer must remain callable until replaced or cleared.
 * @post `get_violation_log_hook()` returns `hook` after the release-store is
 * observed.
 * @invariant Storing null restores the default no-logging path and must not
 * affect the configured violation policy.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: installation is atomic. The library does not
 * own the pointed-to function or synchronize inside it.
 */
inline void set_violation_log_hook(violation_log_hook hook) noexcept {
    detail::log_hook_slot().store(hook, std::memory_order_release);
}

/**
 * @brief Return the currently installed optional logging hook.
 *
 * @return Installed `memsafe::violation_log_hook`, or null when none is
 * installed.
 * @pre No precondition.
 * @post No state is modified.
 * @invariant A null result means policy dispatch proceeds without calling any
 * logging integration.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: lookup is atomic and does not transfer
 * ownership of the pointed-to function.
 */
inline violation_log_hook get_violation_log_hook() noexcept {
    return detail::log_hook_slot().load(std::memory_order_acquire);
}

/**
 * @brief Install, replace, or clear the optional violation logger alias.
 *
 * @param hook Function pointer to install, or null to clear the logger.
 * @return Nothing.
 * @pre A non-null pointer must remain callable until replaced or cleared.
 * @post `get_violation_logger()` returns `hook` after the release-store is
 * observed.
 * @invariant This alias forwards to `set_violation_log_hook` so logging
 * frameworks can use logger terminology without a second storage slot.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: installation is atomic through the shared log
 * hook slot.
 */
inline void set_violation_logger(violation_log_hook hook) noexcept {
    set_violation_log_hook(hook);
}

/**
 * @brief Return the currently installed optional violation logger alias.
 *
 * @return Installed `memsafe::violation_log_hook`, or null when none is
 * installed.
 * @pre No precondition.
 * @post No state is modified.
 * @invariant This alias reads the same slot as `get_violation_log_hook`.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: lookup is atomic and does not transfer
 * ownership of the pointed-to function.
 */
inline violation_log_hook get_violation_logger() noexcept {
    return get_violation_log_hook();
}

namespace detail {

/**
 * @brief Build an extended violation payload from explicit source fields.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param file Source file, or null when unavailable.
 * @param line Source line, or 0 when unavailable.
 * @param function Source function, or null when unavailable.
 * @param context Optional Slice 6 context copied into the payload.
 * @param borrow_chain Optional array of structured borrow-chain frames.
 * @param borrow_chain_size Number of frames available through `borrow_chain`.
 * @return Aggregate payload carrying source fields, context, active policy, and
 * borrow-chain diagnostics.
 * @pre Any non-null pointer must remain valid for the synchronous report call or
 * subsequent exception inspection. If `borrow_chain_size > 0`, `borrow_chain`
 * points to at least that many valid frames.
 * @post No global state is modified.
 * @invariant The returned aggregate preserves the Slice 0 field order:
 * kind, message, file, line, function. A null chain pointer always produces
 * `borrow_chain_size == 0` in the payload.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the returned payload borrows all pointer
 * fields and does not take ownership of `context.user_context`.
 *
 * @code
 * const memsafe::borrow_chain_frame chain[] = {
 *     MEMSAFE_DETAIL_BORROW_CHAIN_FRAME("borrow_mut", "Owner<int>", &owner)
 * };
 * const auto info = memsafe::detail::make_violation_info(
 *     memsafe::violation_kind::borrow_exclusivity,
 *     "exclusive borrow blocked",
 *     __FILE__,
 *     __LINE__,
 *     __func__,
 *     memsafe::violation_context{"owner", "borrow_mut", "Owner<int>", &owner,
 *                                nullptr, nullptr},
 *     chain,
 *     1U);
 * @endcode
 */
inline violation_info make_violation_info(violation_kind kind,
                                          const char* message,
                                          const char* file,
                                          int line,
                                          const char* function,
                                          violation_context context,
                                          const borrow_chain_frame* borrow_chain,
                                          std::size_t borrow_chain_size) noexcept {
    violation_info info{kind, message, file, line, function};
    info.policy = active_violation_policy();
    info.component = context.component ? context.component : "memsafe";
    info.operation = context.operation;
    info.object_type = context.object_type;
    info.object_address = context.object_address;
    info.diagnostic_note = context.diagnostic_note;
    /*
     * Slice 6 requires structured borrow-chain diagnostics, but a partial
     * pointer/count pair is worse than no chain. Normalize the absent case so
     * handlers can branch on `borrow_chain_size != 0` without a second null
     * check when the payload came through the public builders.
     */
    if (borrow_chain != nullptr && borrow_chain_size != 0U) {
        info.borrow_chain = borrow_chain;
        info.borrow_chain_size = borrow_chain_size;
    } else {
        info.borrow_chain = nullptr;
        info.borrow_chain_size = 0U;
    }
    info.user_context = context.user_context;
    return info;
}

/**
 * @brief Build a Slice 0-compatible violation payload from explicit fields.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param file Source file, or null when unavailable.
 * @param line Source line, or 0 when unavailable.
 * @param function Source function, or null when unavailable.
 * @return Aggregate payload carrying the supplied fields plus default Slice 6
 * metadata.
 * @pre Any non-null pointer must remain valid for the synchronous report call or
 * subsequent exception inspection.
 * @post No state is modified.
 * @invariant This overload preserves the finalized Slice 0 call shape while
 * setting `policy` and default context fields for advanced handlers.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: the returned payload borrows all pointer
 * fields.
 */
inline violation_info make_violation_info(violation_kind kind,
                                          const char* message,
                                          const char* file,
                                          int line,
                                          const char* function) noexcept {
    return make_violation_info(kind,
                               message,
                               file,
                               line,
                               function,
                               violation_context{},
                               nullptr,
                               0U);
}

#if MEMSAFE_DETAIL_HAS_SOURCE_LOCATION
/**
 * @brief Build an extended violation payload from `std::source_location`.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param location Source location captured at the report site.
 * @param context Optional Slice 6 context copied into the payload.
 * @param borrow_chain Optional array of structured borrow-chain frames.
 * @param borrow_chain_size Number of frames available through `borrow_chain`.
 * @return Aggregate payload carrying `kind`, `message`, source fields from
 * `location`, and the extended Slice 6 metadata.
 * @pre `location` is a valid `std::source_location` object. If
 * `borrow_chain_size > 0`, `borrow_chain` points to at least that many valid
 * frames.
 * @post No state is modified.
 * @invariant This overload is not declared when `MEMSAFE_CXX17_COMPAT` is
 * nonzero, preserving the C++17 lane's no-`source_location` rule.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: source-location strings are borrowed from
 * implementation-managed storage; context and chain pointers are caller-owned.
 */
inline violation_info make_violation_info(violation_kind kind,
                                          const char* message,
                                          const std::source_location& location,
                                          violation_context context,
                                          const borrow_chain_frame* borrow_chain,
                                          std::size_t borrow_chain_size) noexcept {
    return make_violation_info(kind,
                               message,
                               location.file_name(),
                               static_cast<int>(location.line()),
                               location.function_name(),
                               context,
                               borrow_chain,
                               borrow_chain_size);
}

/**
 * @brief Build a Slice 0-compatible violation payload from `std::source_location`.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param location Source location captured at the report site.
 * @return Aggregate payload carrying `kind`, `message`, and source fields from
 * `location` plus default Slice 6 metadata.
 * @pre `location` is a valid `std::source_location` object.
 * @post No state is modified.
 * @invariant This overload is not declared when `MEMSAFE_CXX17_COMPAT` is
 * nonzero.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: source-location strings are borrowed from
 * implementation-managed storage.
 */
inline violation_info make_violation_info(
    violation_kind kind,
    const char* message,
    const std::source_location& location = std::source_location::current()) noexcept {
    return make_violation_info(kind,
                               message,
                               location,
                               violation_context{},
                               nullptr,
                               0U);
}
#endif

/**
 * @brief Emit the canonical abort diagnostic and terminate the process.
 *
 * @param info Violation payload to print.
 * @return This function never returns.
 * @pre `info.kind` identifies the violated invariant. Pointer fields may be
 * null and are rendered with safe fallback text. If a borrow chain is present,
 * `info.borrow_chain` points to at least `info.borrow_chain_size` frames.
 * @post `stderr` is flushed and the process terminates through `std::abort`.
 * @invariant The first diagnostic line starts with `memsafe: violation` so
 * death tests can recognize the failure path without relying on CTest
 * `WILL_FAIL` behavior. Borrow-chain frames are printed only after that stable
 * first line.
 * @throws Nothing; this function is `noexcept` and terminates.
 * @note Ownership/thread-safety: stdio serialization is delegated to the C
 * runtime.
 */
[[noreturn]] inline void abort_with(const violation_info& info) noexcept {
    std::fprintf(stderr,
                 "memsafe: violation [%s] %s policy=%s component=%s "
                 "operation=%s object_type=%s object=%p (%s:%d in %s)\n",
                 to_string(info.kind),
                 info.message ? info.message : "memsafe violation",
                 to_string(info.policy),
                 info.component ? info.component : "memsafe",
                 info.operation ? info.operation : "?",
                 info.object_type ? info.object_type : "?",
                 const_cast<void*>(info.object_address),
                 info.file ? info.file : "?",
                 info.line,
                 info.function ? info.function : "?");
    if (info.diagnostic_note != nullptr && info.diagnostic_note[0] != '\0') {
        std::fprintf(stderr, "memsafe: diagnostic-note %s\n", info.diagnostic_note);
    }
    if (info.borrow_chain != nullptr && info.borrow_chain_size != 0U) {
        for (std::size_t index = 0U; index < info.borrow_chain_size; ++index) {
            const borrow_chain_frame& frame = info.borrow_chain[index];
            std::fprintf(stderr,
                         "memsafe: borrow-chain[%zu] operation=%s "
                         "object_type=%s object=%p (%s:%d in %s)\n",
                         index,
                         frame.operation ? frame.operation : "?",
                         frame.object_type ? frame.object_type : "?",
                         const_cast<void*>(frame.object_address),
                         frame.file ? frame.file : "?",
                         frame.line,
                         frame.function ? frame.function : "?");
        }
    }
    std::fflush(stderr);
    std::abort();
}

/**
 * @brief Offer a payload to the optional logging hook.
 *
 * @param info Payload to forward to the installed hook.
 * @return Nothing.
 * @pre `info` remains valid for the duration of the synchronous hook call.
 * @post The installed hook has been called once, or no work is performed when
 * no hook is installed.
 * @invariant Hook exceptions are caught and discarded so logging integration
 * cannot change ABORT, THROW, or HANDLER policy outcomes.
 * @throws Nothing; this function is `noexcept`.
 * @note Ownership/thread-safety: hook lookup is atomic. Hook bodies own any
 * synchronization for their logging framework.
 */
inline void notify_log_hook(const violation_info& info) noexcept {
    if (violation_log_hook hook = ::memsafe::get_violation_log_hook()) {
        try {
            hook(info);
        } catch (...) {
            /*
             * Slice 6 logging is optional. Dropping hook failures preserves the
             * configured violation policy and prevents diagnostics from hiding
             * the original memory-safety violation.
             */
        }
    }
}

/**
 * @brief Dispatch a prepared payload according to `MEMSAFE_ON_VIOLATION`.
 *
 * @param info Complete Slice 0/Slice 6 violation payload.
 * @return Nothing when the active HANDLER policy has an installed handler that
 * returns; otherwise this function does not return.
 * @pre `info` remains valid for the duration of synchronous policy dispatch.
 * @post The optional logging hook is invoked first when installed. ABORT
 * terminates; THROW throws `memsafe::violation`; HANDLER invokes the installed
 * handler or aborts when no handler is installed.
 * @invariant The no-handler HANDLER branch is deliberately identical to ABORT
 * for fail-safe F2 case 4 behavior.
 * @throws `memsafe::violation` when `MEMSAFE_ON_VIOLATION` is
 * `MEMSAFE_VIOLATION_THROW`; an installed handler may throw in HANDLER policy.
 * @note Ownership/thread-safety: handler lookup is atomic; callback bodies own
 * any synchronization they require.
 */
inline void report_violation(const violation_info& info)
#if MEMSAFE_ON_VIOLATION == MEMSAFE_VIOLATION_THROW
{
    notify_log_hook(info);
    throw ::memsafe::violation(info);
}
#elif MEMSAFE_ON_VIOLATION == MEMSAFE_VIOLATION_HANDLER
{
    notify_log_hook(info);
    if (violation_handler handler = ::memsafe::get_violation_handler()) {
        handler(info);
        return;
    }
    // F2 requires HANDLER policy to fail safe when no handler is installed.
    abort_with(info);
}
#else
{
    notify_log_hook(info);
    abort_with(info);
}
#endif

#if MEMSAFE_DETAIL_HAS_SOURCE_LOCATION
/**
 * @brief Report an extended violation captured with `std::source_location`.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param location Source location captured at the call site.
 * @param context Optional Slice 6 context copied into the payload.
 * @param borrow_chain Optional array of structured borrow-chain frames.
 * @param borrow_chain_size Number of frames available through `borrow_chain`.
 * @return Nothing when the active HANDLER policy has an installed handler that
 * returns; otherwise this function does not return.
 * @pre `kind` identifies the failed invariant; `message`, context pointers,
 * and `borrow_chain` remain valid for synchronous dispatch or thrown exception
 * inspection.
 * @post Builds an extended payload and delegates to
 * `report_violation(const violation_info&)`.
 * @invariant This overload is not declared when `MEMSAFE_CXX17_COMPAT` is
 * nonzero.
 * @throws `memsafe::violation` when `MEMSAFE_ON_VIOLATION` is
 * `MEMSAFE_VIOLATION_THROW`; an installed handler may throw in HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings, context fields, and
 * chain frames are borrowed.
 */
inline void report_violation(violation_kind kind,
                             const char* message,
                             const std::source_location& location,
                             violation_context context,
                             const borrow_chain_frame* borrow_chain,
                             std::size_t borrow_chain_size) {
    report_violation(
        make_violation_info(kind, message, location, context, borrow_chain, borrow_chain_size));
}

/**
 * @brief Report a violation captured with `std::source_location`.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param location Source location captured at the call site.
 * @return Nothing when the active HANDLER policy has an installed handler that
 * returns; otherwise this function does not return.
 * @pre `kind` identifies the failed invariant; `message` remains valid for the
 * synchronous dispatch or thrown exception inspection.
 * @post Delegates to `report_violation(const violation_info&)`.
 * @invariant This overload is not declared when `MEMSAFE_CXX17_COMPAT` is
 * nonzero.
 * @throws `memsafe::violation` when `MEMSAFE_ON_VIOLATION` is
 * `MEMSAFE_VIOLATION_THROW`; an installed handler may throw in HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings are borrowed.
 */
inline void report_violation(
    violation_kind kind,
    const char* message,
    const std::source_location& location = std::source_location::current()) {
    report_violation(make_violation_info(kind, message, location));
}
#endif

/**
 * @brief Report an extended violation captured with explicit source fields.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param file Source file, or null when unavailable.
 * @param line Source line, or 0 when unavailable.
 * @param function Source function, or null when unavailable.
 * @param context Optional Slice 6 context copied into the payload.
 * @param borrow_chain Optional array of structured borrow-chain frames.
 * @param borrow_chain_size Number of frames available through `borrow_chain`.
 * @return Nothing when the active HANDLER policy has an installed handler that
 * returns; otherwise this function does not return.
 * @pre Any non-null pointer remains valid for the synchronous dispatch or thrown
 * exception inspection. If `borrow_chain_size > 0`, `borrow_chain` points to at
 * least that many valid frames.
 * @post Builds an extended payload and delegates to
 * `report_violation(const violation_info&)`.
 * @invariant This overload is always available, including the C++17 lane.
 * @throws `memsafe::violation` when `MEMSAFE_ON_VIOLATION` is
 * `MEMSAFE_VIOLATION_THROW`; an installed handler may throw in HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings, context fields, and
 * chain frames are borrowed.
 */
inline void report_violation(violation_kind kind,
                             const char* message,
                             const char* file,
                             int line,
                             const char* function,
                             violation_context context,
                             const borrow_chain_frame* borrow_chain,
                             std::size_t borrow_chain_size) {
    report_violation(make_violation_info(kind,
                                         message,
                                         file,
                                         line,
                                         function,
                                         context,
                                         borrow_chain,
                                         borrow_chain_size));
}

/**
 * @brief Report a violation captured with explicit source fields.
 *
 * @param kind Violated invariant class.
 * @param message Diagnostic message, or null for the default message.
 * @param file Source file, or null when unavailable.
 * @param line Source line, or 0 when unavailable.
 * @param function Source function, or null when unavailable.
 * @return Nothing when the active HANDLER policy has an installed handler that
 * returns; otherwise this function does not return.
 * @pre Any non-null pointer remains valid for the synchronous dispatch or thrown
 * exception inspection.
 * @post Delegates to `report_violation(const violation_info&)`.
 * @invariant This overload is always available, including the C++17 lane.
 * @throws `memsafe::violation` when `MEMSAFE_ON_VIOLATION` is
 * `MEMSAFE_VIOLATION_THROW`; an installed handler may throw in HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings are borrowed.
 */
inline void report_violation(violation_kind kind,
                             const char* message,
                             const char* file,
                             int line,
                             const char* function) {
    report_violation(make_violation_info(kind, message, file, line, function));
}

} // namespace detail

} // namespace memsafe

/**
 * @def MEMSAFE_DETAIL_BORROW_CHAIN_FRAME
 * @brief Capture one structured borrow-chain frame at the current source site.
 *
 * @param OPERATION Expression yielding a borrowed C string naming the borrow
 * operation or API step.
 * @param OBJECT_TYPE Expression yielding a borrowed C string naming the logical
 * object or wrapper type.
 * @param OBJECT_ADDRESS Expression yielding an object pointer or null; the
 * pointer is diagnostic-only and will not be dereferenced by memsafe.
 * @return A `memsafe::borrow_chain_frame` aggregate expression.
 * @pre String arguments remain valid for the synchronous violation report that
 * consumes the frame. `OBJECT_ADDRESS` is convertible to `const void*`.
 * @post The returned frame contains the supplied diagnostic fields plus
 * `__FILE__`, `__LINE__`, and `__func__` from the macro expansion site.
 * @invariant The macro does not depend on `std::source_location`, so it remains
 * available in the C++17 compatibility lane.
 * @throws Nothing directly; it expands to aggregate initialization.
 * @note Ownership/thread-safety: the frame borrows every pointer field and owns
 * no storage.
 *
 * @code
 * const memsafe::borrow_chain_frame chain[] = {
 *     MEMSAFE_DETAIL_BORROW_CHAIN_FRAME("borrow", "Owner<int>", &owner)
 * };
 * @endcode
 */
#define MEMSAFE_DETAIL_BORROW_CHAIN_FRAME(OPERATION, OBJECT_TYPE, OBJECT_ADDRESS) \
    ::memsafe::borrow_chain_frame{(OPERATION),                                  \
                                  (OBJECT_TYPE),                                \
                                  static_cast<const void*>(OBJECT_ADDRESS),      \
                                  __FILE__,                                     \
                                  __LINE__,                                     \
                                  __func__}

#if MEMSAFE_DETAIL_HAS_SOURCE_LOCATION
/**
 * @def MEMSAFE_DETAIL_VIOLATE
 * @brief Report a Slice 0 violation at the current source location.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre `KIND` and `MSG` are valid in the calling expression context, and any
 * non-null message pointer remains valid for policy dispatch or exception
 * inspection.
 * @post Calls `memsafe::detail::report_violation` with
 * `std::source_location::current()`.
 * @invariant This C++20 path is disabled when `MEMSAFE_CXX17_COMPAT` is nonzero.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings are borrowed.
 *
 * @code
 * MEMSAFE_DETAIL_VIOLATE(memsafe::violation_kind::borrow_exclusivity,
 *                        "mutable borrow while shared borrows are live");
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE(KIND, MSG) \
      ::memsafe::detail::report_violation((KIND), (MSG), ::std::source_location::current())

/**
 * @def MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT
 * @brief Report a Slice 6 violation with context and a borrow-chain payload.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @param CONTEXT Expression yielding `memsafe::violation_context`.
 * @param CHAIN Pointer to a `memsafe::borrow_chain_frame` array, or null.
 * @param CHAIN_SIZE Number of frames available through `CHAIN`.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre Arguments are valid in the calling expression context. Any non-null
 * message, context string, or chain pointer remains valid for policy dispatch
 * or exception inspection.
 * @post Calls `memsafe::detail::report_violation` with
 * `std::source_location::current()` and the supplied Slice 6 payload fields.
 * @invariant This C++20 path is disabled when `MEMSAFE_CXX17_COMPAT` is
 * nonzero.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings and chain frames are
 * borrowed. Build `CONTEXT` in a named variable when aggregate commas would
 * otherwise be parsed as macro argument separators.
 *
 * @code
 * const memsafe::violation_context context{"owner", "borrow_mut", "Owner<int>",
 *                                          &owner, nullptr, nullptr};
 * MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(kind, message, context, chain, chain_count);
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(KIND, MSG, CONTEXT, CHAIN, CHAIN_SIZE) \
      ::memsafe::detail::report_violation((KIND),                                    \
                                          (MSG),                                     \
                                          ::std::source_location::current(),          \
                                          (CONTEXT),                                 \
                                          (CHAIN),                                   \
                                          (CHAIN_SIZE))

/**
 * @def MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN
 * @brief Report a Slice 6 violation with a borrow-chain payload.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @param CHAIN Pointer to a `memsafe::borrow_chain_frame` array, or null.
 * @param CHAIN_SIZE Number of frames available through `CHAIN`.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre Arguments are valid in the calling expression context. Any non-null
 * message or chain pointer remains valid for policy dispatch or exception
 * inspection.
 * @post Calls `MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT` with default
 * `memsafe::violation_context{}` and the supplied chain.
 * @invariant This helper keeps the common borrow-chain case concise while
 * preserving the same source-location behavior as `MEMSAFE_DETAIL_VIOLATE`.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings and chain frames are
 * borrowed.
 *
 * @code
 * MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN(kind, "borrow failed", chain, chain_count);
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN(KIND, MSG, CHAIN, CHAIN_SIZE) \
      MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(                               \
          (KIND), (MSG), ::memsafe::violation_context{}, (CHAIN), (CHAIN_SIZE))
#else
/**
 * @def MEMSAFE_DETAIL_VIOLATE
 * @brief Report a Slice 0 violation with fallback source fields.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre `KIND` and `MSG` are valid in the calling expression context, and any
 * non-null message pointer remains valid for policy dispatch or exception
 * inspection.
 * @post Calls `memsafe::detail::report_violation` with `__FILE__`, `__LINE__`,
 * and `__func__`.
 * @invariant This path is always available, including C++17 compatibility.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings are borrowed.
 *
 * @code
 * MEMSAFE_DETAIL_VIOLATE(memsafe::violation_kind::generation_mismatch,
 *                        "stale handle generation");
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE(KIND, MSG) \
      ::memsafe::detail::report_violation((KIND), (MSG), __FILE__, __LINE__, __func__)

/**
 * @def MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT
 * @brief Report a Slice 6 violation with context and fallback source fields.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @param CONTEXT Expression yielding `memsafe::violation_context`.
 * @param CHAIN Pointer to a `memsafe::borrow_chain_frame` array, or null.
 * @param CHAIN_SIZE Number of frames available through `CHAIN`.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre Arguments are valid in the calling expression context. Any non-null
 * message, context string, or chain pointer remains valid for policy dispatch
 * or exception inspection.
 * @post Calls `memsafe::detail::report_violation` with `__FILE__`, `__LINE__`,
 * `__func__`, and the supplied Slice 6 payload fields.
 * @invariant This path is always available, including C++17 compatibility.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings and chain frames are
 * borrowed. Build `CONTEXT` in a named variable when aggregate commas would
 * otherwise be parsed as macro argument separators.
 *
 * @code
 * const memsafe::violation_context context{"owner", "borrow_mut", "Owner<int>",
 *                                          &owner, nullptr, nullptr};
 * MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(kind, message, context, chain, chain_count);
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(KIND, MSG, CONTEXT, CHAIN, CHAIN_SIZE) \
      ::memsafe::detail::report_violation((KIND),                                    \
                                          (MSG),                                     \
                                          __FILE__,                                  \
                                          __LINE__,                                  \
                                          __func__,                                  \
                                          (CONTEXT),                                 \
                                          (CHAIN),                                   \
                                          (CHAIN_SIZE))

/**
 * @def MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN
 * @brief Report a Slice 6 violation with a borrow-chain payload.
 *
 * @param KIND Expression convertible to `memsafe::violation_kind`.
 * @param MSG Diagnostic message expression, or null for the default message.
 * @param CHAIN Pointer to a `memsafe::borrow_chain_frame` array, or null.
 * @param CHAIN_SIZE Number of frames available through `CHAIN`.
 * @return The macro expression returns only under HANDLER policy with an
 * installed handler that returns.
 * @pre Arguments are valid in the calling expression context. Any non-null
 * message or chain pointer remains valid for policy dispatch or exception
 * inspection.
 * @post Calls `MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT` with default
 * `memsafe::violation_context{}` and the supplied chain.
 * @invariant This helper keeps the common borrow-chain case concise while
 * preserving the same fallback source-field behavior as `MEMSAFE_DETAIL_VIOLATE`.
 * @throws `memsafe::violation` under THROW policy; installed handlers may throw
 * under HANDLER policy.
 * @note Ownership/thread-safety: generated payload strings and chain frames are
 * borrowed.
 *
 * @code
 * MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN(kind, "borrow failed", chain, chain_count);
 * @endcode
 */
#  define MEMSAFE_DETAIL_VIOLATE_WITH_CHAIN(KIND, MSG, CHAIN, CHAIN_SIZE) \
      MEMSAFE_DETAIL_VIOLATE_WITH_CONTEXT(                               \
          (KIND), (MSG), ::memsafe::violation_context{}, (CHAIN), (CHAIN_SIZE))
#endif

#undef MEMSAFE_DETAIL_LANG_VERSION
#undef MEMSAFE_DETAIL_HAS_SOURCE_LOCATION

#endif // MEMSAFE_VIOLATION_HPP
