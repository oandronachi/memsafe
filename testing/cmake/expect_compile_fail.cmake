# ===========================================================================
# GENERIC compile-fail driver. Contains NO library-specific text.
#
# Invoked in CMake script mode (cmake -P) as a CTest test command. It builds a
# single must-not-compile target and decides pass/fail:
#
#   * build SUCCEEDS            -> FAIL  (the code that must not compile, did)
#   * build FAILS, EXPECT empty -> PASS  (any failure accepted; a configure-time
#                                         warning already flagged the missing
#                                         expectation)
#   * build FAILS, EXPECT set   -> PASS iff the captured diagnostic matches the
#                                  EXPECT regex, else FAIL ("failed for the
#                                  wrong reason")
#
# This pins a negative compile test to its INTENDED diagnostic (SP7 / Q8). The
# expected-reason regex is declared CODE-SIDE, in the test source's
# `// INFRA_EXPECT_FAIL: <regex>` directive, so this driver and the project's
# CMakeLists.txt never hardcode any compiler wording or library symbol.
#
# Required -D variables: BUILD_DIR, TARGET, CONFIG. Optional: EXPECT.
# ===========================================================================
if(NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET OR NOT DEFINED CONFIG)
  message(FATAL_ERROR "expect_compile_fail.cmake requires -DBUILD_DIR, -DTARGET, -DCONFIG")
endif()
if(NOT DEFINED EXPECT)
  set(EXPECT "")
endif()

# Force a stable C/POSIX diagnostic locale for GCC/Clang-family tools that honor
# LC_ALL/LANG. MSVC/clang-cl users should still key EXPECT on stable diagnostic
# codes (for example C2280/C4834) rather than prose.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C LANG=C
          "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET}"
  RESULT_VARIABLE build_rc
  OUTPUT_VARIABLE build_out
  ERROR_VARIABLE  build_err)

set(diag "${build_out}\n${build_err}")

if(build_rc STREQUAL "0")
  message(FATAL_ERROR
    "compile-fail target '${TARGET}' COMPILED successfully but was expected to FAIL.\n"
    "${diag}")
endif()

if(EXPECT STREQUAL "")
  message(STATUS "compile-fail target '${TARGET}' failed to build as required (no expected-reason regex).")
  return()
endif()

if(diag MATCHES "${EXPECT}")
  message(STATUS "compile-fail target '${TARGET}' failed for the expected reason (/${EXPECT}/).")
  return()
endif()

message(FATAL_ERROR
  "compile-fail target '${TARGET}' failed, but NOT for the expected reason.\n"
  "Expected diagnostic to match: /${EXPECT}/\n"
  "Actual diagnostic was:\n${diag}")
