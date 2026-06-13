// Minimal, dependency-free test harness for the barebones memsafe build.
// No GoogleTest / RapidCheck required: each test is its own executable whose
// process exit code is the verdict (0 = pass, non-zero = failures). This keeps
// the baseline runnable on any machine with a C++ compiler, by an AI agent
// (exit code) or a human (console output) alike.
#ifndef MEMSAFE_TEST_HARNESS_HPP
#define MEMSAFE_TEST_HARNESS_HPP

#include <cstdio>

namespace mstest {

inline int& failures() {
    static int f = 0;
    return f;
}

inline void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        ++failures();
        std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", expr, file, line);
    }
}

inline int summary(const char* name) {
    if (failures() == 0) {
        std::fprintf(stdout, "PASS: %s\n", name);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %s (%d check(s) failed)\n", name, failures());
    return 1;
}

} // namespace mstest

#define CHECK(cond) ::mstest::check((cond), #cond, __FILE__, __LINE__)

#define CHECK_THROWS(stmt, ExceptionType)                                      \
    do {                                                                       \
        bool memsafe_threw_ = false;                                           \
        try {                                                                  \
            stmt;                                                              \
        } catch (const ExceptionType&) {                                       \
            memsafe_threw_ = true;                                             \
        } catch (...) {                                                        \
        }                                                                      \
        ::mstest::check(memsafe_threw_, "expected " #ExceptionType " from: " #stmt, \
                        __FILE__, __LINE__);                                   \
    } while (0)

#define RUN_TESTS(name) return ::mstest::summary(name)

#endif // MEMSAFE_TEST_HARNESS_HPP
