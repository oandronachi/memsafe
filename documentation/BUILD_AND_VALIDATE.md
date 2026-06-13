# Memory-Safe C++ Library - Build & Validate Tool

A self-contained, generic build-and-validate tool plus a small barebones fixture.
The tool makes no assumptions about the code under test: drop in the real
(not-yet-built) library headers and tests, keep a few folder conventions, and the
same one-liner validates them on whatever backend the machine has: local
toolchain, WSL, Docker, or Podman. No agent compiles or runs C++ in its own
sandbox; everything runs on a real backend on the target machine.

## TL;DR

```text
python tools/run_tests.py --backend auto
```

That is the whole default path. It auto-detects a backend, builds every
test/example it finds, runs them, and exits `0` if all selected lanes pass.

**Where your code goes** — discovery is purely by location:

- `include/**` — put your headers here; this folder is added to the compiler **include path**.
- `testing/tests/*.cpp` — each is built and run as a test (process exit code `0` = pass).
- `examples/*.cpp` — each is built and run as a smoke test.
- `testing/tests/compile_fail/*.cpp` — each must *fail* to compile.

> **Header-only only.** The tool adds `include/` to the include path and compiles each
> test/example on its own. It does **not** build or link a separate library or an external
> test framework, so it supports a **header-only library** validated by **header-only /
> exit-code tests**. Compiled library `.cpp` units and link-dependent frameworks
> (GoogleTest, google/benchmark) are **not wired yet** — see **Implementation Sources &
> Test Frameworks** below.

The final line is machine-readable:

```text
SUMMARY backend=local passed=4 failed=0 result=OK lanes_ok=[debug,release,asan-ubsan,cxx17] lanes_failed=[]
```

A structured result is written to `testing/build/summary.json`. On Windows you
can instead double-click `tools/run_tests.py` for a small GUI.

## The Drop-In Contract

The tool discovers work by location, never by hardcoded names or types.

| Put here | What the tool does |
|----------|--------------------|
| `include/**` | added to the compiler include path |
| `testing/tests/*.cpp` | each file is built and run as a test; exit code 0 means pass |
| `examples/*.cpp` | each file is built and run as a smoke test |
| `testing/tests/compile_fail/*.cpp` | each file must fail to compile for its declared reason |
| `testing/infra_lanes.json` | optional code-side lane definitions |

Add or remove a `.cpp` and it is picked up automatically on the next run. A test
is just a `main()` that returns non-zero on failure; use the tiny
`testing/test_harness.hpp` or any framework you prefer.

### Implementation Sources & Test Frameworks

**The tool supports header-only libraries only.** It adds `include/` to the compiler
include path and compiles each `tests/` and `examples/` translation unit into its own
executable. It deliberately does **not** (a) compile or link a separate library target
(there is no `src/` build), nor (b) download / `find_package` / link an external test
framework. In practice:

- **Library under test** must be **header-only** — everything reachable via `#include`
  from `include/`. This matches the memsafe PRD (F1: "no `.cpp` files in the library
  distribution"). A library shipping compiled `.cpp` units would need an added
  `src/*.cpp` -> library-target step.
- **Test framework** can be anything whose test compiles to an executable that exits
  `0` / non-zero. **Header-only** frameworks (doctest, Catch2 single-header) just drop
  into `include/`. **Link-dependent** frameworks — notably **GoogleTest** and
  **google/benchmark**, which the test plan (F2) calls for — are **not supported yet**;
  they need an added `find_package` / `FetchContent` + link step and a lane key to request it.

Lifting this limitation is an explicit extension point: an optional `src/` glob (for
compiled units) and a per-lane `link` / `frameworks` key (for linked frameworks), added
when the real, not-yet-built code needs them.

### Negative Compile Tests

A must-not-compile test passes only when the build fails for the reason you
declare, not merely because some unrelated error occurred. Declare the expected
diagnostic with an in-source directive:

```cpp
// INFRA_EXPECT_FAIL: nodiscard|unused-result|discarding return value|C4834
```

The regex is matched against the captured compiler diagnostic. The driver forces
`LC_ALL=C` and `LANG=C` for the nested `cmake --build` where POSIX-style locale
variables are honored. For MSVC/clang-cl, prefer stable diagnostic codes such as
`C2280` or `C4834` over prose. Keep regexes permissive across compilers and avoid
`;` because CMake treats it as a list separator. If a compile-fail source omits
the directive, CMake warns at configure time and the test falls back to passing
on any build failure.

### Lanes

Anything build-wide and library-specific lives in `testing/infra_lanes.json`,
not in the runner or generic CMake:

```json
{
  "lanes": {
    "debug":      { "build_type": "Debug",   "std": "20" },
    "release":    { "build_type": "Release", "std": "20", "defines": ["MEMSAFE_RELEASE_CHECKS=0"] },
    "asan-ubsan": { "build_type": "Debug",   "std": "20", "sanitize": "address,undefined" },
    "cxx17":      { "build_type": "Debug",   "std": "17", "defines": ["MEMSAFE_CXX17_COMPAT=1"] }
  },
  "default": ["debug", "release", "asan-ubsan", "cxx17"]
}
```

The CMake project only understands generic knobs: `INFRA_STD`,
`INFRA_SANITIZE`, `INFRA_DEFINES`, source globs/excludes, and an optional test
launcher. The `MEMSAFE_*` values above are this fixture's choices.

Optional per-lane keys:

| Key | Meaning |
|-----|---------|
| `image` | container image tag or digest pin for Docker/Podman lanes |
| `sources.tests` | runtime test glob list, relative to `testing/` unless absolute |
| `sources.examples` | example/smoke glob list |
| `sources.compile_fail` | compile-fail glob list |
| `exclude` | glob list removed from discovered sources |
| `test_launcher` | command list prepended to runtime tests, for tools such as Valgrind |

The shipped lane file includes non-default optional stubs for TSan, MSan, HWASan,
Valgrind, property, fuzz, concurrency, and performance categories. They are
wired so future tests can be dropped under `testing/tests/property/`,
`testing/tests/fuzz/`, `testing/tests/concurrency/`, and `testing/tests/perf/`;
they do not make RapidCheck, libFuzzer, Relacy, google/benchmark, Valgrind, or
advanced sanitizers mandatory for the green baseline.

Run a subset with `--preset`:

```text
python tools/run_tests.py --backend auto --preset debug --preset asan-ubsan
python tools/run_tests.py --backend auto --preset tsan
```

The barebones `include/memsafe/memsafe.hpp`, `examples/`, and `testing/tests/`
are a removable smoke fixture so the tool has something green to build today.
Delete them and drop in the real implementation; the tool does not care.

## Machine-Readable Results

Every run writes a versioned JSON result so AI/CI consumers parse a stable schema
instead of scraping log text:

```json
{
  "schema_version": 1,
  "tool_version": "0+local",
  "result": "OK",
  "backend": "local",
  "image_ref": null,
  "requested_lanes": ["debug", "release", "asan-ubsan", "cxx17"],
  "lanes": [{ "name": "debug", "result": "passed", "detail": "" }],
  "fallbacks": []
}
```

`schema_version` increments only on incompatible changes. `tool_version`
correlates a result with runner behavior. `fallbacks` lists any backend skipped
in auto mode and why. CTest JUnit XML is written to
`testing/build/<lane>/junit.xml` when the selected CTest supports
`--output-junit`; older CTest installs still run and just omit the XML artifact.

## Backend Selection

`--backend auto` uses the first backend that is installed and reachable:

| Order | Backend | Used when |
|-------|---------|-----------|
| 1 | `local` | CMake, CTest, and a C++ compiler are on `PATH` |
| 2 | `wsl` | on Windows, a non-internal WSL distro starts and can run `true` |
| 3 | `docker` | `docker info` succeeds |
| 4 | `podman` | `podman info` succeeds |

Automatic fallback applies only in auto mode. If a backend fails on
infrastructure grounds, such as a missing toolchain, unreachable daemon, a distro
that will not start, or an image that will not pull, the runner records the
reason in `fallbacks` and tries the next backend. A validation failure, such as a
configure/compile/test error on a working backend, stops and is reported. A
forced backend such as `--backend docker` never falls back silently.

Inspect availability without running tests:

```text
python tools/run_tests.py --list-backends
```

### Preparing a Dormant Backend

`--prepare-backends` brings an already-installed backend to a reachable state:
start Docker Desktop, run `podman machine start`, or wake the default WSL distro.
It never installs software unless you add `--install-missing --yes-install`.

```text
python tools/run_tests.py --prepare-backends
python tools/run_tests.py --prepare-backends --backend docker
```

Container/WSL lanes bind-mount the folder and build a Linux GCC/Clang lane
inside, which is also where the sanitizer matrix runs. Override with `--image`
and `--wsl-distro`.

### When Nothing Is Available

The tool prints OS-specific install commands and exits non-zero, for example on
Windows:

```text
  # Local toolchain (winget)
    winget install -e --id Kitware.CMake
    winget install -e --id Ninja-build.Ninja
    winget install -e --id LLVM.LLVM

  # WSL (Ubuntu)
    wsl --install -d Ubuntu
    wsl -d Ubuntu -- sudo apt-get update
    wsl -d Ubuntu -- sudo apt-get install -y cmake ninja-build g++ clang
```

Add `--install-missing` to always print prompts, or
`--install-missing --yes-install` to attempt the first option automatically.

### Guardrails

Every subprocess runs under two guardrails so the runner cannot hang forever:
`--step-timeout` (per-command wall-clock seconds, default 3600) and
`--no-output-timeout` (kill after N seconds with no output, default 600). Pass
`0` to disable either.

## Layout

```text
run-test-infra/
|- include/memsafe/memsafe.hpp     # barebones header (removable smoke fixture)
|- examples/                       # example/smoke sources (removable)
|- testing/
|  |- CMakeLists.txt               # generic: globs sources, knows no library names
|  |- CMakePresets.json            # IDE convenience presets mirroring lanes
|  |- infra_lanes.json             # code-side lane definitions consumed by the tool
|  |- test_harness.hpp             # tiny optional assert harness
|  |- cmake/
|  |  `- expect_compile_fail.cmake # generic negative-compile driver
|  `- tests/
|     |- *.cpp                     # auto-discovered tests
|     `- compile_fail/*.cpp        # auto-discovered must-not-compile tests
|- tools/
|  |- run_tests.py                 # self-adjusting runner (CLI + GUI)
|  |- infra_common.py              # backend detection / prepare / guardrails / JSON
|  `- clean.py                     # remove build directories + __pycache__
`- .github/workflows/ci.yml        # CI: runs the runner with the local backend
```

`testing/build/` and `__pycache__/` are generated, git-ignored, and never part of
the source deliverable. `tools/clean.py` or the GUI Clean button removes them.

## Requirements

- Python 3.8+ (standard library only).
- Local backend: CMake, CTest, and a C++17/20 compiler such as GCC, Clang, MSVC,
  or clang-cl. Ninja is used automatically if present.
- `CMakePresets.json` convenience presets require CMake 3.21+; the generic
  `testing/CMakeLists.txt` keeps a lower 3.16 floor for direct configure/build
  use where JUnit is not available.
- Container backend: Docker or Podman. WSL backend: a user WSL2 distro on
  Windows.
