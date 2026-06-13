/**
 * @file backend.hpp
 * @brief Backend detection and annotation macros for libmemsafe.
 *
 * @details
 * Work package: CPP_MEMSAFE-0700-FUNC.
 *
 * Purpose:
 * - Preserve the compiler-family backend switches `MEMSAFE_BACKEND_CLANG`,
 *   `MEMSAFE_BACKEND_GCC`, and `MEMSAFE_BACKEND_MSVC` established by
 *   CPP_MEMSAFE-0020-FUNC.
 * - Add the optional vendor backend switches `MEMSAFE_BACKEND_RUSTY_CPP` and
 *   `MEMSAFE_BACKEND_CHERI` required by PRD milestone Slice 7 and the
 *   architecture model's `Slice_Vendor_Backends` component.
 * - Auto-detect exactly one compiler-family backend for Clang, GCC, or MSVC
 *   translation units when the user has not pre-defined any known backend
 *   switch. Rusty-C++ and CHERI remain build-selected optional backends.
 * - Provide the public annotation macros `MEMSAFE_NODISCARD`,
 *   `MEMSAFE_LIFETIMEBOUND`, and `MEMSAFE_BORROWS(x)` required by Slice 0,
 *   including the Clang ordinary and coroutine lifetime hooks required by the
 *   declared public macro contract.
 * - Provide the Slice 7 public annotation macros `MEMSAFE_OWNED`,
 *   `MEMSAFE_POINTER`, and `MEMSAFE_LOCK_HELD(m)` with Clang, Rusty-C++, and
 *   CHERI vendor-attribute branches.
 *
 * Key invariants:
 * - A user-provided known `MEMSAFE_BACKEND_*` macro is authoritative and
 *   suppresses compiler-family autodetection.
 * - Clang is tested before GCC and MSVC because Clang-family compilers commonly
 *   define compatibility macros such as `__GNUC__` and, for clang-cl, `_MSC_VER`.
 * - Rusty-C++ and CHERI are not auto-detected; they are optional vendor lanes
 *   that must be selected explicitly by the build.
 * - Defining more than one known backend switch is a configuration error.
 * - `MEMSAFE_NODISCARD` always expands to the standard `[[nodiscard]]`
 *   attribute, so discard diagnostics are backend-independent and do not rely
 *   on vendor attribute spellings.
 * - `MEMSAFE_BORROWS(x)` composes Clang's ordinary `lifetimebound` annotation
 *   with Clang's coroutine `coro_lifetimebound` annotation for the Clang
 *   backend, and every non-Clang or no-backend build sees an empty expansion.
 * - The coroutine lifetime hook remains named internally as
 *   `MEMSAFE_DETAIL_CORO_LIFETIMEBOUND`, but it is part of the public
 *   `MEMSAFE_BORROWS(x)` expansion so the Slice 0 symbol contract is visible
 *   through the declared macro rather than through a separate user-facing API.
 * - The ordinary and coroutine lifetime annotations are feature-probed
 *   independently so an older Clang that lacks one spelling degrades only that
 *   component while preserving the Slice 0 macro surface.
 * - Every Slice 7 vendor attribute is gated first on the selected backend and
 *   then on `__has_cpp_attribute` or `__has_attribute`; unsupported compilers,
 *   unsupported spellings, and non-selected backends see empty expansions.
 * - Default builds are unaffected because the new macros are additive and no
 *   unsupported attribute spelling is emitted.
 *
 * Ownership and thread-safety:
 * - This header declares no storage, performs no allocation, and owns no
 *   resources. All behavior is compile-time preprocessor configuration.
 * - Including the header is thread-safe by construction because it has no
 *   runtime state.
 *
 * @note This header intentionally remains a compile-time-only preprocessor
 * boundary. Runtime ownership, borrow counting, lock acquisition, and CHERI
 * hardware enforcement are implemented by later feature headers or the target
 * platform, not by these macros.
 */
#ifndef MEMSAFE_BACKEND_HPP
#define MEMSAFE_BACKEND_HPP

/**
 * @def MEMSAFE_BACKEND_CLANG
 * @brief Select the Clang compiler-family backend for Slice 0 annotations.
 *
 * @details
 * This macro may be defined by user code before including this header, or it is
 * auto-defined by this header when `__clang__` is present and no known backend
 * switch has already been provided. The macro is considered active when it is
 * defined, regardless of its replacement value, because preprocessor selection
 * macros are conventionally presence-based.
 *
 * @retval 1 The value assigned by autodetection when Clang is selected.
 * @pre Define at most one known `MEMSAFE_BACKEND_*` switch before including
 * this header.
 * @post `MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE` is `1` when this macro is
 * defined; Clang lifetime attributes may be emitted after compiler feature
 * probing.
 * @invariant This backend is mutually exclusive with the GCC, MSVC,
 * Rusty-C++, and CHERI backends.
 * @throws Nothing directly; selecting multiple backends triggers a
 * preprocessing `#error`.
 * @note Ownership/thread-safety: this macro owns no storage and changes no
 * runtime synchronization behavior.
 *
 * Example:
 * @code
 * #define MEMSAFE_BACKEND_CLANG 1
 * #include <memsafe/backend.hpp>
 * @endcode
 */

/**
 * @def MEMSAFE_BACKEND_GCC
 * @brief Select the GCC compiler-family backend for portable Slice 0 builds.
 *
 * @details
 * This macro may be defined by user code before including this header, or it is
 * auto-defined by this header when `__GNUC__` is present, `__clang__` is absent,
 * and no known backend switch has already been provided. GCC has no Slice 0
 * vendor lifetime attributes in this package, so `MEMSAFE_BORROWS(x)` remains a
 * no-op under this backend.
 *
 * @retval 1 The value assigned by autodetection when GCC is selected.
 * @pre Define at most one known `MEMSAFE_BACKEND_*` switch before including
 * this header.
 * @post `MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE` is `1` when this macro is defined;
 * borrow annotations expand to nothing.
 * @invariant This backend is mutually exclusive with the Clang, MSVC,
 * Rusty-C++, and CHERI backends.
 * @throws Nothing directly; selecting multiple backends triggers a
 * preprocessing `#error`.
 * @note Ownership/thread-safety: this macro owns no storage and changes no
 * runtime synchronization behavior.
 *
 * Example:
 * @code
 * #define MEMSAFE_BACKEND_GCC 1
 * #include <memsafe/backend.hpp>
 * @endcode
 */

/**
 * @def MEMSAFE_BACKEND_MSVC
 * @brief Select the MSVC compiler-family backend for portable Slice 0 builds.
 *
 * @details
 * This macro may be defined by user code before including this header, or it is
 * auto-defined by this header when `_MSC_VER` is present, `__clang__` and
 * `__GNUC__` are absent, and no known backend switch has already been
 * provided. MSVC has no Slice 0 vendor lifetime attributes in this package, so
 * `MEMSAFE_BORROWS(x)` remains a no-op under this backend.
 *
 * @retval 1 The value assigned by autodetection when MSVC is selected.
 * @pre Define at most one known `MEMSAFE_BACKEND_*` switch before including
 * this header.
 * @post `MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE` is `1` when this macro is defined;
 * borrow annotations expand to nothing.
 * @invariant This backend is mutually exclusive with the Clang, GCC,
 * Rusty-C++, and CHERI backends.
 * @throws Nothing directly; selecting multiple backends triggers a
 * preprocessing `#error`.
 * @note Ownership/thread-safety: this macro owns no storage and changes no
 * runtime synchronization behavior.
 *
 * Example:
 * @code
 * #define MEMSAFE_BACKEND_MSVC 1
 * #include <memsafe/backend.hpp>
 * @endcode
 */

/**
 * @def MEMSAFE_BACKEND_RUSTY_CPP
 * @brief Select the optional Rusty-C++ analysis backend for Slice 7
 * annotations.
 *
 * @details
 * This macro is user- or build-system-defined before including this header.
 * It is not auto-detected because PRD milestone Slice 7 describes Rusty-C++ as
 * an optional vendor backend, not a compiler-family default. When selected,
 * `MEMSAFE_OWNED`, `MEMSAFE_POINTER`, and `MEMSAFE_LOCK_HELD(m)` probe for
 * Rusty-C++ attribute spellings and otherwise expand to no-ops.
 *
 * @retval 1 Conventional user-provided value when Rusty-C++ is selected; this
 * header treats macro presence as authoritative regardless of replacement
 * value.
 * @pre Define at most one known `MEMSAFE_BACKEND_*` switch before including
 * this header.
 * @post `MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE` is `1` when this macro is
 * defined; Slice 7 Rusty-C++ annotation branches may emit supported vendor
 * attributes after feature probing.
 * @invariant This backend is mutually exclusive with the Clang, GCC, MSVC, and
 * CHERI backends.
 * @throws Nothing directly; selecting multiple backends triggers a
 * preprocessing `#error`.
 * @note Ownership/thread-safety: this macro owns no storage and changes no
 * runtime synchronization behavior. It only selects compile-time annotations.
 *
 * Example:
 * @code
 * #define MEMSAFE_BACKEND_RUSTY_CPP 1
 * #include <memsafe/backend.hpp>
 * @endcode
 */

/**
 * @def MEMSAFE_BACKEND_CHERI
 * @brief Select the optional CHERI capability backend for Slice 7 annotations.
 *
 * @details
 * This macro is user- or build-system-defined before including this header.
 * It is not auto-detected because CHERI is a target/ABI lane that must be
 * selected deliberately. When selected, `MEMSAFE_OWNED`, `MEMSAFE_POINTER`,
 * and `MEMSAFE_LOCK_HELD(m)` probe for CHERI-specific spellings and for Clang
 * annotation fallbacks that preserve CHERI intent on CHERI-aware Clang
 * toolchains.
 *
 * @retval 1 Conventional user-provided value when CHERI is selected; this
 * header treats macro presence as authoritative regardless of replacement
 * value.
 * @pre Define at most one known `MEMSAFE_BACKEND_*` switch before including
 * this header.
 * @post `MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE` is `1` when this macro is
 * defined; Slice 7 CHERI annotation branches may emit supported vendor
 * attributes after feature probing.
 * @invariant This backend is mutually exclusive with the Clang, GCC, MSVC, and
 * Rusty-C++ backends.
 * @throws Nothing directly; selecting multiple backends triggers a
 * preprocessing `#error`.
 * @note Ownership/thread-safety: this macro owns no storage and changes no
 * runtime synchronization behavior. CHERI runtime enforcement is provided by
 * the target hardware and ABI, not by this selector.
 *
 * Example:
 * @code
 * #define MEMSAFE_BACKEND_CHERI 1
 * #include <memsafe/backend.hpp>
 * @endcode
 */

/*
 * PRD Slice 0 requires manual override to be honored, and Slice 7 adds two
 * optional vendor lanes. Therefore autodetection runs only when none of the
 * known backend switches has been defined already.
 */
#if !defined(MEMSAFE_BACKEND_CLANG) && \
    !defined(MEMSAFE_BACKEND_GCC) && \
    !defined(MEMSAFE_BACKEND_MSVC) && \
    !defined(MEMSAFE_BACKEND_RUSTY_CPP) && \
    !defined(MEMSAFE_BACKEND_CHERI)
/*
 * Clang must be first because it advertises GCC compatibility through
 * `__GNUC__`; clang-cl can also advertise MSVC compatibility through
 * `_MSC_VER`. Rusty-C++ and CHERI are intentionally not auto-detected because
 * Slice 7 treats them as optional vendor lanes selected by the build.
 */
#  if defined(__clang__)
#    define MEMSAFE_BACKEND_CLANG 1
#  elif defined(__GNUC__)
#    define MEMSAFE_BACKEND_GCC 1
#  elif defined(_MSC_VER)
#    define MEMSAFE_BACKEND_MSVC 1
#  endif
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE
 * @brief Report whether the Clang backend switch is active in this translation
 * unit.
 *
 * @retval 1 `MEMSAFE_BACKEND_CLANG` is defined.
 * @retval 0 `MEMSAFE_BACKEND_CLANG` is not defined.
 * @pre Include this header before testing the helper macro.
 * @post No runtime state is modified; the value is available for preprocessor
 * checks and `static_assert` expressions.
 * @invariant The value is derived from macro presence, not from the replacement
 * token assigned to `MEMSAFE_BACKEND_CLANG`.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#if defined(MEMSAFE_BACKEND_CLANG)
#  define MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE 1
#else
#  define MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE 0
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE
 * @brief Report whether the GCC backend switch is active in this translation
 * unit.
 *
 * @retval 1 `MEMSAFE_BACKEND_GCC` is defined.
 * @retval 0 `MEMSAFE_BACKEND_GCC` is not defined.
 * @pre Include this header before testing the helper macro.
 * @post No runtime state is modified; the value is available for preprocessor
 * checks and `static_assert` expressions.
 * @invariant The value is derived from macro presence, not from the replacement
 * token assigned to `MEMSAFE_BACKEND_GCC`.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#if defined(MEMSAFE_BACKEND_GCC)
#  define MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE 1
#else
#  define MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE 0
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE
 * @brief Report whether the MSVC backend switch is active in this translation
 * unit.
 *
 * @retval 1 `MEMSAFE_BACKEND_MSVC` is defined.
 * @retval 0 `MEMSAFE_BACKEND_MSVC` is not defined.
 * @pre Include this header before testing the helper macro.
 * @post No runtime state is modified; the value is available for preprocessor
 * checks and `static_assert` expressions.
 * @invariant The value is derived from macro presence, not from the replacement
 * token assigned to `MEMSAFE_BACKEND_MSVC`.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#if defined(MEMSAFE_BACKEND_MSVC)
#  define MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE 1
#else
#  define MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE 0
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE
 * @brief Report whether the Rusty-C++ backend switch is active in this
 * translation unit.
 *
 * @retval 1 `MEMSAFE_BACKEND_RUSTY_CPP` is defined.
 * @retval 0 `MEMSAFE_BACKEND_RUSTY_CPP` is not defined.
 * @pre Include this header before testing the helper macro.
 * @post No runtime state is modified; the value is available for preprocessor
 * checks and `static_assert` expressions.
 * @invariant The value is derived from macro presence, not from the replacement
 * token assigned to `MEMSAFE_BACKEND_RUSTY_CPP`.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#if defined(MEMSAFE_BACKEND_RUSTY_CPP)
#  define MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE 1
#else
#  define MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE 0
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE
 * @brief Report whether the CHERI backend switch is active in this translation
 * unit.
 *
 * @retval 1 `MEMSAFE_BACKEND_CHERI` is defined.
 * @retval 0 `MEMSAFE_BACKEND_CHERI` is not defined.
 * @pre Include this header before testing the helper macro.
 * @post No runtime state is modified; the value is available for preprocessor
 * checks and `static_assert` expressions.
 * @invariant The value is derived from macro presence, not from the replacement
 * token assigned to `MEMSAFE_BACKEND_CHERI`.
 * @throws Nothing; it expands to an integer literal.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#if defined(MEMSAFE_BACKEND_CHERI)
#  define MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE 1
#else
#  define MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE 0
#endif

/**
 * @def MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT
 * @brief Count active backend switches.
 *
 * @return Integer constant expression equal to the number of active compiler
 * and vendor backend switches among Clang, GCC, MSVC, Rusty-C++, and CHERI.
 * @pre Include this header before using the helper macro.
 * @post The value can be used in preprocessing expressions and `static_assert`
 * checks.
 * @invariant Supported compiler autodetection produces `1`; unknown compilers
 * without a manual backend produce `0`; a single manually selected vendor
 * backend produces `1`; conflicting selections produce a preprocessing error
 * below.
 * @throws Nothing; it expands to an integer constant expression.
 * @note Ownership/thread-safety: compile-time helper only.
 */
#define MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT                  \
    (MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE +                   \
     MEMSAFE_DETAIL_BACKEND_GCC_ACTIVE +                     \
     MEMSAFE_DETAIL_BACKEND_MSVC_ACTIVE +                    \
     MEMSAFE_DETAIL_BACKEND_RUSTY_CPP_ACTIVE +               \
     MEMSAFE_DETAIL_BACKEND_CHERI_ACTIVE)

#if MEMSAFE_DETAIL_BACKEND_ACTIVE_COUNT > 1
#  error "Only one MEMSAFE_BACKEND_* macro may be defined"
#endif

/**
 * @def MEMSAFE_NODISCARD
 * @brief Mark a type or function result as non-discardable.
 *
 * @details
 * Expands unconditionally to the standard C++17 `[[nodiscard]]` attribute. The
 * PRD makes discardability a core API contract rather than an optional backend
 * diagnostic, so this macro deliberately avoids vendor-specific spellings that
 * can trigger `-Wattributes` warnings on non-vendor compilers.
 *
 * @return No runtime value; expands to a standard attribute specifier.
 * @pre Apply only where the C++ standard permits `[[nodiscard]]`, such as a
 * class declaration, enum declaration, function declaration, or constructor.
 * @post A conforming compiler may diagnose discarded values or function
 * results annotated with this macro.
 * @invariant The macro is never backend-gated and never expands to a vendor
 * attribute.
 * @throws Nothing; invalid placement is a compile-time syntax error reported by
 * the compiler.
 * @note Ownership/thread-safety: this macro creates no runtime ownership and no
 * synchronization obligations.
 *
 * Example:
 * @code
 * struct MEMSAFE_NODISCARD RefToken { int value; };
 * MEMSAFE_NODISCARD RefToken make_ref_token();
 * @endcode
 */
#define MEMSAFE_NODISCARD [[nodiscard]]

/*
 * PRD Slice 0 limits emitted lifetime annotations to the Clang backend. The
 * `defined(__clang__)` guard is intentionally separate from the selected
 * backend so a manual `MEMSAFE_BACKEND_CLANG` definition on GCC or MSVC remains
 * a no-op instead of emitting unknown Clang-scoped attributes.
 */
#if defined(MEMSAFE_BACKEND_CLANG) && defined(__clang__)
#  if defined(__has_cpp_attribute)
#    if __has_cpp_attribute(clang::lifetimebound)
#      define MEMSAFE_LIFETIMEBOUND [[clang::lifetimebound]]
#    else
#      define MEMSAFE_LIFETIMEBOUND
#    endif
#    if __has_cpp_attribute(clang::coro_lifetimebound)
#      define MEMSAFE_DETAIL_CORO_LIFETIMEBOUND [[clang::coro_lifetimebound]]
#    else
#      define MEMSAFE_DETAIL_CORO_LIFETIMEBOUND
#    endif
#  else
#    define MEMSAFE_LIFETIMEBOUND
#    define MEMSAFE_DETAIL_CORO_LIFETIMEBOUND
#  endif
#else
#  define MEMSAFE_LIFETIMEBOUND
#  define MEMSAFE_DETAIL_CORO_LIFETIMEBOUND
#endif

/**
 * @def MEMSAFE_LIFETIMEBOUND
 * @brief Bind a returned borrow's lifetime to an annotated Clang parameter.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, expands to
 * `[[clang::lifetimebound]]` when `__has_cpp_attribute` reports support for
 * that spelling. Under GCC, MSVC, unknown compilers, no-backend builds, or
 * Clang versions that do not expose the attribute, it expands to nothing.
 *
 * @return No runtime value; expands to zero or one compile-time attribute.
 * @pre Apply only at declaration positions accepted by the active compiler,
 * typically to a parameter that controls a returned borrow's lifetime.
 * @post Supporting Clang builds may diagnose borrows that escape the annotated
 * lifetime source.
 * @invariant Unsupported compilers and unsupported Clang versions see an empty
 * expansion, preventing unknown-attribute diagnostics.
 * @throws Nothing; invalid placement is a compile-time syntax error reported by
 * the compiler.
 * @note Ownership/thread-safety: this macro does not create or extend runtime
 * ownership. It only supplies static-analysis metadata.
 *
 * Example:
 * @code
 * const int& identity(const int& value MEMSAFE_LIFETIMEBOUND);
 * @endcode
 */

/**
 * @def MEMSAFE_DETAIL_CORO_LIFETIMEBOUND
 * @brief Internal coroutine-return-type lifetime companion attribute for Clang
 * backend lanes.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, expands to
 * `[[clang::coro_lifetimebound]]` when `__has_cpp_attribute` reports support
 * for that spelling. Under every other backend, compiler, or unsupported Clang
 * version, it expands to nothing. It is kept as an internal helper so
 * `MEMSAFE_BORROWS(x)` can compose the PRD Slice 0 lifetimebound and
 * coro_lifetimebound hooks while each vendor spelling remains independently
 * feature-probed.
 *
 * @return No runtime value; expands to zero or one compile-time attribute.
 * @pre Internal use by `MEMSAFE_BORROWS(x)` and future backend-specific
 * declarations only. Do not use this helper directly in public declarations.
 * @post Supporting Clang builds may include coroutine suspension points in
 * lifetime diagnostics for borrow-returning APIs.
 * @invariant Unsupported compilers and unsupported Clang versions see an empty
 * expansion, preventing unknown-attribute diagnostics.
 * @throws Nothing; invalid placement is a compile-time syntax error reported by
 * the compiler.
 * @note Ownership/thread-safety: this macro does not create or extend runtime
 * ownership. It only supplies static-analysis metadata.
 */

/**
 * @def MEMSAFE_BORROWS
 * @brief Annotate a declaration position that borrows from a named source.
 *
 * @param x Source expression or parameter name whose lifetime conceptually
 * bounds the returned borrow. The parameter is intentionally not evaluated and
 * not substituted into Clang's current attribute spellings; it documents the
 * contract at the call site and keeps the macro ready for richer future
 * backends.
 * @return No runtime value; expands to zero, one, or two compile-time
 * attributes depending on the selected backend and Clang attribute support.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, this macro expands
 * to `MEMSAFE_LIFETIMEBOUND MEMSAFE_DETAIL_CORO_LIFETIMEBOUND`, which maps to
 * `[[clang::lifetimebound]] [[clang::coro_lifetimebound]]` when both spellings
 * are supported by the compiler. Under GCC, MSVC, unknown compilers, and
 * no-backend builds, both helpers are empty and the macro produces no
 * diagnostics.
 *
 * @pre Apply at a declaration position selected by the backend integration for
 * the active compiler and accepted by that compiler's attribute rules.
 * @post Supporting Clang builds may diagnose dangling borrows for return values
 * tied to the annotated source, including the coroutine lifetime hook required
 * by the Slice 0 public macro contract.
 * @invariant The macro evaluates @p x zero times, emits no non-Clang
 * attributes, and degrades to an empty expansion outside real Clang-backend
 * builds.
 * @throws Nothing; invalid placement is a compile-time syntax error reported by
 * the compiler.
 * @note Ownership/thread-safety: this macro creates no runtime borrow tracking,
 * owns no storage, and has no synchronization behavior.
 *
 * Example:
 * @code
 * #if MEMSAFE_DETAIL_BACKEND_CLANG_ACTIVE
 * #  define MEMSAFE_EXAMPLE_BORROW_ANNOTATION MEMSAFE_BORROWS(owner)
 * #endif
 * @endcode
 */
/*
 * PRD Slice 0 line 82 and the CPP_MEMSAFE-0020-FUNC symbol contract require
 * the public `MEMSAFE_BORROWS(x)` hook itself to expose both Clang lifetime
 * attributes. The helper macros above keep each spelling feature-probed while
 * this macro remains the single declared user-facing annotation hook.
 */
#define MEMSAFE_BORROWS(x) MEMSAFE_LIFETIMEBOUND MEMSAFE_DETAIL_CORO_LIFETIMEBOUND

/*
 * Slice 7 requires optional vendor backend annotations without perturbing the
 * portable default build. Each public annotation below therefore has two gates:
 * first the selected backend switch, then the compiler's feature probe for the
 * concrete attribute spelling. The Clang owner/pointer branch also probes the
 * project-requested `clang::owner` / `clang::pointer` spellings before falling
 * back to Clang's documented GSL owner/pointer attributes. The CHERI branches
 * probe CHERI-named attributes first, then use Clang's generic declaration
 * annotation hook as a source-level vendor marker on CHERI+Clang lanes. This
 * is deliberate: Slice 7 acceptance requires an enabled CHERI backend to carry
 * annotation metadata when the toolchain exposes a safe Clang annotation hook,
 * while unsupported compilers still receive no unknown attributes.
 */
#if defined(MEMSAFE_BACKEND_CLANG) && defined(__clang__) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(clang::owner)
#    define MEMSAFE_OWNED [[clang::owner]]
#  elif __has_cpp_attribute(gsl::Owner)
#    define MEMSAFE_OWNED [[gsl::Owner]]
#  else
#    define MEMSAFE_OWNED
#  endif
#elif defined(MEMSAFE_BACKEND_RUSTY_CPP) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(rusty::owned)
#    define MEMSAFE_OWNED [[rusty::owned]]
#  else
#    define MEMSAFE_OWNED
#  endif
#elif defined(MEMSAFE_BACKEND_CHERI) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(cheri::owner)
#    define MEMSAFE_OWNED [[cheri::owner]]
#  elif defined(__clang__) && __has_cpp_attribute(clang::annotate)
#    define MEMSAFE_OWNED [[clang::annotate("memsafe.cheri.owner")]]
#  else
#    define MEMSAFE_OWNED
#  endif
#else
#  define MEMSAFE_OWNED
#endif

/**
 * @def MEMSAFE_OWNED
 * @brief Annotate an owning wrapper type for the selected backend.
 *
 * @return No runtime value; expands to zero or one compile-time attribute.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, expands to
 * `[[clang::owner]]` if that spelling is exposed, otherwise to the documented
 * `[[gsl::Owner]]` spelling when available. Under
 * `MEMSAFE_BACKEND_RUSTY_CPP`, expands to `[[rusty::owned]]` when the compiler
 * exposes that vendor attribute. Under `MEMSAFE_BACKEND_CHERI`, expands to
 * `[[cheri::owner]]` when available, or to
 * `[[clang::annotate("memsafe.cheri.owner")]]` on Clang toolchains that expose
 * generic declaration annotations. Under GCC, MSVC, unknown compilers,
 * unselected backends, or unsupported spellings, it expands to nothing.
 *
 * @pre Apply only where the active compiler accepts a class or struct
 * declaration attribute, typically on an owning wrapper such as `Owner<T>` or a
 * capability-owning storage node.
 * @post Supporting vendor backends can classify the annotated declaration as an
 * owner for static analysis or CHERI-aware metadata extraction.
 * @invariant The macro never emits an unprobed attribute spelling and never
 * evaluates a runtime expression.
 * @throws Nothing directly; invalid placement is a compile-time syntax error
 * reported by the compiler.
 * @note Ownership/thread-safety: the macro creates no runtime ownership,
 * lifetime extension, lock acquisition, or synchronization. It only describes
 * ownership intent to optional analysis backends.
 *
 * Example:
 * @code
 * template <class T>
 * class MEMSAFE_OWNED Owner;
 * @endcode
 */

#if defined(MEMSAFE_BACKEND_CLANG) && defined(__clang__) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(clang::pointer)
#    define MEMSAFE_POINTER [[clang::pointer]]
#  elif __has_cpp_attribute(gsl::Pointer)
#    define MEMSAFE_POINTER [[gsl::Pointer]]
#  else
#    define MEMSAFE_POINTER
#  endif
#elif defined(MEMSAFE_BACKEND_RUSTY_CPP) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(rusty::borrowed)
#    define MEMSAFE_POINTER [[rusty::borrowed]]
#  else
#    define MEMSAFE_POINTER
#  endif
#elif defined(MEMSAFE_BACKEND_CHERI) && defined(__has_cpp_attribute)
#  if __has_cpp_attribute(cheri::pointer)
#    define MEMSAFE_POINTER [[cheri::pointer]]
#  elif defined(__clang__) && __has_cpp_attribute(clang::annotate)
#    define MEMSAFE_POINTER [[clang::annotate("memsafe.cheri.pointer")]]
#  else
#    define MEMSAFE_POINTER
#  endif
#else
#  define MEMSAFE_POINTER
#endif

/**
 * @def MEMSAFE_POINTER
 * @brief Annotate a non-owning pointer, view, or borrow wrapper for the
 * selected backend.
 *
 * @return No runtime value; expands to zero or one compile-time attribute.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, expands to
 * `[[clang::pointer]]` if that spelling is exposed, otherwise to the documented
 * `[[gsl::Pointer]]` spelling when available. Under
 * `MEMSAFE_BACKEND_RUSTY_CPP`, expands to `[[rusty::borrowed]]` when the
 * compiler exposes that vendor attribute. Under `MEMSAFE_BACKEND_CHERI`,
 * expands to `[[cheri::pointer]]` when available, or to
 * `[[clang::annotate("memsafe.cheri.pointer")]]` on Clang toolchains that
 * expose generic declaration annotations. Under GCC, MSVC, unknown compilers,
 * unselected backends, or unsupported spellings, it expands to nothing.
 *
 * @pre Apply only where the active compiler accepts a class or struct
 * declaration attribute, typically on a non-owning wrapper such as `Ref<T>`,
 * `MutRef<T>`, or another pointer-like facade.
 * @post Supporting vendor backends can classify the annotated declaration as a
 * non-owning view into an owner and may diagnose uses that outlive that owner.
 * @invariant The macro never emits an unprobed attribute spelling and never
 * evaluates a runtime expression.
 * @throws Nothing directly; invalid placement is a compile-time syntax error
 * reported by the compiler.
 * @note Ownership/thread-safety: the macro takes no ownership, extends no
 * lifetime, and creates no synchronization. It only describes pointer or borrow
 * intent to optional analysis backends.
 *
 * Example:
 * @code
 * template <class T>
 * class MEMSAFE_POINTER Ref;
 * @endcode
 */

#if defined(MEMSAFE_BACKEND_CLANG) && defined(__clang__) && defined(__has_attribute)
#  if __has_attribute(requires_capability)
#    define MEMSAFE_LOCK_HELD(m) __attribute__((requires_capability(m)))
#  else
#    define MEMSAFE_LOCK_HELD(m)
#  endif
#elif defined(MEMSAFE_BACKEND_RUSTY_CPP) && defined(__has_attribute)
#  if __has_attribute(rusty_lock_held)
#    define MEMSAFE_LOCK_HELD(m) __attribute__((rusty_lock_held(m)))
#  else
#    define MEMSAFE_LOCK_HELD(m)
#  endif
#elif defined(MEMSAFE_BACKEND_CHERI) && defined(__has_attribute)
#  if __has_attribute(cheri_requires_capability)
#    define MEMSAFE_LOCK_HELD(m) __attribute__((cheri_requires_capability(m)))
#  elif defined(__clang__) && __has_attribute(requires_capability)
#    define MEMSAFE_LOCK_HELD(m) __attribute__((requires_capability(m)))
#  elif defined(__clang__) && __has_attribute(annotate)
#    define MEMSAFE_LOCK_HELD(m) __attribute__((annotate("memsafe.cheri.lock_held")))
#  else
#    define MEMSAFE_LOCK_HELD(m)
#  endif
#else
#  define MEMSAFE_LOCK_HELD(m)
#endif

/**
 * @def MEMSAFE_LOCK_HELD
 * @brief Annotate a function whose caller must hold a lock or capability.
 *
 * @param m Lock, mutex, guard, or capability expression required by the
 * annotated function. Attribute branches that accept an expression forward
 * @p m verbatim; generic annotation fallbacks keep @p m as documentation-level
 * contract text and store only a stable marker string.
 * @return No runtime value; expands to zero or one compile-time attribute.
 *
 * @details
 * Under `MEMSAFE_BACKEND_CLANG` on an actual Clang compiler, expands to
 * `__attribute__((requires_capability(m)))` when Clang exposes that thread
 * safety spelling. Under `MEMSAFE_BACKEND_RUSTY_CPP`, expands to
 * `__attribute__((rusty_lock_held(m)))` when available. Under
 * `MEMSAFE_BACKEND_CHERI`, expands first to a probed
 * `cheri_requires_capability` spelling, then to Clang's
 * `requires_capability(m)` when available, and finally to
 * `__attribute__((annotate("memsafe.cheri.lock_held")))` when a Clang
 * declaration annotation hook is the only available safe metadata path. Under
 * GCC, MSVC, unknown compilers, unselected backends, or unsupported spellings,
 * it expands to nothing.
 *
 * @pre Apply only to a function declaration whose caller is required to hold
 * @p m on entry and where the active compiler accepts the emitted attribute
 * syntax.
 * @post Supporting vendor backends may diagnose calls that violate the lock or
 * capability precondition, or may preserve CHERI/Rusty-C++ lock-intent metadata
 * for external analysis.
 * @invariant The macro never emits an unprobed attribute spelling and evaluates
 * @p m zero times at runtime.
 * @throws Nothing directly; invalid placement is a compile-time syntax error
 * reported by the compiler.
 * @note Ownership/thread-safety: the macro acquires no lock and transfers no
 * ownership. It only describes a caller-side synchronization or capability
 * precondition to optional backends.
 *
 * Example:
 * @code
 * int value() const MEMSAFE_LOCK_HELD(mutex_);
 * @endcode
 */

#endif /* MEMSAFE_BACKEND_HPP */
