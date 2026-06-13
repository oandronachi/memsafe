# ===========================================================================
# GENERIC build + validate project for the memsafe infra.
#
# This file makes NO assumptions about the code under test. It auto-discovers
# sources by convention and builds/runs them:
#
#   ../include/**           -> added to the include path (headers live here)
#   tests/*.cpp             -> each is a standalone test exe; exit 0 == pass
#   ../examples/*.cpp       -> each is a smoke-test exe; exit 0 == pass
#   tests/compile_fail/*.cpp-> each MUST fail to compile, FOR ITS DECLARED REASON
#
# The tool knows no type names, headers or macros of the library. Anything
# library-specific (extra -D defines, C++ standard, sanitizers per lane, optional
# source manifests/excludes, optional test launchers) is supplied from outside
# via these cache variables, which the runner fills in from the code-side
# testing/infra_lanes.json:
#
#   INFRA_STD                (e.g. 17 / 20 / 23)
#   INFRA_SANITIZE           (e.g. address,undefined)   [GNU/Clang]
#   INFRA_DEFINES            (semicolon list, e.g. FOO=1;BAR)
#   INFRA_TEST_GLOBS         (default tests/*.cpp)
#   INFRA_EXAMPLE_GLOBS      (default ../examples/*.cpp)
#   INFRA_COMPILE_FAIL_GLOBS (default tests/compile_fail/*.cpp)
#   INFRA_EXCLUDE_GLOBS      (optional glob exclusions relative to testing/)
#   INFRA_TEST_LAUNCHER      (optional command list before runtime tests)
#
# Drop in real source + tests, keep the folder conventions, and the same
# one-liner (python tools/run_tests.py --backend auto) validates them.
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(infra_validate LANGUAGES CXX)

set(INFRA_STD      20 CACHE STRING "C++ standard: 17 / 20 / 23")
set(INFRA_SANITIZE "" CACHE STRING "Comma list for -fsanitize (GNU/Clang)")
set(INFRA_DEFINES  "" CACHE STRING "Semicolon-separated extra compile definitions")
set(INFRA_TEST_GLOBS "tests/*.cpp" CACHE STRING "Runtime test glob(s), relative to testing/")
set(INFRA_EXAMPLE_GLOBS "../examples/*.cpp" CACHE STRING "Example/smoke glob(s), relative to testing/")
set(INFRA_COMPILE_FAIL_GLOBS "tests/compile_fail/*.cpp" CACHE STRING "Compile-fail glob(s), relative to testing/")
set(INFRA_EXCLUDE_GLOBS "" CACHE STRING "Glob(s) to exclude from discovered sources")
set(INFRA_TEST_LAUNCHER "" CACHE STRING "Optional semicolon-list command prepended to runtime tests")

set(CMAKE_CXX_STANDARD ${INFRA_STD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INFRA_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../include")

if(MSVC)
  set(INFRA_WARN   /W4)
  set(INFRA_WERROR /WX)
else()
  set(INFRA_WARN   -Wall -Wextra)
  set(INFRA_WERROR -Werror -Wunused-result)
endif()

set(INFRA_SAN_FLAGS "")
if(INFRA_SANITIZE AND NOT MSVC)
  set(INFRA_SAN_FLAGS -fsanitize=${INFRA_SANITIZE} -fno-omit-frame-pointer -g)
endif()

enable_testing()

function(_infra_abspath out_var pattern)
  if(IS_ABSOLUTE "${pattern}")
    set(${out_var} "${pattern}" PARENT_SCOPE)
  else()
    get_filename_component(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${pattern}" ABSOLUTE)
    set(${out_var} "${_abs}" PARENT_SCOPE)
  endif()
endfunction()

function(_infra_glob_sources out_var)
  set(_sources "")
  foreach(_pattern ${ARGN})
    if(NOT _pattern STREQUAL "")
      _infra_abspath(_abs_pattern "${_pattern}")
      file(GLOB _matched CONFIGURE_DEPENDS "${_abs_pattern}")
      list(APPEND _sources ${_matched})
    endif()
  endforeach()

  set(_excluded "")
  foreach(_pattern IN LISTS INFRA_EXCLUDE_GLOBS)
    if(NOT _pattern STREQUAL "")
      _infra_abspath(_abs_pattern "${_pattern}")
      file(GLOB _matched_excluded CONFIGURE_DEPENDS "${_abs_pattern}")
      list(APPEND _excluded ${_matched_excluded})
    endif()
  endforeach()

  if(_excluded)
    list(REMOVE_ITEM _sources ${_excluded})
  endif()
  if(_sources)
    list(REMOVE_DUPLICATES _sources)
  endif()
  set(${out_var} ${_sources} PARENT_SCOPE)
endfunction()

# Common settings for a normal (must-build, must-run) target.
function(_infra_apply tgt)
  target_include_directories(${tgt} PRIVATE "${INFRA_INCLUDE}")
  if(INFRA_DEFINES)
    target_compile_definitions(${tgt} PRIVATE ${INFRA_DEFINES})
  endif()
  target_compile_options(${tgt} PRIVATE ${INFRA_WARN} ${INFRA_SAN_FLAGS})
  if(INFRA_SAN_FLAGS)
    target_link_options(${tgt} PRIVATE ${INFRA_SAN_FLAGS})
  endif()
endfunction()

function(_infra_add_runtime_test test_name target_name)
  if(INFRA_TEST_LAUNCHER)
    add_test(NAME ${test_name} COMMAND ${INFRA_TEST_LAUNCHER} $<TARGET_FILE:${target_name}>)
  else()
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
  endif()
endfunction()

# --- runtime tests : run as CTest tests ---
_infra_glob_sources(INFRA_TESTS ${INFRA_TEST_GLOBS})
foreach(src ${INFRA_TESTS})
  get_filename_component(name "${src}" NAME_WE)
  add_executable(${name} "${src}")
  _infra_apply(${name})
  _infra_add_runtime_test(${name} ${name})
endforeach()

# --- examples : run as smoke tests ---
_infra_glob_sources(INFRA_EXAMPLES ${INFRA_EXAMPLE_GLOBS})
foreach(src ${INFRA_EXAMPLES})
  get_filename_component(name "${src}" NAME_WE)
  add_executable(${name} "${src}")
  _infra_apply(${name})
  _infra_add_runtime_test(${name} ${name})
endforeach()

# --- compile-fail tests : MUST NOT compile, for the DECLARED reason ---
#
# Each compile-fail source declares the diagnostic it must produce via an
# in-source directive (parsed here, kept code-side so this generic file holds no
# library text):
#
#   // INFRA_EXPECT_FAIL: <regex>
#
# The expectation regex is handed to the generic driver cmake/expect_compile_fail.cmake,
# which builds the target and passes only when the build fails AND the captured
# diagnostic matches the regex -- never merely because *some* build error
# occurred (which could be an unrelated typo or a renamed symbol). (SP7 / Q8.)
_infra_glob_sources(INFRA_CFAIL ${INFRA_COMPILE_FAIL_GLOBS})
foreach(src ${INFRA_CFAIL})
  get_filename_component(name "${src}" NAME_WE)
  add_executable(${name} EXCLUDE_FROM_ALL "${src}")
  target_include_directories(${name} PRIVATE "${INFRA_INCLUDE}")
  if(INFRA_DEFINES)
    target_compile_definitions(${name} PRIVATE ${INFRA_DEFINES})
  endif()
  target_compile_options(${name} PRIVATE ${INFRA_WARN} ${INFRA_WERROR})

  # Read the code-side expected-reason directive, if present.
  set(_expect "")
  file(STRINGS "${src}" _expect_lines REGEX "INFRA_EXPECT_FAIL:")
  if(_expect_lines)
    list(GET _expect_lines 0 _line)
    string(REGEX REPLACE "^.*INFRA_EXPECT_FAIL:[ \t]*" "" _expect "${_line}")
    string(STRIP "${_expect}" _expect)
  else()
    message(WARNING
      "compile_fail/${name}.cpp has no '// INFRA_EXPECT_FAIL: <regex>' directive; "
      "it will pass on ANY build failure. Add the directive to pin the expected diagnostic.")
  endif()

  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND}
                   -D "BUILD_DIR=${CMAKE_BINARY_DIR}"
                   -D "TARGET=${name}"
                   -D "CONFIG=$<CONFIG>"
                   -D "EXPECT=${_expect}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_compile_fail.cmake")
endforeach()
