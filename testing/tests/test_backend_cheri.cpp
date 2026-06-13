/**
 * @file test_backend_cheri.cpp
 * @brief Optional CHERI and Rusty-C++ backend smoke test for Slice 7.
 *
 * @details
 * Work package: CPP_MEMSAFE-0720-TEST.
 *
 * Purpose:
 * - Validate the finalized CPP_MEMSAFE-0700-FUNC vendor backend switches when
 *   an optional backend lane selects `MEMSAFE_BACKEND_CHERI` or
 *   `MEMSAFE_BACKEND_RUSTY_CPP`.
 * - Keep CPP_MEMSAFE-0720-TEST within its declared TEST artifact scope by
 *   submitting this self-gated test only; final optional `infra_lanes.json`
 *   wiring is owned by CPP_MEMSAFE-0920-TEST's terminal integration gate.
 *   This is the CPP_MEMSAFE-0720-TEST rework response for the Section 6A scope
 *   allowlist finding: this package emits no `testing/infra_lanes.json`
 *   artifact.
 * - Confirm that a vendor backend selection is mutually exclusive with the
 *   compiler-family backends auto-detected by the default lanes.
 * - Exercise the Slice 7 public annotation macros `MEMSAFE_OWNED`,
 *   `MEMSAFE_POINTER`, and `MEMSAFE_LOCK_HELD(m)` at ordinary declaration
 *   sites so unsupported vendor spellings remain no-ops instead of false
 *   positives.
 * - On a CHERI-capable target with `MEMSAFE_BACKEND_CHERI` selected, perform
 *   only in-bounds object, pointer, and `memsafe::Owner` operations. A real
 *   CHERI capability violation terminates the process and therefore fails the
 *   CTest executable under the F4 exit-code contract.
 *
 * Key invariants:
 * - Ordinary debug/release/sanitizer/C++17 lanes do not define a vendor
 *   backend, so this test reports a skip and exits successfully.
 * - A CHERI lane must define `MEMSAFE_BACKEND_CHERI` before including any
 *   memsafe header; this file never defines the backend macro itself.
 * - Capability-specific runtime checks and CHERI annotated call sites run only
 *   when both the CHERI backend macro and CHERI target/ABI feature macros are
 *   present.
 * - The test never intentionally manufactures an out-of-bounds or revoked
 *   capability because CPP_MEMSAFE-0720-TEST is an optional false-positive
 *   lane: any unexpected capability fault is already an executable failure.
 *
 * Ownership and thread-safety:
 * - The local annotation fixtures own only automatic scalar state. The
 *   `memsafe::Owner<int>` smoke path owns one heap allocation and releases it
 *   before process exit.
 * - The test is single-threaded and performs no synchronization beyond
 *   compiling a function annotated with `MEMSAFE_LOCK_HELD(m)`.
 *
 * Traceability:
 * - F1 Slice 7 optional vendor backends
 *   (`MEMSAFE_BACKEND_RUSTY_CPP`, `MEMSAFE_BACKEND_CHERI`).
 * - F2 Optional Back-End and Sanitizer Tests, including "CHERI capability
 *   violations = failure" on capable systems.
 * - F3 `Backend` enumeration values `Rusty_Cpp` and `CHERI`.
 */
#include "../test_harness.hpp"

#include <memsafe/backend.hpp>
#include <memsafe/owner.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>

/**
 * @def MEMSAFE_CHERI_TEST_STRINGIFY_IMPL
 * @brief Stringize an already-expanded token sequence for backend annotation
 * checks.
 *
 * @param ... Token sequence supplied by `MEMSAFE_CHERI_TEST_STRINGIFY`; the
 * sequence may be empty when an unsupported optional backend attribute degrades
 * to a no-op.
 * @return A string literal containing the preprocessor spelling of the supplied
 * token sequence, or an empty string for an empty expansion.
 * @pre Use only in this translation unit after including
 * `<memsafe/backend.hpp>`.
 * @post No runtime state is modified; the compiler creates a string literal.
 * @invariant The macro evaluates the supplied token sequence zero times.
 * @throws Nothing directly; malformed tokens are diagnosed by the
 * preprocessor or compiler.
 * @note Ownership/thread-safety: owns no runtime resources and has no
 * synchronization behavior.
 *
 * Example:
 * @code
 * constexpr const char* text =
 *     MEMSAFE_CHERI_TEST_STRINGIFY_IMPL([[nodiscard]]);
 * @endcode
 */
#define MEMSAFE_CHERI_TEST_STRINGIFY_IMPL(...) "" #__VA_ARGS__

/**
 * @def MEMSAFE_CHERI_TEST_STRINGIFY
 * @brief Expand then stringize a backend annotation macro.
 *
 * @param ... Token sequence to expand before stringification; may be empty
 * after macro expansion.
 * @return A string literal containing the expanded token spelling.
 * @pre The token sequence must be acceptable as one macro argument.
 * @post No runtime state is modified; the result is usable in compile-time or
 * runtime spelling checks.
 * @invariant The macro evaluates the supplied token sequence zero times, preserving the
 * annotation-only contract of the backend macros under test.
 * @throws Nothing directly; invalid macro arguments are preprocessing errors.
 * @note Ownership/thread-safety: produces no owned runtime resources.
 *
 * Example:
 * @code
 * constexpr const char* owned =
 *     MEMSAFE_CHERI_TEST_STRINGIFY(MEMSAFE_OWNED);
 * @endcode
 */
#define MEMSAFE_CHERI_TEST_STRINGIFY(...) \
    MEMSAFE_CHERI_TEST_STRINGIFY_IMPL(__VA_ARGS__)

/**
 * @def MEMSAFE_CHERI_TEST_TARGET_CAPABLE
 * @brief Report whether this translation unit is being compiled for a
 * CHERI-capable target or ABI.
 *
 * @retval 1 A known CHERI target macro such as `__CHERI__`,
 * `__CHERI_PURE_CAPABILITY__`, `__cheri__`, or
 * `__CHERI_CAPABILITY_WIDTH__` is present.
 * @retval 0 No known CHERI target macro is present.
 * @pre Evaluate after compiler predefined macros are available.
 * @post Capability-specific runtime probes can be gated without excluding the
 * source file from ordinary F4 lanes.
 * @invariant This macro describes target capability support only; it does not
 * select the memsafe backend by itself.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time environment selector only.
 */
#if defined(__CHERI__) || defined(__CHERI_PURE_CAPABILITY__) || \
    defined(__cheri__) || defined(__CHERI_CAPABILITY_WIDTH__)
#  define MEMSAFE_CHERI_TEST_TARGET_CAPABLE 1
#else
#  define MEMSAFE_CHERI_TEST_TARGET_CAPABLE 0
#endif

/**
 * @def MEMSAFE_CHERI_TEST_PURE_CAPABILITY_ABI
 * @brief Report whether the target uses a CHERI pure-capability ABI.
 *
 * @retval 1 `__CHERI_PURE_CAPABILITY__` is present.
 * @retval 0 The target is either not CHERI-capable or is not pure-capability.
 * @pre Evaluate after compiler predefined macros are available.
 * @post Diagnostics can distinguish hybrid and pure-capability CHERI lanes.
 * @invariant Hybrid CHERI targets may still be capability-capable even when
 * this macro is zero.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time environment selector only.
 */
#if defined(__CHERI_PURE_CAPABILITY__)
#  define MEMSAFE_CHERI_TEST_PURE_CAPABILITY_ABI 1
#else
#  define MEMSAFE_CHERI_TEST_PURE_CAPABILITY_ABI 0
#endif

/**
 * @def MEMSAFE_CHERI_TEST_CAPABILITY_ATTRIBUTE
 * @brief Apply a Clang thread-safety capability marker to the local guard type
 * when that spelling is available.
 *
 * @return A declaration attribute or an empty token sequence.
 * @pre Apply only after the active compiler's `__has_attribute` support has
 * been probed.
 * @post `MEMSAFE_LOCK_HELD(m)` can be compiled against a realistic capability
 * guard type on Clang-family toolchains while other compilers see a no-op.
 * @invariant Unsupported compilers never receive an unprobed vendor attribute.
 * @throws Nothing directly; invalid placement would be a compiler diagnostic.
 * @note Ownership/thread-safety: supplies static-analysis metadata only and
 * does not acquire a runtime lock.
 */
#if defined(__has_attribute)
#  if __has_attribute(capability)
#    define MEMSAFE_CHERI_TEST_CAPABILITY_ATTRIBUTE \
        __attribute__((capability("memsafe_cheri_guard")))
#  else
#    define MEMSAFE_CHERI_TEST_CAPABILITY_ATTRIBUTE
#  endif
#else
#  define MEMSAFE_CHERI_TEST_CAPABILITY_ATTRIBUTE
#endif

/**
 * @def MEMSAFE_CHERI_TEST_LOCK_HELD
 * @brief Apply the public `MEMSAFE_LOCK_HELD(m)` macro only in vendor-backend
 * lanes owned by this package.
 *
 * @param m Lock, guard, or capability expression forwarded to
 * `MEMSAFE_LOCK_HELD(m)` when a CHERI or Rusty-C++ backend is selected.
 * @return The active public lock/capability annotation in vendor lanes, or an
 * empty token sequence in ordinary compiler-family lanes.
 * @pre Include `<memsafe/backend.hpp>` before evaluating this helper so the
 * backend detail macros are defined.
 * @post Default Clang/GCC/MSVC lanes do not receive a thread-safety
 * precondition from this optional-backend test file.
 * @invariant The helper never evaluates @p m at runtime and never changes the
 * spelling of the public macro in a selected vendor lane.
 * @throws Nothing directly; invalid emitted attributes are diagnosed by the
 * compiler.
 * @note Ownership/thread-safety: this is declaration metadata only. It does
 * not acquire a lock or capability.
 *
 * Example:
 * @code
 * int value(const backend_capability_guard& guard)
 *     MEMSAFE_CHERI_TEST_LOCK_HELD(guard);
 * @endcode
 */
#if MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE || \
    MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE
#  define MEMSAFE_CHERI_TEST_LOCK_HELD(m) MEMSAFE_LOCK_HELD(m)
#else
#  define MEMSAFE_CHERI_TEST_LOCK_HELD(m)
#endif

namespace {

/// Whether the optional CHERI backend selector is active in this translation unit.
constexpr bool k_cheri_backend_selected =
    MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE == 1;

/// Whether the optional Rusty-C++ backend selector is active in this translation unit.
constexpr bool k_rusty_backend_selected =
    MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE == 1;

/// Whether either Slice 7 vendor backend selector is active.
constexpr bool k_vendor_backend_selected =
    k_cheri_backend_selected || k_rusty_backend_selected;

/// Whether compiler predefined macros identify the target as CHERI-capable.
constexpr bool k_cheri_target_capable =
    MEMSAFE_CHERI_TEST_TARGET_CAPABLE != 0;

/// Whether the target is explicitly using a pure-capability CHERI ABI.
constexpr bool k_cheri_pure_capability_abi =
    MEMSAFE_CHERI_TEST_PURE_CAPABILITY_ABI != 0;

/// Spelling emitted by `MEMSAFE_OWNED` for the active backend.
constexpr const char k_owned_expansion[] =
    MEMSAFE_CHERI_TEST_STRINGIFY(MEMSAFE_OWNED);

/// Spelling emitted by `MEMSAFE_POINTER` for the active backend.
constexpr const char k_pointer_expansion[] =
    MEMSAFE_CHERI_TEST_STRINGIFY(MEMSAFE_POINTER);

/// Spelling emitted by `MEMSAFE_LOCK_HELD(m)` for the active backend.
constexpr const char k_lock_held_expansion[] =
    MEMSAFE_CHERI_TEST_STRINGIFY(MEMSAFE_LOCK_HELD(guard));

/// Spelling emitted by `MEMSAFE_BORROWS(x)` for vendor-backend isolation checks.
constexpr const char k_borrows_expansion[] =
    MEMSAFE_CHERI_TEST_STRINGIFY(MEMSAFE_BORROWS(source));

/**
 * @brief Compare two null-terminated byte strings during constant evaluation.
 *
 * @param lhs Left-hand string pointer; must not be null.
 * @param rhs Right-hand string pointer; must not be null.
 * @return `true` when both strings contain exactly the same byte sequence;
 * otherwise `false`.
 * @pre `lhs` and `rhs` point to valid null-terminated byte strings.
 * @post No storage is modified and no ownership is transferred.
 * @invariant The comparison is byte-for-byte and locale-independent.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows both strings for the duration of the
 * call and is reentrant.
 *
 * Example:
 * @code
 * static_assert(c_string_equal("CHERI", "CHERI"));
 * @endcode
 */
[[maybe_unused]] constexpr bool c_string_equal(const char* lhs,
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

/**
 * @brief Return whether a string contains a non-empty token.
 *
 * @param text String to search; null is accepted.
 * @param token Token to find; null is accepted.
 * @retval true Both inputs are non-null, @p token is non-empty, and @p token
 * appears in @p text.
 * @retval false Either input is null, @p token is empty, or @p token is absent.
 * @pre Non-null pointers must refer to valid null-terminated byte strings.
 * @post No state is modified.
 * @invariant Null inputs are rejected before `std::strstr` is called.
 * @throws Nothing.
 * @note Ownership/thread-safety: borrows both strings only during the call and
 * touches no shared state.
 *
 * Example:
 * @code
 * const bool has_marker = contains_text("[[cheri::owner]]", "cheri");
 * @endcode
 */
bool contains_text(const char* text, const char* token) noexcept {
    return text != nullptr && token != nullptr && token[0] != '\0' &&
           std::strstr(text, token) != nullptr;
}

/**
 * @brief Emit a stable skip diagnostic without changing the test verdict.
 *
 * @param reason Human-readable reason for skipping an environment-gated path.
 * @return No value.
 * @pre @p reason is either null or points to a null-terminated string.
 * @post One line is written to stdout.
 * @invariant Skip reporting never increments the harness failure count.
 * @throws Nothing intentionally; C stdio failures are ignored.
 * @note Ownership/thread-safety: writes from the single test thread only.
 *
 * Example:
 * @code
 * report_skip("MEMSAFE_BACKEND_CHERI not selected");
 * @endcode
 */
void report_skip(const char* reason) noexcept {
    std::fprintf(stdout, "SKIP: %s\n",
                 reason == nullptr ? "optional CHERI backend lane unavailable"
                                   : reason);
}

/**
 * @brief Return a compact description of the CHERI target environment.
 *
 * @return Static text naming the compile-time CHERI capability mode.
 * @pre None.
 * @post No state is modified.
 * @invariant The return value has static storage duration.
 * @throws Nothing.
 * @note Ownership/thread-safety: returns immutable static text and is
 * reentrant.
 *
 * Example:
 * @code
 * std::fprintf(stdout, "%s\n", cheri_target_description());
 * @endcode
 */
[[maybe_unused]] const char* cheri_target_description() noexcept {
    if (k_cheri_pure_capability_abi) {
        return "CHERI pure-capability ABI";
    }
    if (k_cheri_target_capable) {
        return "CHERI-capable target";
    }
    return "non-CHERI target";
}

/**
 * @brief Small owning fixture annotated through `MEMSAFE_OWNED`.
 *
 * @pre Construct with any integer value.
 * @post The value is stored directly in the fixture.
 * @invariant The object owns exactly one scalar payload and never aliases
 * another fixture.
 * @throws Nothing; construction and member functions are `noexcept`.
 * @note Ownership/thread-safety: automatic-storage value type; callers own
 * synchronization and this test uses it on one thread.
 *
 * Example:
 * @code
 * backend_owned_cell cell(7);
 * cell.set(9);
 * @endcode
 */
class MEMSAFE_OWNED backend_owned_cell final {
public:
    /**
     * @brief Construct the annotated owner fixture.
     *
     * @param value Initial scalar payload.
     * @return Constructors do not return a value.
     * @pre No precondition; all integer values are accepted.
     * @post `get() == value`.
     * @invariant The payload remains owned by this object.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes only this object.
     */
    explicit backend_owned_cell(int value) noexcept : value_(value) {}

    /**
     * @brief Return the owned scalar payload.
     *
     * @return Current payload value.
     * @pre The object is alive.
     * @post No state is modified.
     * @invariant Returned values are copied out; no reference escapes.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only; no synchronization.
     */
    int get() const noexcept {
        return value_;
    }

    /**
     * @brief Replace the owned scalar payload.
     *
     * @param value New scalar payload.
     * @return No value.
     * @pre The object is alive and exclusively accessed.
     * @post `get() == value`.
     * @invariant Ownership of the payload remains with this object.
     * @throws Nothing.
     * @note Ownership/thread-safety: unsynchronized mutation used on one test
     * thread.
     */
    void set(int value) noexcept {
        value_ = value;
    }

private:
    /// Owned scalar payload used by the CHERI in-bounds access smoke path.
    int value_;
};

/**
 * @brief Non-owning view fixture annotated through `MEMSAFE_POINTER`.
 *
 * @pre Construct from a non-null `backend_owned_cell*`.
 * @post The view stores the pointer without taking ownership.
 * @invariant All public operations dereference only the stored pointer and the
 * test constructs it from a live object.
 * @throws Nothing; construction and member functions are `noexcept`.
 * @note Ownership/thread-safety: non-owning single-threaded view.
 *
 * Example:
 * @code
 * backend_owned_cell cell(1);
 * backend_pointer_view view(&cell);
 * view.set(2);
 * @endcode
 */
class MEMSAFE_POINTER backend_pointer_view final {
public:
    /**
     * @brief Construct a non-owning annotated view.
     *
     * @param cell Pointer to the live owned fixture to view.
     * @return Constructors do not return a value.
     * @pre `cell != nullptr` and the pointed object outlives this view.
     * @post `points_to(cell)` is true.
     * @invariant The constructor does not copy or allocate the pointee.
     * @throws Nothing.
     * @note Ownership/thread-safety: borrows @p cell and assumes
     * single-threaded access.
     */
    explicit backend_pointer_view(backend_owned_cell* cell) noexcept
        : cell_(cell) {}

    /**
     * @brief Report whether this view points at a specific cell.
     *
     * @param cell Candidate pointee address.
     * @retval true The stored pointer equals @p cell.
     * @retval false The stored pointer differs from @p cell.
     * @pre No precondition; @p cell may be null.
     * @post No state is modified.
     * @invariant Pointer equality is tested only, with no dereference.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only.
     */
    bool points_to(const backend_owned_cell* cell) const noexcept {
        return cell_ == cell;
    }

    /**
     * @brief Read the viewed cell's value.
     *
     * @return Current value of the viewed cell.
     * @pre The stored pointer is non-null and points to a live cell.
     * @post No state is modified by this view.
     * @invariant The function performs exactly one in-bounds object
     * dereference in the CHERI capability path.
     * @throws Nothing.
     * @note Ownership/thread-safety: non-owning read on one thread.
     */
    int get() const noexcept {
        return cell_->get();
    }

    /**
     * @brief Replace the viewed cell's value.
     *
     * @param value New value to store through the view.
     * @return No value.
     * @pre The stored pointer is non-null, points to a live cell, and is
     * exclusively accessed.
     * @post `get() == value`.
     * @invariant The function performs only an in-bounds object dereference.
     * @throws Nothing.
     * @note Ownership/thread-safety: non-owning write on one thread.
     */
    void set(int value) const noexcept {
        cell_->set(value);
    }

private:
    /// Non-owning pointer to the annotated owner fixture.
    backend_owned_cell* cell_;
};

/**
 * @brief Local guard type used to compile `MEMSAFE_LOCK_HELD(m)` call
 * contracts.
 *
 * @pre Construct with a nonzero token when the guarded function should return
 * the protected payload.
 * @post The token is stored by value.
 * @invariant The guard owns only a scalar token; it is not a real mutex and is
 * used solely as an annotation target.
 * @throws Nothing; construction and member functions are `noexcept`.
 * @note Ownership/thread-safety: no actual lock is acquired. The type supplies
 * optional static-analysis metadata only.
 *
 * Example:
 * @code
 * backend_capability_guard guard(1);
 * (void)guard.present();
 * @endcode
 */
class MEMSAFE_CHERI_TEST_CAPABILITY_ATTRIBUTE backend_capability_guard final {
public:
    /**
     * @brief Construct the local annotation guard.
     *
     * @param token Nonzero token used by the smoke path.
     * @return Constructors do not return a value.
     * @pre No precondition; zero is accepted but the test uses nonzero.
     * @post `present()` reflects whether @p token was nonzero.
     * @invariant The token is immutable after construction.
     * @throws Nothing.
     * @note Ownership/thread-safety: initializes only this object.
     */
    explicit backend_capability_guard(int token) noexcept : token_(token) {}

    /**
     * @brief Report whether the guard token is present.
     *
     * @retval true The stored token is nonzero.
     * @retval false The stored token is zero.
     * @pre The guard object is alive.
     * @post No state is modified.
     * @invariant The result is stable for the object's lifetime.
     * @throws Nothing.
     * @note Ownership/thread-safety: observer only.
     */
    bool present() const noexcept {
        return token_ != 0;
    }

private:
    /// Immutable token used so the guard has observable runtime state.
    int token_;
};

/**
 * @brief Read a pointer view under a documented lock/capability precondition.
 *
 * @param guard Guard expression forwarded to
 * `MEMSAFE_CHERI_TEST_LOCK_HELD(m)`.
 * @param view Non-owning view to read.
 * @return The viewed value when @p guard is present; otherwise `-1`.
 * @pre `view` points to a live `backend_owned_cell`.
 * @post Neither @p guard nor @p view is modified.
 * @invariant The function's only backend-specific behavior is the declaration
 * annotation emitted by `MEMSAFE_CHERI_TEST_LOCK_HELD(guard)`.
 * @throws Nothing.
 * @note Ownership/thread-safety: reads through a non-owning view on one thread;
 * the annotation itself acquires no runtime lock. Positive smoke paths compile
 * this function but do not call it, because an active backend may legitimately
 * diagnose calls that do not prove the required capability is held.
 *
 * Example:
 * @code
 * backend_capability_guard guard(1);
 * backend_owned_cell cell(5);
 * backend_pointer_view view(&cell);
 * const auto annotated_entry_point = &guarded_view_value;
 * (void)annotated_entry_point;
 * @endcode
 */
[[maybe_unused]] int guarded_view_value(const backend_capability_guard& guard,
                                        const backend_pointer_view& view)
    MEMSAFE_CHERI_TEST_LOCK_HELD(guard) {
    return guard.present() ? view.get() : -1;
}

/**
 * @brief Verify that vendor backend selection remains mutually exclusive.
 *
 * @return No value.
 * @pre `<memsafe/backend.hpp>` has been included.
 * @post Harness failures are recorded if a vendor backend coexists with any
 * compiler-family backend or another vendor backend.
 * @invariant `MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT` is the single source of
 * truth for the selected backend count.
 * @throws Nothing intentionally; CHECK records failures instead of throwing.
 * @note Ownership/thread-safety: preprocessor constants only.
 */
void verify_vendor_backend_exclusivity() {
    if (!k_vendor_backend_selected) {
        return;
    }

    CHECK(MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT == 1);
    CHECK(MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE == 0);
    CHECK(MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE == 0);
    CHECK(MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE == 0);

    if (k_cheri_backend_selected) {
        CHECK(MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE == 0);
    }
    if (k_rusty_backend_selected) {
        CHECK(MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE == 0);
    }
}

/**
 * @brief Verify that annotation macro spelling does not cross vendor lanes.
 *
 * @return No value.
 * @pre A vendor backend is active.
 * @post Harness failures are recorded if a CHERI lane emits Rusty-C++ markers,
 * a Rusty-C++ lane emits CHERI markers, or vendor lanes unexpectedly inherit
 * Clang lifetime borrow annotations from the compiler-family backend.
 * @invariant Empty vendor expansions are accepted, and future vendor-specific
 * `MEMSAFE_BORROWS(x)` expansions are also accepted provided they do not cross
 * backend families.
 * @throws Nothing intentionally; CHECK records failures instead of throwing.
 * @note Ownership/thread-safety: reads compiler-owned string literals only.
 */
void verify_vendor_annotation_spelling() {
    if (!k_vendor_backend_selected) {
        return;
    }

    /*
     * PRD Slice 7 allows vendor backends to gain their own annotation
     * spellings. The false-positive guard here is narrower than exact equality
     * with an empty string: a vendor lane must not accidentally inherit the
     * Clang lifetime/coroutine lifetime hook from the compiler-family lane.
     */
    CHECK(!contains_text(k_borrows_expansion, "clang::"));
    CHECK(!contains_text(k_borrows_expansion, "lifetimebound"));

    if (k_cheri_backend_selected) {
        CHECK(!contains_text(k_owned_expansion, "rusty"));
        CHECK(!contains_text(k_pointer_expansion, "rusty"));
        CHECK(!contains_text(k_lock_held_expansion, "rusty"));
        CHECK(!contains_text(k_borrows_expansion, "rusty"));
    }

    if (k_rusty_backend_selected) {
        CHECK(!contains_text(k_owned_expansion, "cheri"));
        CHECK(!contains_text(k_pointer_expansion, "cheri"));
        CHECK(!contains_text(k_lock_held_expansion, "cheri"));
        CHECK(!contains_text(k_borrows_expansion, "cheri"));
    }
}

/**
 * @brief Exercise the macro-decorated local fixtures when the selected vendor
 * lane is applicable.
 *
 * @return No value.
 * @pre A Rusty-C++ backend is active, a CHERI backend is active on a
 * CHERI-capable target, or the caller is willing to receive a skip.
 * @post Harness failures are recorded if ordinary in-bounds operations through
 * annotated declarations regress.
 * @invariant The smoke path never depends on an emitted vendor spelling and
 * never calls the `MEMSAFE_LOCK_HELD(m)` function without proving a held
 * capability. This validates that selected owner/pointer attributes introduce
 * no compile-time or runtime false positives.
 * @throws Nothing intentionally; CHECK records failures instead of throwing.
 * @note Ownership/thread-safety: all objects are automatic storage and are used
 * on one thread.
 */
void exercise_annotation_surface() {
#if MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE || \
    (MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE && MEMSAFE_CHERI_TEST_TARGET_CAPABLE)
    backend_owned_cell cell(31);
    backend_pointer_view view(&cell);
    backend_capability_guard guard(1);

    CHECK(view.points_to(&cell));
    CHECK(view.get() == 31);

    view.set(37);
    CHECK(cell.get() == 37);
    CHECK(view.get() == 37);
    CHECK(guard.present());
#elif MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE
    /*
     * F4 marks the CHERI lane environment-gated. On a non-CHERI target, avoid
     * executing annotated call sites so a forced local `MEMSAFE_BACKEND_CHERI`
     * smoke run is an explicit skip rather than a portability false positive.
     */
    report_skip("MEMSAFE_BACKEND_CHERI selected on a non-CHERI target");
#else
    report_skip("no CHERI or Rusty-C++ backend selected");
#endif
}

/**
 * @brief Exercise safe `memsafe::Owner<int>` access on a CHERI-capable lane.
 *
 * @return No value.
 * @pre `MEMSAFE_BACKEND_CHERI` is selected and CHERI target feature macros are
 * present.
 * @post Harness failures are recorded if safe ownership and borrow operations
 * fail. On CHERI hardware, any capability violation terminates the process and
 * fails the executable rather than being converted into a passing result.
 * @invariant The test creates only valid borrows and performs only in-bounds
 * reads and writes.
 * @throws `std::bad_alloc` if the owner allocation fails; that surfaces as a
 * test failure under the process exit-code contract.
 * @note Ownership/thread-safety: the owner holds one heap object and all
 * borrows end before owner destruction on the same thread.
 */
[[maybe_unused]] void exercise_cheri_owner_safe_path() {
    memsafe::Owner<int> owner(41);
    CHECK(owner.has_value());
    CHECK(*owner == 41);

    {
        const memsafe::Ref<int> shared = owner.borrow();
        CHECK(*shared == 41);
    }

    {
        memsafe::MutRef<int> exclusive = owner.borrow_mut();
        *exclusive = 43;
        CHECK(*exclusive == 43);
    }

    CHECK(*owner == 43);
}

/**
 * @brief Run the CHERI target-gated capability smoke path.
 *
 * @return No value.
 * @pre The caller has already run backend-exclusivity and annotation checks.
 * @post Capability-specific checks run only on a CHERI backend and target; all
 * other environments emit a skip without changing the harness verdict.
 * @invariant No intentional capability violation is generated. The F2 oracle is
 * the process itself: a hardware capability fault is an abnormal test exit and
 * therefore a failure.
 * @throws `std::bad_alloc` from the owner smoke path if allocation fails.
 * @note Ownership/thread-safety: single-threaded in-bounds memory access only.
 */
void exercise_cheri_capability_path_if_available() {
#if MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE && MEMSAFE_CHERI_TEST_TARGET_CAPABLE
    std::fprintf(stdout, "INFO: running CHERI capability smoke path (%s)\n",
                 cheri_target_description());

    backend_owned_cell cell(51);
    backend_pointer_view view(&cell);
    backend_capability_guard guard(1);

    CHECK(view.points_to(&cell));
    CHECK(view.get() == 51);
    CHECK(guard.present());
    view.set(53);
    CHECK(cell.get() == 53);
    CHECK(view.get() == 53);

    /*
     * F2 Optional Back-End and Sanitizer Tests require any CHERI capability
     * violation to fail. We do not catch signals or recover here; if the safe
     * owner/borrow path trips a capability fault, CTest observes the abnormal
     * process termination as the required failure.
     */
    exercise_cheri_owner_safe_path();
#else
    if (!k_cheri_backend_selected) {
        report_skip("MEMSAFE_BACKEND_CHERI not selected");
        return;
    }

    if (!k_cheri_target_capable) {
        report_skip("MEMSAFE_BACKEND_CHERI selected on a non-CHERI target");
        return;
    }

#endif
}

} // namespace

/**
 * @brief Execute the optional CHERI/Rusty-C++ backend smoke test.
 *
 * @retval 0 No harness checks failed; ordinary lanes may report a skip because
 * no vendor backend is selected.
 * @retval 1 One or more harness checks failed.
 * @pre The executable is built as a standalone F4 runtime test with the
 * project include directory available. Optional CHERI capability checks require
 * a build lane that defines `MEMSAFE_BACKEND_CHERI` on a CHERI-capable system.
 * @post The test harness prints the final summary and returns the process
 * verdict.
 * @invariant Capability faults, sanitizer failures, and uncaught exceptions are
 * not masked by this test; they fail the executable naturally.
 * @throws Nothing intentionally; unexpected exceptions surface through the
 * runner's normal executable failure contract.
 * @note Ownership/thread-safety: all test actions run on the main thread.
 */
int main() {
    verify_vendor_backend_exclusivity();
    verify_vendor_annotation_spelling();
    exercise_annotation_surface();
    exercise_cheri_capability_path_if_available();

    RUN_TESTS("test_backend_cheri");
}
