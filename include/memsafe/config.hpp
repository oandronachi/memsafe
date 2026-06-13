/**
 * @file config.hpp
 * @brief Compile-time configuration, feature selection, and C++17 SFINAE helpers for memsafe.
 *
 * @details
 * Work package: CPP_MEMSAFE-0800-FUNC, extending the configuration plumbing
 * introduced by CPP_MEMSAFE-0030-FUNC.
 *
 * Purpose:
 * - Provide the shared configuration switches that must be visible before any
 *   public memsafe feature header is included through `<memsafe/memsafe.hpp>`.
 * - Centralize the language-version, concept-availability, and source-location
 *   feature probes used by later feature headers.
 * - Expose a C++17-compatible SFINAE support layer under `memsafe::detail` so
 *   feature headers can constrain template participation without relying on
 *   C++20-only syntax when `MEMSAFE_CXX17_COMPAT` is nonzero.
 *
 * Key invariants:
 * - The library remains header-only: this header declares no storage, emits no
 *   out-of-line definitions, and requires no link step.
 * - `MEMSAFE_CXX17_COMPAT` defaults to `0` unless a translation unit defines it
 *   before including a memsafe header.
 * - `MEMSAFE_HAS_CONCEPTS` is a derived output of this header, not a user
 *   override point. It is always `0` when `MEMSAFE_CXX17_COMPAT` is nonzero,
 *   even if a build system predefines `MEMSAFE_HAS_CONCEPTS`.
 * - `MEMSAFE_HAS_SOURCE_LOCATION` is always `0` when `MEMSAFE_CXX17_COMPAT` is
 *   nonzero, so compatibility-mode diagnostics use the explicit
 *   `__FILE__`/`__LINE__`/`__func__` path supplied by `violation.hpp`.
 * - The SFINAE helper templates depend only on C++17 standard-library
 *   facilities and are safe to include in every feature header in the full type
 *   roster: `Owner`, `Ref`, `MutRef`, `Slot`, `Handle`, `SlotMap`, `Scope`,
 *   `SyncOwner`, `SyncRef`, `SyncMutRef`, `Arc`, and `Mutex`.
 *
 * Ownership and thread-safety:
 * - This header owns no resources, performs no allocation, and has no runtime
 *   side effects. Including it is thread-safe by construction.
 *
 * @note The defaults mirror the PRD configuration macro requirements and the
 * AADL `Memory_Safe_Library` model: C++20 is the baseline, C++17 is available
 * through an explicit compatibility switch, and feature headers use a common
 * selector instead of locally re-probing language facilities.
 */
#ifndef MEMSAFE_CONFIG_HPP
#define MEMSAFE_CONFIG_HPP

#include <type_traits>

#ifndef MEMSAFE_LANG_VERSION
/**
 * @def MEMSAFE_LANG_VERSION
 * @brief Report the active C++ language mode as a preprocessor integer.
 *
 * @details
 * MSVC historically leaves `__cplusplus` stale unless a compiler option is
 * enabled, while `_MSVC_LANG` reports the selected language mode. This macro
 * therefore uses `_MSVC_LANG` when present and falls back to `__cplusplus`
 * everywhere else.
 *
 * @retval 201703L A C++17 language mode is active.
 * @retval 202002L A C++20 language mode is active.
 * @pre Include this header before testing the macro. User code should not
 * define this macro unless it is deliberately adapting an unusual toolchain.
 * @post The macro is available for preprocessor checks and `static_assert`
 * expressions.
 * @invariant The value reflects compiler language mode only; it does not prove
 * that every standard-library header for that mode is implemented.
 * @throws Nothing; the macro expands to an integer literal supplied by the
 * compiler.
 * @note Ownership/thread-safety: this macro owns no storage and creates no
 * synchronization obligations.
 *
 * Example:
 * @code
 * #include <memsafe/config.hpp>
 * static_assert(MEMSAFE_LANG_VERSION >= 201703L);
 * @endcode
 */
#  if defined(_MSVC_LANG)
#    define MEMSAFE_LANG_VERSION _MSVC_LANG
#  else
#    define MEMSAFE_LANG_VERSION __cplusplus
#  endif
#endif

#ifndef MEMSAFE_CXX17_COMPAT
/**
 * @def MEMSAFE_CXX17_COMPAT
 * @brief Select the C++17-compatible implementation surface for memsafe.
 *
 * @details
 * A translation unit may define this macro to a nonzero value before including
 * any memsafe header to request the compatibility lane. In that lane, feature
 * headers must avoid concept syntax and C++20-only diagnostic facilities such
 * as `std::source_location`; the Slice 8 helpers below provide the SFINAE
 * vocabulary used by C++17-compatible declarations.
 *
 * @retval 0 The default baseline mode; C++20 facilities may be used when
 * feature-test macros report support.
 * @retval 1 Compatibility mode requested by the user or by the F4 cxx17 lane.
 * @pre Define this macro before including any memsafe header when compatibility
 * mode is required.
 * @post `MEMSAFE_HAS_CONCEPTS` and `MEMSAFE_HAS_SOURCE_LOCATION` are forced to
 * `0` when this macro is nonzero.
 * @invariant The default remains `0` so the C++20 baseline path is selected
 * unless the build lane explicitly opts into C++17 compatibility.
 * @throws Nothing; this macro is compile-time configuration only.
 * @note Ownership/thread-safety: the macro owns no storage and has no runtime
 * synchronization behavior.
 *
 * Example:
 * @code
 * #define MEMSAFE_CXX17_COMPAT 1
 * #include <memsafe/memsafe.hpp>
 * static_assert(MEMSAFE_HAS_CONCEPTS == 0);
 * @endcode
 */
#  define MEMSAFE_CXX17_COMPAT 0
#endif

/**
 * @def MEMSAFE_HAS_CONCEPTS
 * @brief Report whether memsafe feature headers may use C++20 concept syntax.
 *
 * @details
 * The macro is derived from the user-visible compatibility switch and the
 * standard `__cpp_concepts` feature-test macro. It is intentionally
 * non-overridable because the PRD requires `MEMSAFE_CXX17_COMPAT` to build a
 * C++17 subset that removes concept syntax. If a build system predefines
 * `MEMSAFE_HAS_CONCEPTS`, this header discards that value and recomputes the
 * selector so compatibility mode remains authoritative.
 *
 * @retval 1 `MEMSAFE_CXX17_COMPAT` is zero and the compiler reports
 * `__cpp_concepts >= 201907L`.
 * @retval 0 Concept syntax is unavailable or has been disabled by the
 * compatibility switch.
 * @pre Include this header before using the macro in public feature headers.
 * Do not define this macro directly; define `MEMSAFE_CXX17_COMPAT` to select
 * the compatibility path.
 * @post Downstream headers can select C++20 declarations or SFINAE declarations
 * without re-probing compiler-specific language switches.
 * @invariant Compatibility mode always wins over compiler support or command
 * line predefinitions; a C++20 compiler still reports `0` here when
 * `MEMSAFE_CXX17_COMPAT` is nonzero.
 * @throws Nothing; this macro expands to an integer literal.
 * @note Ownership/thread-safety: this is compile-time plumbing only.
 *
 * Example:
 * @code
 * #if MEMSAFE_HAS_CONCEPTS
 * // C++20-only constrained declarations may be selected here.
 * #endif
 * @endcode
 */
#ifdef MEMSAFE_HAS_CONCEPTS
/*
 * PRD line 68 requires MEMSAFE_CXX17_COMPAT to remove concept syntax. Treating
 * MEMSAFE_HAS_CONCEPTS as user-overridable would let a build command re-enable
 * the C++20 path in the C++17 lane, so this public selector is recomputed every
 * time config.hpp is included.
 */
#  undef MEMSAFE_HAS_CONCEPTS
#endif

#if !MEMSAFE_CXX17_COMPAT && defined(__cpp_concepts) && __cpp_concepts >= 201907L
#  define MEMSAFE_HAS_CONCEPTS 1
#else
#  define MEMSAFE_HAS_CONCEPTS 0
#endif

/**
 * @def MEMSAFE_HAS_SOURCE_LOCATION
 * @brief Report whether memsafe diagnostics may name `std::source_location`.
 *
 * @details
 * This selector is derived from the language mode, the header probe, and
 * `MEMSAFE_CXX17_COMPAT`. It intentionally does not include `<source_location>`
 * itself; `violation.hpp` owns the actual diagnostic type include on the
 * C++20-capable path and falls back to explicit source fields when this macro
 * is zero.
 *
 * @retval 1 Compatibility mode is off, the active language mode is at least
 * C++20, and the standard-library header probe reports `<source_location>`.
 * @retval 0 Compatibility mode is on, the language mode is below C++20, or the
 * standard-library header probe is unavailable or negative.
 * @pre Include this header before selecting diagnostic capture paths.
 * @post Downstream headers can keep `std::source_location` out of the C++17
 * compatibility surface.
 * @invariant `MEMSAFE_CXX17_COMPAT != 0` always forces this macro to `0`, even
 * on a compiler whose standard library provides `<source_location>`.
 * @throws Nothing; this macro expands to an integer literal.
 * @note Ownership/thread-safety: the macro owns no storage and performs no
 * runtime synchronization.
 *
 * Example:
 * @code
 * #define MEMSAFE_CXX17_COMPAT 1
 * #include <memsafe/config.hpp>
 * static_assert(MEMSAFE_HAS_SOURCE_LOCATION == 0);
 * @endcode
 */
#ifdef MEMSAFE_HAS_SOURCE_LOCATION
/*
 * Slice 8 makes the compatibility switch authoritative. Recomputing this macro
 * prevents a build system from making `std::source_location` reachable after a
 * translation unit opted into the C++17 lane.
 */
#  undef MEMSAFE_HAS_SOURCE_LOCATION
#endif

#if !MEMSAFE_CXX17_COMPAT && MEMSAFE_LANG_VERSION >= 202002L && defined(__has_include)
#  if __has_include(<source_location>)
#    define MEMSAFE_HAS_SOURCE_LOCATION 1
#  else
#    define MEMSAFE_HAS_SOURCE_LOCATION 0
#  endif
#else
#  define MEMSAFE_HAS_SOURCE_LOCATION 0
#endif

namespace memsafe {
namespace detail {

/**
 * @brief C++17-compatible alias for `std::integral_constant<bool, Value>`.
 *
 * @tparam Value Boolean value represented by the resulting type.
 * @return Type alias whose `value` member equals `Value`.
 * @pre `Value` is a compile-time boolean expression.
 * @post The alias can participate in tag dispatch, inheritance, and
 * `static_assert` checks.
 * @invariant The alias owns no storage and maps exactly to the standard
 * `std::integral_constant` representation available in C++17.
 * @throws Nothing; this is a type alias.
 * @note Ownership/thread-safety: aliases create no objects and introduce no
 * synchronization.
 *
 * Example:
 * @code
 * static_assert(memsafe::detail::compat_bool_constant<true>::value);
 * @endcode
 */
template <bool Value>
using compat_bool_constant = std::integral_constant<bool, Value>;

/**
 * @brief C++17-compatible `void_t` alias used by SFINAE probes.
 *
 * @tparam Ts Types or unevaluated expressions wrapped as types by the caller.
 * @return `void` when every template argument is well-formed.
 * @pre Use this alias only in substitution contexts where failure should remove
 * a candidate rather than produce a hard diagnostic.
 * @post Ill-formed probe arguments cause the enclosing partial specialization
 * or overload to be discarded by SFINAE.
 * @invariant The alias does not evaluate expressions or instantiate objects; it
 * only forms a type during substitution.
 * @throws Nothing; this is a type alias.
 * @note Ownership/thread-safety: aliases own no storage and are safe in every
 * translation unit.
 *
 * Example:
 * @code
 * template <typename T, typename = memsafe::detail::void_t<>>
 * struct probe : std::false_type {};
 * @endcode
 */
template <typename... Ts>
using void_t = void;

/**
 * @brief C++17-compatible `enable_if` alias for constrained declarations.
 *
 * @tparam Condition Boolean expression controlling participation.
 * @tparam T Type produced when `Condition` is true.
 * @return `T` when `Condition` is true; no type when `Condition` is false.
 * @pre Use in a SFINAE context such as a defaulted template parameter or return
 * type.
 * @post A false condition removes the associated declaration from overload
 * resolution instead of requiring C++20 constraint syntax.
 * @invariant The alias is exactly `std::enable_if<Condition, T>::type`, keeping
 * behavior portable across C++17, C++20, and later modes.
 * @throws Nothing; this is a type alias.
 * @note Ownership/thread-safety: aliases own no runtime state.
 *
 * Example:
 * @code
 * template <typename T,
 *           memsafe::detail::enable_if_t<std::is_integral<T>::value, int> = 0>
 * T twice(T value) { return value + value; }
 * @endcode
 */
template <bool Condition, typename T = void>
using enable_if_t = typename std::enable_if<Condition, T>::type;

/**
 * @brief C++17-compatible backport of the C++20 `remove_cvref_t` utility.
 *
 * @tparam T Type whose reference and cv-qualifiers should be removed.
 * @return `T` stripped of reference, const, and volatile qualifiers.
 * @pre `T` may be any type accepted by `std::remove_reference` and
 * `std::remove_cv`.
 * @post The resulting type can be used for payload-type normalization without
 * depending on C++20 standard-library aliases.
 * @invariant The transformation is exactly
 * `remove_cv<remove_reference<T>::type>::type`.
 * @throws Nothing; this is a type alias.
 * @note Ownership/thread-safety: aliases own no storage and perform no runtime
 * work.
 *
 * Example:
 * @code
 * static_assert(std::is_same<
 *     memsafe::detail::remove_cvref_t<const int&>, int>::value);
 * @endcode
 */
template <typename T>
using remove_cvref_t =
    typename std::remove_cv<typename std::remove_reference<T>::type>::type;

/**
 * @brief Primary completeness probe that is false until `sizeof(T)` is valid.
 *
 * @tparam T Type being tested for completeness.
 * @tparam Enable Private SFINAE parameter; callers must leave it as the
 * default.
 * @pre `T` may be incomplete, `void`, a function type, an array type, or a
 * complete object type.
 * @post `is_complete<T>::value` is false when `sizeof(T)` is not well-formed.
 * @invariant The probe never odr-uses a value of type `T`; it only attempts an
 * unevaluated `sizeof(T)` inside a substitution context.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 *
 * Example:
 * @code
 * struct forward_declared;
 * static_assert(!memsafe::detail::is_complete<forward_declared>::value);
 * @endcode
 */
template <typename T, typename Enable = void>
struct is_complete : std::false_type {};

/**
 * @brief Completeness probe specialization selected when `sizeof(T)` is valid.
 *
 * @tparam T Complete type for which `sizeof(T)` can be formed.
 * @pre `T` is complete enough for `sizeof(T)` in an unevaluated operand.
 * @post `is_complete<T>::value` is true.
 * @invariant The specialization records only compile-time metadata and does not
 * allocate or inspect any object instance.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 */
template <typename T>
struct is_complete<T, void_t<decltype(sizeof(T))>> : std::true_type {};

/**
 * @brief Trait for complete non-array object payload types accepted by v1 APIs.
 *
 * @tparam T Type being tested as a memsafe payload.
 * @pre `T` may be any type; incomplete and non-object types are reported as
 * false through SFINAE rather than hard diagnostics.
 * @post `value` is true only for complete object types that are not arrays.
 * @invariant This predicate captures the common object roster rule shared by
 * `Owner`, `Slot`, `SlotMap`, `SyncOwner`, `Arc`, and `Mutex`.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no storage and introduce no
 * synchronization.
 *
 * Example:
 * @code
 * static_assert(memsafe::detail::is_complete_object<int>::value);
 * static_assert(!memsafe::detail::is_complete_object<int[2]>::value);
 * @endcode
 */
template <typename T>
struct is_complete_object
    : compat_bool_constant<is_complete<T>::value &&
                           std::is_object<T>::value &&
                           !std::is_array<T>::value> {};

/**
 * @brief Private false branch for constructible-object SFINAE checks.
 *
 * @tparam T Candidate payload type.
 * @tparam Enable Private SFINAE parameter selected by `sizeof(T)`.
 * @tparam Args Constructor argument types forwarded to `T`.
 * @pre Callers use `is_constructible_object`, not this implementation detail.
 * @post The branch reports false when `T` is incomplete or cannot be sized.
 * @invariant Incomplete-type rejection happens before `std::is_constructible`
 * is instantiated, avoiding undefined library-trait use in the C++17 lane.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 */
template <typename T, typename Enable, typename... Args>
struct is_constructible_object_impl : std::false_type {};

/**
 * @brief Private true-candidate branch for constructible-object checks.
 *
 * @tparam T Complete candidate payload type.
 * @tparam Args Constructor argument types forwarded to `T`.
 * @pre `sizeof(T)` is well-formed.
 * @post The branch reports true only when `T` is a non-array object type and is
 * constructible from `Args...`.
 * @invariant The object-type rule and constructor-availability rule are kept in
 * one trait so C++17 constructor declarations can use one SFINAE predicate.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 */
template <typename T, typename... Args>
struct is_constructible_object_impl<T, void_t<decltype(sizeof(T))>, Args...>
    : compat_bool_constant<std::is_object<T>::value &&
                           !std::is_array<T>::value &&
                           std::is_constructible<T, Args...>::value> {};

/**
 * @brief Trait for complete object payloads constructible from given arguments.
 *
 * @tparam T Candidate payload type.
 * @tparam Args Constructor argument types, normally already reference-qualified
 * as they will be forwarded at the call site.
 * @pre `T` may be incomplete or ill-formed for construction; failures are
 * represented as `value == false`.
 * @post `value` is true only for complete non-array object types constructible
 * from `Args...`.
 * @invariant This is the common SFINAE predicate for constructor templates in
 * the owner, sync owner, shared owner, mutex, and slot-map roster.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 *
 * Example:
 * @code
 * static_assert(memsafe::detail::is_constructible_object<int, int>::value);
 * static_assert(!memsafe::detail::is_constructible_object<int, int*>::value);
 * @endcode
 */
template <typename T, typename... Args>
struct is_constructible_object
    : is_constructible_object_impl<T, void, Args...> {};

/**
 * @brief Private false branch for scope-owned object SFINAE checks.
 *
 * @tparam T Candidate scope-created type.
 * @tparam Enable Private SFINAE parameter selected by `sizeof(T)`.
 * @tparam Args Constructor argument types forwarded to `T`.
 * @pre Callers use `is_scope_constructible_object`, not this implementation
 * detail.
 * @post The branch reports false when `T` is incomplete or cannot be sized.
 * @invariant `std::is_nothrow_destructible` is not instantiated for incomplete
 * types, preserving C++17 standard-library trait preconditions.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 */
template <typename T, typename Enable, typename... Args>
struct is_scope_constructible_object_impl : std::false_type {};

/**
 * @brief Private true-candidate branch for scope-owned object checks.
 *
 * @tparam T Complete candidate scope-created type.
 * @tparam Args Constructor argument types forwarded to `T`.
 * @pre `sizeof(T)` is well-formed.
 * @post The branch reports true only when `T` is a non-array object type,
 * constructible from `Args...`, and nothrow destructible.
 * @invariant The nothrow-destructor rule mirrors the Slice 3 `Scope` cleanup
 * invariant that destruction during scope exit must not throw.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 */
template <typename T, typename... Args>
struct is_scope_constructible_object_impl<T,
                                          void_t<decltype(sizeof(T))>,
                                          Args...>
    : compat_bool_constant<std::is_object<T>::value &&
                           !std::is_array<T>::value &&
                           std::is_constructible<T, Args...>::value &&
                           std::is_nothrow_destructible<T>::value> {};

/**
 * @brief Trait for `Scope::create<T>`-style payload construction.
 *
 * @tparam T Candidate scope-created type.
 * @tparam Args Constructor argument types, normally already reference-qualified
 * as they will be forwarded at the call site.
 * @pre `T` may be incomplete or ill-formed for construction; failures are
 * represented as `value == false`.
 * @post `value` is true only for complete non-array object types constructible
 * from `Args...` with a non-throwing destructor.
 * @invariant This predicate captures the Slice 3 rule that scope teardown is
 * `noexcept` because accepted payload destructors are non-throwing.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 *
 * Example:
 * @code
 * static_assert(
 *     memsafe::detail::is_scope_constructible_object<int, int>::value);
 * @endcode
 */
template <typename T, typename... Args>
struct is_scope_constructible_object
    : is_scope_constructible_object_impl<T, void, Args...> {};

/**
 * @brief Dependent false value for static assertions in template fallbacks.
 *
 * @tparam T Type that makes the false value dependent on a template parameter.
 * @pre Use only when a diagnostic should be delayed until a template is
 * instantiated.
 * @post `value` is always false, but dependent on `T`.
 * @invariant The helper has no runtime representation and is safe in all
 * language modes supported by this library.
 * @throws Nothing; this is a compile-time trait.
 * @note Ownership/thread-safety: traits own no runtime storage.
 *
 * Example:
 * @code
 * template <typename T>
 * void unsupported() {
 *     static_assert(memsafe::detail::dependent_false<T>::value,
 *                   "unsupported memsafe payload");
 * }
 * @endcode
 */
template <typename T>
struct dependent_false : std::false_type {};

} // namespace detail
} // namespace memsafe

/**
 * @def MEMSAFE_DETAIL_ENABLE_IF
 * @brief Produce a defaulted non-type template parameter for SFINAE.
 *
 * @param CONDITION Boolean expression controlling whether a declaration
 * participates in overload resolution.
 * @return A defaulted unnamed `int` template parameter when `CONDITION` is
 * true; substitution failure when `CONDITION` is false.
 * @pre Use inside a template parameter list after any template parameters used
 * by `CONDITION` have been declared.
 * @post A false condition removes the surrounding declaration from overload
 * resolution in C++17 mode.
 * @invariant The macro expands only to C++17-valid SFINAE syntax; it never
 * expands to C++20 constraint syntax.
 * @throws Nothing directly; it is compile-time declaration plumbing.
 * @note Ownership/thread-safety: the macro declares no runtime object and has
 * no synchronization behavior.
 *
 * Example:
 * @code
 * template <typename T,
 *           MEMSAFE_DETAIL_ENABLE_IF(std::is_integral<T>::value)>
 * T identity(T value) { return value; }
 * @endcode
 */
#define MEMSAFE_DETAIL_ENABLE_IF(CONDITION) \
    ::memsafe::detail::enable_if_t<(CONDITION), int> = 0

/**
 * @def MEMSAFE_DETAIL_ENABLE_IF_TYPE
 * @brief Produce a type for SFINAE defaulted type parameters.
 *
 * @param CONDITION Boolean expression controlling whether the type exists.
 * @return `void` when `CONDITION` is true; substitution failure when
 * `CONDITION` is false.
 * @pre Use where a type is expected, typically as the default for an otherwise
 * unused template type parameter.
 * @post A false condition removes the surrounding declaration from overload
 * resolution in C++17 mode.
 * @invariant The macro delegates to `memsafe::detail::enable_if_t`, so it is
 * available in both compatibility and baseline modes without C++20 syntax.
 * @throws Nothing directly; it is compile-time declaration plumbing.
 * @note Ownership/thread-safety: the macro declares no runtime object and has
 * no synchronization behavior.
 *
 * Example:
 * @code
 * template <typename T,
 *           typename Enable = MEMSAFE_DETAIL_ENABLE_IF_TYPE(
 *               std::is_integral<T>::value)>
 * T identity(T value) { return value; }
 * @endcode
 */
#define MEMSAFE_DETAIL_ENABLE_IF_TYPE(CONDITION) \
    ::memsafe::detail::enable_if_t<(CONDITION), void>

/**
 * @def MEMSAFE_DETAIL_ENABLE_IF_RETURN
 * @brief Produce a SFINAE-controlled function return type.
 *
 * @param CONDITION Boolean expression controlling whether the return type
 * exists.
 * @param TYPE Return type to expose when `CONDITION` is true.
 * @return `TYPE` when `CONDITION` is true; substitution failure when
 * `CONDITION` is false.
 * @pre Use as a function template return type where SFINAE on the return type
 * is acceptable for overload resolution.
 * @post A false condition removes the function from overload resolution in
 * C++17 mode.
 * @invariant The macro expands only to `enable_if` alias syntax and never
 * names C++20-only diagnostic or constraint facilities.
 * @throws Nothing directly; runtime exception behavior is the behavior of the
 * function whose return type is controlled.
 * @note Ownership/thread-safety: the macro declares no runtime object and has
 * no synchronization behavior.
 *
 * Example:
 * @code
 * template <typename T>
 * MEMSAFE_DETAIL_ENABLE_IF_RETURN(std::is_integral<T>::value, T)
 * identity(T value) { return value; }
 * @endcode
 */
#define MEMSAFE_DETAIL_ENABLE_IF_RETURN(CONDITION, TYPE) \
    ::memsafe::detail::enable_if_t<(CONDITION), TYPE>

#endif /* MEMSAFE_CONFIG_HPP */
