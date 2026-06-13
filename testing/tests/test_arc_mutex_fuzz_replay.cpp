/**
 * @file test_arc_mutex_fuzz_replay.cpp
 * @brief Sanitizer-lane wrapper for the Arc/Mutex fuzz replay harness.
 *
 * @details
 * Work package: CPP_MEMSAFE-0540-TEST.
 *
 * Purpose:
 * - Make the byte-driven `memsafe::Arc<memsafe::Mutex<T>>` fuzz replay target
 *   reachable from the sanitizer lanes without editing `testing/infra_lanes.json`.
 * - Reuse the canonical `testing/tests/fuzz/fuzz_arc_mutex_replay.cpp`
 *   translation unit verbatim, so the fuzz lane and sanitizer lanes execute the
 *   same clone/drop/lock/unlock state-machine oracle through the same
 *   `fuzz_replay.hpp` adapter.
 *
 * Key invariants:
 * - This file does not define an alternate model, alternate corpus, or alternate
 *   `LLVMFuzzerTestOneInput`; it includes the canonical fuzz target exactly once.
 * - The F4 default sanitizer lane discovers `testing/tests/*.cpp`, while the
 *   optional fuzz lane discovers both `testing/tests/*.cpp` and
 *   `testing/tests/fuzz/*.cpp`. The distinct top-level filename keeps CMake
 *   target names unique when both files are present.
 * - Any ASan or UBSan report from this wrapper is therefore a report from the
 *   same Arc/Mutex replay oracle used by the fuzz-lane executable.
 *
 * Ownership and thread-safety:
 * - The included harness owns all replay state per input and remains
 *   single-threaded. This wrapper owns no state and introduces no additional
 *   synchronization.
 */

/*
 * CPP_MEMSAFE-0540-TEST acceptance requires the fuzz replay to run under both
 * the fuzz and sanitizer lanes. The generic F4 sanitizer lane only compiles
 * `testing/tests/*.cpp`; including the canonical fuzz source here makes that
 * lane build the same executable body without broadening lane globs or forking
 * the oracle.
 */
#include "fuzz/fuzz_arc_mutex_replay.cpp"
