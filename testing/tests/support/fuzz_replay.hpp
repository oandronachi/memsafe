/**
 * @file fuzz_replay.hpp
 * @brief Normal-executable replay adapter for byte-sequence fuzz entry points.
 *
 * @details
 * Work package: CPP_MEMSAFE-0035-TEST.
 *
 * Purpose:
 * - Invoke libFuzzer-shaped entry points from a plain `main()` so F4 runtime
 *   tests can replay fuzz corpora without linking libFuzzer.
 * - Keep true timed libFuzzer execution optional future infrastructure while
 *   preserving the same byte-driven state-machine interface for fuzz work
 *   packages.
 * - Support built-in byte seeds and command-line corpus files.
 *
 * Key invariants:
 * - Each replay case is passed exactly once to the entry point.
 * - A nonzero entry-point return, thrown exception, or unreadable corpus file
 *   produces a nonzero process status.
 * - Empty corpora still replay one empty byte sequence so harnesses exercise
 *   their zero-length path.
 *
 * Ownership and thread-safety:
 * - `byte_sequence` owns its byte vector.
 * - Replay is single-threaded; entry points own any synchronization they need.
 */
#ifndef MEMSAFE_TEST_SUPPORT_FUZZ_REPLAY_HPP
#define MEMSAFE_TEST_SUPPORT_FUZZ_REPLAY_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace memsafe {
namespace test_support {
namespace fuzz {

/**
 * @brief Owned byte corpus entry used by replay tests.
 *
 * @pre Construct with helper functions or assign a stable name and byte vector
 * directly.
 * @post The object owns its name and bytes.
 * @invariant `bytes.data()` and `bytes.size()` are the exact pair passed to
 * fuzz entry points.
 * @throws Copying or assigning can throw `std::bad_alloc`.
 * @note Thread-safety: immutable instances may be shared across threads.
 *
 * Example:
 * @code
 * auto seed = memsafe::test_support::fuzz::bytes_case(
 *     "open-close", {0x01u, 0x02u});
 * @endcode
 */
struct byte_sequence {
    /// Stable display name for the corpus entry.
    std::string name;
    /// Owned bytes passed to the fuzz entry point.
    std::vector<std::uint8_t> bytes;
};

/**
 * @typedef fuzz_entry_point
 * @brief Function-pointer type matching libFuzzer's byte entry-point shape.
 *
 * @param data Pointer to the first byte, or an implementation-defined non-null
 * or null value when `size == 0`; the entry point must not dereference it when
 * the size is zero.
 * @param size Number of bytes available at `data`.
 * @return Zero for success; nonzero for a replay failure.
 * @pre The pointed-to bytes remain valid for the duration of the call.
 * @post The adapter does not inspect entry-point side effects.
 * @invariant This shape is source-compatible with `LLVMFuzzerTestOneInput`.
 * @throws Entry points should not throw; `replay` catches exceptions and turns
 * them into nonzero status.
 * @note Ownership/thread-safety: the entry point borrows bytes and owns no
 * storage through this typedef.
 */
using fuzz_entry_point = int (*)(const std::uint8_t* data, std::size_t size);

namespace detail {

/**
 * @brief Invoke a fuzz entry point and normalize its return type to `int`.
 *
 * @tparam EntryPoint Callable compatible with
 * `int(const std::uint8_t*, std::size_t)` or
 * `void(const std::uint8_t*, std::size_t)`.
 * @param entry Entry point to invoke.
 * @param data Pointer to replay bytes, or null when `size == 0`.
 * @param size Number of replay bytes.
 * @return Zero for void entry points or the integer status returned by
 * non-void entry points.
 * @pre `entry` must not retain `data` after returning.
 * @post The entry point has been invoked exactly once.
 * @invariant Supporting both void and integer-returning entry points keeps F4
 * replay tests source-compatible with common libFuzzer target styles.
 * @throws Any exception thrown by `entry` propagates to `replay`, where it is
 * converted into a nonzero process status.
 * @note Ownership/thread-safety: borrows the byte buffer and callable for the
 * duration of the call.
 */
template <typename EntryPoint>
int invoke(EntryPoint&& entry, const std::uint8_t* data, std::size_t size) {
    using return_type = decltype(std::forward<EntryPoint>(entry)(data, size));
    if constexpr (std::is_void<return_type>::value) {
        std::forward<EntryPoint>(entry)(data, size);
        return 0;
    } else {
        return static_cast<int>(std::forward<EntryPoint>(entry)(data, size));
    }
}

} // namespace detail

/**
 * @brief Build a named corpus entry from explicit bytes.
 *
 * @param name Stable corpus-entry name. Null is treated as the empty string.
 * @param bytes Bytes to copy into the corpus entry.
 * @return Owned byte sequence.
 * @pre Byte values must fit in `std::uint8_t`.
 * @post The returned object owns a copy of the name and bytes.
 * @invariant The byte order is preserved exactly.
 * @throws `std::bad_alloc` if allocation for the name or vector fails.
 * @note Thread-safety: uses only local state.
 */
inline byte_sequence bytes_case(const char* name,
                                std::initializer_list<std::uint8_t> bytes) {
    byte_sequence sequence;
    sequence.name = name == nullptr ? "" : name;
    sequence.bytes.assign(bytes.begin(), bytes.end());
    return sequence;
}

/**
 * @brief Build a named corpus entry from an ASCII string literal.
 *
 * @param name Stable corpus-entry name. Null is treated as the empty string.
 * @param text Null-terminated byte text. Null is treated as an empty sequence.
 * @return Owned byte sequence containing bytes before the terminating null.
 * @pre `text` must point to a null-terminated byte string when non-null.
 * @post The returned object owns a copy of the name and text bytes.
 * @invariant The terminating null byte is not included.
 * @throws `std::bad_alloc` if allocation fails.
 * @note Thread-safety: uses only local state.
 */
inline byte_sequence ascii_case(const char* name, const char* text) {
    byte_sequence sequence;
    sequence.name = name == nullptr ? "" : name;
    const char* cursor = text == nullptr ? "" : text;
    while (*cursor != '\0') {
        sequence.bytes.push_back(static_cast<std::uint8_t>(*cursor));
        ++cursor;
    }
    return sequence;
}

/**
 * @brief Load a command-line corpus file into an owned byte sequence.
 *
 * @param path File path to read as binary data.
 * @param error Optional output string populated when the file cannot be read.
 * @return Owned byte sequence named by `path`; bytes are empty on read failure.
 * @pre `path` must be non-null and name a readable file for successful loads.
 * @post On success, `error` is cleared when supplied. On failure, `error`
 * contains a diagnostic and the returned bytes are empty.
 * @invariant Bytes are read without text translation.
 * @throws `std::bad_alloc` if vector or string allocation fails.
 * @note Thread-safety: uses only local file and memory state.
 */
inline byte_sequence load_file_case(const char* path, std::string* error = nullptr) {
    byte_sequence sequence;
    sequence.name = path == nullptr ? "" : path;
    if (path == nullptr) {
        if (error != nullptr) {
            *error = "null corpus path";
        }
        return sequence;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = std::string("could not open corpus file: ") + path;
        }
        return sequence;
    }

    char ch = '\0';
    while (input.get(ch)) {
        sequence.bytes.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(ch)));
    }
    if (!input.eof()) {
        if (error != nullptr) {
            *error = std::string("could not read corpus file: ") + path;
        }
        sequence.bytes.clear();
        return sequence;
    }
    if (error != nullptr) {
        error->clear();
    }
    return sequence;
}

/**
 * @brief Replay an in-memory byte corpus against a fuzz entry point.
 *
 * @tparam EntryPoint Callable compatible with
 * `int(const std::uint8_t*, std::size_t)` or
 * `void(const std::uint8_t*, std::size_t)`.
 * @param corpus Corpus entries to replay. If empty, an empty byte sequence is
 * replayed once.
 * @param entry Entry point under test.
 * @return Zero when every entry succeeds; otherwise a nonzero status suitable
 * for returning from `main()`.
 *
 * @pre `entry` must not retain the byte pointer after returning.
 * @post Every selected corpus entry has been invoked exactly once until the
 * first failure.
 * @invariant Returned status is compatible with F4's exit-code verdict model.
 * @throws `std::bad_alloc` if constructing the implicit empty case fails.
 * Exceptions thrown by `entry` are caught and converted to status 1.
 * @note Ownership/thread-safety: replay borrows corpus entries and runs on the
 * calling thread.
 *
 * Example:
 * @code
 * int status = memsafe::test_support::fuzz::replay(
 *     {memsafe::test_support::fuzz::bytes_case("empty", {})},
 *     LLVMFuzzerTestOneInput);
 * @endcode
 */
template <typename EntryPoint>
int replay(const std::vector<byte_sequence>& corpus, EntryPoint&& entry) {
    std::vector<byte_sequence> fallback;
    const std::vector<byte_sequence>* selected = &corpus;
    if (corpus.empty()) {
        fallback.push_back(bytes_case("empty", {}));
        selected = &fallback;
    }

    for (std::vector<byte_sequence>::const_iterator it = selected->begin();
         it != selected->end(); ++it) {
        try {
            const std::uint8_t* data = it->bytes.empty() ? nullptr : it->bytes.data();
            const int status = detail::invoke(entry, data, it->bytes.size());
            if (status != 0) {
                std::fprintf(stderr, "fuzz replay failed: %s returned %d\n",
                             it->name.c_str(), status);
                return status;
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "fuzz replay threw in %s: %s\n",
                         it->name.c_str(), ex.what());
            return 1;
        } catch (...) {
            std::fprintf(stderr, "fuzz replay threw in %s\n", it->name.c_str());
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Implement a normal F4 `main()` for a fuzz replay executable.
 *
 * @tparam EntryPoint Callable compatible with libFuzzer's byte entry shape.
 * @param argc Argument count from `main`.
 * @param argv Argument vector from `main`. Additional arguments are treated as
 * binary corpus file paths.
 * @param entry Entry point under test.
 * @param built_in Built-in corpus entries used when no file paths are supplied.
 * @return Zero when replay succeeds; nonzero when a file cannot be read or an
 * entry point fails.
 *
 * @pre Pass the original `argc` and `argv` from a standalone executable.
 * `entry` must not retain byte pointers after returning.
 * @post Either command-line files or built-in seeds have been replayed through
 * `entry`.
 * @invariant The executable remains a normal exit-code F4 test even though its
 * entry point can later be reused by true libFuzzer infrastructure.
 * @throws `std::bad_alloc` if corpus construction fails.
 * @note Ownership/thread-safety: replay owns loaded file bytes and runs on the
 * calling thread.
 *
 * Example:
 * @code
 * int main(int argc, char** argv) {
 *     return memsafe::test_support::fuzz::replay_main(
 *         argc, argv, LLVMFuzzerTestOneInput,
 *         {memsafe::test_support::fuzz::bytes_case("empty", {})});
 * }
 * @endcode
 */
template <typename EntryPoint>
int replay_main(int argc,
                char* const argv[],
                EntryPoint&& entry,
                const std::vector<byte_sequence>& built_in) {
    std::vector<byte_sequence> corpus;
    if (argc > 1) {
        corpus.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            std::string error;
            corpus.push_back(load_file_case(argv[i], &error));
            if (!error.empty()) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 2;
            }
        }
    } else {
        corpus = built_in;
    }
    return replay(corpus, std::forward<EntryPoint>(entry));
}

} // namespace fuzz
} // namespace test_support
} // namespace memsafe

#endif /* MEMSAFE_TEST_SUPPORT_FUZZ_REPLAY_HPP */
