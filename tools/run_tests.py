#!/usr/bin/env python3
"""Self-adjusting build + validate runner for the memsafe infra.

The runner is GENERIC: it knows nothing about the library under test. It
discovers an execution backend (local / WSL / Docker / Podman), then drives a
convention-based CMake project that auto-discovers whatever source + tests are
present. Library-specific build knobs (C++ standard, sanitizers, extra defines
per lane, optional source manifests, optional test launchers, container image)
come from the code-side file testing/infra_lanes.json; if that file is absent,
generic defaults are used.

Behaviour highlights:
  * --backend auto    : try local -> WSL -> Docker -> Podman, in that order, and
                        fall back to the NEXT backend ONLY on a backend-
                        infrastructure failure (missing toolchain, unreachable
                        daemon, distro that will not start, image pull failure).
                        A real validation failure (configure/compile/test error
                        on a working backend) STOPS and is reported -- it is
                        never silently retried elsewhere. (SP4 + SP6.)
  * --prepare-backends: bring an already-installed but dormant backend to a
                        reachable state (start Docker Desktop / podman machine /
                        wake the default WSL distro). Never installs software
                        unless combined with --install-missing --yes-install.
  * machine-readable  : a stable final SUMMARY line AND a versioned JSON result
                        at testing/build/summary.json (schema_version +
                        tool_version + image_ref + per-lane verdicts +
                        fallbacks). CTest JUnit XML is written per lane when
                        the target CTest supports --output-junit. (SP5.)

Two audiences, one script:
  * AI agents / CI : run as a CLI. Exit code is the verdict; the last line is a
                     machine-readable SUMMARY. No interaction required.
  * Humans         : double-click on Windows to get a small Tkinter GUI.

TL;DR:
  python tools/run_tests.py --backend auto
"""
from __future__ import annotations

import argparse
import json
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import infra_common as ic  # noqa: E402

# Per-step guardrail defaults (seconds). Overridable via CLI.
DEFAULT_STEP_TIMEOUT = 3600
DEFAULT_NO_OUTPUT_TIMEOUT = 600


# ---- result types --------------------------------------------------------
@dataclass
class LaneResult:
    name: str
    result: str            # "passed" | "failed" | "skipped"
    detail: str = ""


@dataclass
class BackendOutcome:
    backend: str
    lanes: List[LaneResult] = field(default_factory=list)
    infra_error: Optional[str] = None   # set -> backend-infrastructure failure
    image_ref: Optional[str] = None

    @property
    def passed(self) -> List[str]:
        return [l.name for l in self.lanes if l.result == "passed"]

    @property
    def failed(self) -> List[str]:
        return [l.name for l in self.lanes if l.result == "failed"]


# ---- lane configuration (code-side, not baked into the tool) -------------
def load_lanes() -> Tuple[dict, list]:
    cfg = ic.testing_dir() / "infra_lanes.json"
    if cfg.exists():
        try:
            data = json.loads(cfg.read_text(encoding="utf-8"))
            lanes = data.get("lanes", {})
            if lanes:
                return lanes, data.get("default", list(lanes.keys()))
        except Exception as exc:  # noqa: BLE001
            print(f"warning: could not parse {cfg}: {exc}")
    # generic fallback (no library-specific knowledge)
    lanes = {
        "debug":      {"build_type": "Debug",   "std": "20"},
        "release":    {"build_type": "Release", "std": "20"},
        "asan-ubsan": {"build_type": "Debug",   "std": "20", "sanitize": "address,undefined"},
    }
    return lanes, ["debug", "release", "asan-ubsan"]


def _as_list(value) -> List[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(v) for v in value]
    return [str(value)]


def _source_globs(spec: dict, category: str) -> List[str]:
    sources = spec.get("sources") or {}
    if isinstance(sources, dict):
        return _as_list(sources.get(category))
    return []


def lane_defs(spec: dict) -> Tuple[str, List[str]]:
    """(build_type, [generic -D cache args]) for a lane spec.

    Supported generic lane keys:
      * std, build_type, sanitize, defines
      * sources.tests / sources.examples / sources.compile_fail
      * exclude: glob(s), relative to testing/
      * test_launcher: command/list prepended to runtime tests (e.g. valgrind)
    """
    bt = spec.get("build_type", "Debug")
    args = [f"-DINFRA_STD={spec.get('std', '20')}"]
    if spec.get("sanitize"):
        args.append(f"-DINFRA_SANITIZE={spec['sanitize']}")

    defines = _as_list(spec.get("defines"))
    if defines:
        args.append("-DINFRA_DEFINES=" + ";".join(defines))

    source_vars = {
        "tests": "INFRA_TEST_GLOBS",
        "examples": "INFRA_EXAMPLE_GLOBS",
        "compile_fail": "INFRA_COMPILE_FAIL_GLOBS",
    }
    for category, cache_var in source_vars.items():
        globs = _source_globs(spec, category)
        if globs:
            args.append(f"-D{cache_var}=" + ";".join(globs))

    excludes = _as_list(spec.get("exclude") or spec.get("excludes"))
    if excludes:
        args.append("-DINFRA_EXCLUDE_GLOBS=" + ";".join(excludes))

    launcher = _as_list(spec.get("test_launcher"))
    if launcher:
        args.append("-DINFRA_TEST_LAUNCHER=" + ";".join(launcher))

    return bt, args


def lane_image(spec: dict, default_image: str) -> str:
    """Container image for a lane: lane-file 'image' wins over the CLI default.

    Accepts both a moving tag (ubuntu:24.04) and a digest pin
    (ubuntu:24.04@sha256:...). Ownership of the exact pin belongs code-side
    (lane file / CI), not in this generic runner. (Q7.)
    """
    return spec.get("image", default_image)


def select_default(backend: str, lanes: dict, default_list: list) -> list:
    sel = [ln for ln in default_list if ln in lanes]
    # ASan/UBSan flags are GNU/Clang only; drop sanitizer lanes for Windows/MSVC local.
    if backend == "local" and ic.host_os() == "windows":
        sel = [ln for ln in sel if not lanes[ln].get("sanitize")]
    return sel


def _ctest_cmd(bdir: Path, build_type: str) -> List[str]:
    cmd = ["ctest", "--test-dir", str(bdir), "--build-config", build_type,
           "--output-on-failure"]
    if ic.ctest_supports_junit():
        cmd += ["--output-junit", str(bdir / "junit.xml")]
    return cmd


# ---- backend runners -----------------------------------------------------
def run_local(run_lanes, lanes, args, log) -> BackendOutcome:
    td = ic.testing_dir()
    gen = ic.find_generator()
    outcome = BackendOutcome(backend="local")
    st, no_out = args.step_timeout, args.no_output_timeout
    for lane in run_lanes:
        bt, defs = lane_defs(lanes[lane])
        bdir = td / "build" / lane
        log(f"\n=== [local] lane: {lane} ({bt}) ===")
        cfg = ["cmake", "-S", str(td), "-B", str(bdir)]
        if gen:
            cfg += ["-G", gen]
        cfg += [f"-DCMAKE_BUILD_TYPE={bt}"] + defs
        r = ic.run(cfg, log=log, timeout=st, no_output_timeout=no_out)
        if not r.ok:
            outcome.lanes.append(LaneResult(lane, "failed", "configure failed"))
            continue
        r = ic.run(["cmake", "--build", str(bdir), "--config", bt, "-j", str(args.jobs)],
                   log=log, timeout=st, no_output_timeout=no_out)
        if not r.ok:
            outcome.lanes.append(LaneResult(lane, "failed", "build failed"))
            continue
        r = ic.run(_ctest_cmd(bdir, bt), log=log, timeout=st, no_output_timeout=no_out)
        outcome.lanes.append(LaneResult(lane, "passed" if r.ok else "failed",
                                        "" if r.ok else "ctest reported failures"))
    return outcome


def _remote_script(run_lanes, lanes, work_testing) -> str:
    """Shell script run inside WSL/container. Best-effort toolchain bootstrap,
    then per-lane configure/build/ctest with per-lane PASS/FAIL markers so the
    Python side can attribute results. Emits SENTINEL_NO_TOOLCHAIN when no
    compiler can be made available (backend-infrastructure failure)."""
    lines = [
        "set -u",
        # Stabilize compiler diagnostics for compile-fail regex matching.
        "export LC_ALL=C",
        "export LANG=C",
        f"cd {shlex.quote(work_testing)}",
        "if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then",
        "  if command -v apt-get >/dev/null 2>&1; then export DEBIAN_FRONTEND=noninteractive;",
        "    (sudo -n apt-get update -qq && sudo -n apt-get install -y -qq cmake ninja-build g++ clang) >/dev/null 2>&1 \\",
        "      || (apt-get update -qq && apt-get install -y -qq cmake ninja-build g++ clang) >/dev/null 2>&1 || true;",
        "  fi;",
        "fi",
        f"command -v cmake >/dev/null 2>&1 || {{ echo '{ic.SENTINEL_NO_TOOLCHAIN} cmake'; exit 3; }}",
        f"command -v ctest >/dev/null 2>&1 || {{ echo '{ic.SENTINEL_NO_TOOLCHAIN} ctest'; exit 3; }}",
        f"command -v c++   >/dev/null 2>&1 || {{ echo '{ic.SENTINEL_NO_TOOLCHAIN} c++'; exit 3; }}",
        "GEN=''; command -v ninja >/dev/null 2>&1 && GEN='-G Ninja'",
        "JUNIT_FLAG=''; ctest --help 2>/dev/null | grep -q -- '--output-junit' && JUNIT_FLAG='yes'",
        "RC=0",
    ]
    for lane in run_lanes:
        bt, defs = lane_defs(lanes[lane])
        quoted = [shlex.quote(d) for d in defs]
        b = f"build/remote-{lane}"
        junit = f'JUNIT=""; [ -n "$JUNIT_FLAG" ] && JUNIT="--output-junit {b}/junit.xml"'
        lines += [
            f'echo "=== lane {lane} ({bt}) ==="',
            junit,
            f'if cmake -S . -B {b} $GEN -DCMAKE_BUILD_TYPE={bt} {" ".join(quoted)} \\',
            f'   && cmake --build {b} \\',
            f'   && ctest --test-dir {b} --output-on-failure $JUNIT; then',
            f'  echo "INFRA_LANE_PASS {lane}"',
            "else",
            f'  echo "INFRA_LANE_FAIL {lane}"; RC=1',
            "fi",
        ]
    lines.append("exit $RC")
    return "\n".join(lines)


def _parse_remote_lanes(run_lanes, output) -> List[LaneResult]:
    results = []
    for lane in run_lanes:
        if f"INFRA_LANE_PASS {lane}" in output:
            results.append(LaneResult(lane, "passed"))
        elif f"INFRA_LANE_FAIL {lane}" in output:
            results.append(LaneResult(lane, "failed", "remote lane failed"))
        else:
            results.append(LaneResult(lane, "skipped", "lane did not run"))
    return results


def run_container(engine, run_lanes, lanes, args, log) -> BackendOutcome:
    root = ic.repo_root()
    outcome = BackendOutcome(backend=engine)
    by_image = {}
    for lane in run_lanes:
        by_image.setdefault(lane_image(lanes[lane], args.image), []).append(lane)
    outcome.image_ref = ",".join(sorted(by_image)) if len(by_image) > 1 else (next(iter(by_image)) if by_image else args.image)

    for image, image_lanes in by_image.items():
        script = _remote_script(image_lanes, lanes, "/work/testing")
        log(f"\n=== [{engine}] image={image}  mounting {root} -> /work ===")
        cmd = [engine, "run", "--rm", "-v", f"{root}:/work", "-w", "/work", image,
               "bash", "-lc", script]
        r = ic.run(cmd, log=log, timeout=args.step_timeout, no_output_timeout=args.no_output_timeout)
        if not r.ok and ic.classify_failure(engine, r) == "infra":
            outcome.infra_error = f"{engine} backend-infrastructure failure (image/daemon/toolchain)"
            return outcome
        parsed = _parse_remote_lanes(image_lanes, r.output)
        if not any(l.result in ("passed", "failed") for l in parsed) and not r.ok:
            parsed = [LaneResult(ln, "failed", "no lane markers; backend run failed")
                      for ln in image_lanes]
        outcome.lanes.extend(parsed)
    return outcome


def run_wsl(distro, run_lanes, lanes, args, log) -> BackendOutcome:
    work_testing = ic.win_to_wsl(ic.repo_root()) + "/testing"
    script = _remote_script(run_lanes, lanes, work_testing)
    log(f"\n=== [wsl] distro={distro or '(default)'} ===")
    cmd = ["wsl"] + (["-d", distro] if distro else []) + ["--", "bash", "-lc", script]
    r = ic.run(cmd, log=log, timeout=args.step_timeout, no_output_timeout=args.no_output_timeout)
    outcome = BackendOutcome(backend="wsl")
    if not r.ok and ic.classify_failure("wsl", r) == "infra":
        outcome.infra_error = "WSL backend-infrastructure failure (distro/toolchain)"
        return outcome
    outcome.lanes = _parse_remote_lanes(run_lanes, r.output)
    if not any(l.result in ("passed", "failed") for l in outcome.lanes) and not r.ok:
        outcome.lanes = [LaneResult(ln, "failed", "no lane markers; backend run failed")
                         for ln in run_lanes]
    return outcome


# ---- orchestration -------------------------------------------------------
def usable_order(requested, backends) -> List[str]:
    if requested == "auto":
        return [name for name in ic.AUTO_ORDER if backends[name][0]]
    return [requested]


def execute(backend, run_lanes, lanes, args, log) -> BackendOutcome:
    if backend == "local":
        return run_local(run_lanes, lanes, args, log)
    if backend in ("docker", "podman"):
        return run_container(backend, run_lanes, lanes, args, log)
    if backend == "wsl":
        return run_wsl(args.wsl_distro, run_lanes, lanes, args, log)
    return BackendOutcome(backend=backend,
                          lanes=[LaneResult(ln, "failed", "unknown backend") for ln in run_lanes])


def _write_summary(outcome, run_lanes, fallbacks, result_str) -> Path:
    summary_path = ic.testing_dir() / "build" / "summary.json"
    ic.write_summary_json(summary_path, {
        "result": result_str,
        "backend": outcome.backend if outcome else "none",
        "image_ref": (outcome.image_ref if outcome else None),
        "requested_lanes": list(run_lanes),
        "lanes": [{"name": l.name, "result": l.result, "detail": l.detail}
                  for l in (outcome.lanes if outcome else [])],
        "fallbacks": fallbacks,
    })
    return summary_path


def cli_main(args, log=print) -> int:
    lanes, default_list = load_lanes()

    if args.list_backends:
        log("Detected backends (auto order: " + ", ".join(ic.AUTO_ORDER) + "):")
        b = ic.detect_backends()
        for name in ic.AUTO_ORDER:
            ok, detail = b[name]
            log(f"  {name:7} {'USABLE   ' if ok else 'unusable '} {detail}")
        log("Lanes available: " + ", ".join(sorted(lanes.keys())))
        return 0

    if args.prepare_backends:
        targets = [args.backend] if args.backend != "auto" else list(ic.AUTO_ORDER)
        log("Preparing backends: " + ", ".join(targets))
        any_ok = False
        for name in targets:
            ok, detail = ic.prepare_backend(name, log=log)
            log(f"  {name:7} {'READY    ' if ok else 'not ready'} {detail}")
            any_ok = any_ok or ok
        if not any_ok and args.install_missing and args.yes_install:
            ic.run_install(log)
        log(f"SUMMARY backend=prepare passed={'1' if any_ok else '0'} failed=0 "
            f"result={'OK' if any_ok else 'NO_BACKEND'}")
        return 0 if any_ok else 2

    backends = ic.detect_backends()
    order = usable_order(args.backend, backends)

    # No usable backend at all (or a forced backend that is not usable).
    if not order or (args.backend != "auto" and not backends[args.backend][0]):
        if args.backend == "auto":
            log("No usable backend found (tried: " + ", ".join(ic.AUTO_ORDER) + ").")
        else:
            log(f"Requested backend '{args.backend}' is not usable: {backends[args.backend][1]}")
        ic.print_install_prompts(log)
        if args.install_missing and args.yes_install:
            ic.run_install(log)
            log("Re-run this script after the install completes.")
        _write_summary(None, [], [], "NO_BACKEND")
        log("SUMMARY backend=none passed=0 failed=0 result=NO_BACKEND")
        return 2

    if args.preset:
        unknown = [p for p in args.preset if p not in lanes]
        if unknown:
            log(f"Unknown lane(s): {', '.join(unknown)}. Available: {', '.join(sorted(lanes))}")
            return 2
        requested_lanes = args.preset
    else:
        requested_lanes = None  # resolved per backend (sanitizer lanes depend on backend)

    fallbacks: List[dict] = []
    for idx, backend in enumerate(order):
        run_lanes = requested_lanes or select_default(backend, lanes, default_list)
        log(f"Backend: {backend}   Lanes: {', '.join(run_lanes)}")
        outcome = execute(backend, run_lanes, lanes, args, log)

        if outcome.infra_error:
            # Forced backend: do NOT fall back silently -- report directly.
            if args.backend != "auto":
                _write_summary(outcome, run_lanes, fallbacks, "BACKEND_FAILURE")
                log(f"\nSUMMARY backend={backend} passed=0 failed=0 "
                    f"result=BACKEND_FAILURE reason=\"{outcome.infra_error}\"")
                return 2
            # Auto mode: record and advance to the next usable backend.
            fallbacks.append({"backend": backend, "reason": outcome.infra_error})
            log(f"  (auto fallback: {outcome.infra_error}; trying next backend)")
            continue

        # A backend actually ran the lanes (success or validation failure). STOP
        # here -- validation failures must never trigger a silent retry. (SP6.)
        result_str = "OK" if not outcome.failed else "FAIL"
        _write_summary(outcome, run_lanes, fallbacks, result_str)
        log("")
        log(f"SUMMARY backend={backend} passed={len(outcome.passed)} "
            f"failed={len(outcome.failed)} result={result_str} "
            f"lanes_ok=[{','.join(outcome.passed)}] lanes_failed=[{','.join(outcome.failed)}]"
            + (f" fallbacks={len(fallbacks)}" if fallbacks else ""))
        return 0 if not outcome.failed else 1

    # Exhausted every usable backend; all failed on infrastructure grounds.
    _write_summary(None, [], fallbacks, "NO_BACKEND_USABLE")
    log("\nAll candidate backends failed on infrastructure grounds:")
    for fb in fallbacks:
        log(f"  - {fb['backend']}: {fb['reason']}")
    ic.print_install_prompts(log)
    log("SUMMARY backend=none passed=0 failed=0 result=NO_BACKEND_USABLE "
        f"fallbacks={len(fallbacks)}")
    return 2


# ---- GUI (humans, Windows double-click) ----------------------------------
def gui_main(args) -> int:
    import threading
    import tkinter as tk
    from tkinter import scrolledtext, ttk

    root = tk.Tk()
    root.title("memsafe - build & validate")
    root.geometry("780x520")

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Label(top, text="Backend:").pack(side="left")
    backend_var = tk.StringVar(value="auto")
    ttk.OptionMenu(top, backend_var, "auto", "auto", "local", "wsl", "docker", "podman").pack(side="left", padx=4)

    out = scrolledtext.ScrolledText(root, height=24, wrap="word")
    out.pack(fill="both", expand=True, padx=8, pady=4)

    def log(msg=""):
        out.insert("end", str(msg) + "\n")
        out.see("end")
        out.update_idletasks()

    buttons = ttk.Frame(root, padding=8)
    buttons.pack(fill="x")

    def in_thread(fn):
        for b in buttons.winfo_children():
            b.config(state="disabled")

        def worker():
            try:
                fn()
            finally:
                for b in buttons.winfo_children():
                    b.config(state="normal")
        threading.Thread(target=worker, daemon=True).start()

    def do_detect():
        log("Detecting backends...")
        b = ic.detect_backends()
        for name in ic.AUTO_ORDER:
            ok, detail = b[name]
            log(f"  {name:7} {'USABLE' if ok else 'unusable'} - {detail}")
        log("")

    def do_prepare():
        log("Preparing backends...")
        for name in ic.AUTO_ORDER:
            ok, detail = ic.prepare_backend(name, log=log)
            log(f"  {name:7} {'READY' if ok else 'not ready'} - {detail}")
        log("")

    def do_run():
        ns = argparse.Namespace(backend=backend_var.get(), preset=None, image=args.image,
                                wsl_distro=args.wsl_distro, jobs=args.jobs,
                                list_backends=False, prepare_backends=False,
                                install_missing=True, yes_install=False,
                                step_timeout=args.step_timeout,
                                no_output_timeout=args.no_output_timeout)
        log(f"Running build + validate (backend={ns.backend})...")
        rc = cli_main(ns, log=log)
        log(f"\nFinished with exit code {rc}.")

    def do_clean():
        import clean
        clean.clean(log=log)

    ttk.Button(buttons, text="Detect backends", command=lambda: in_thread(do_detect)).pack(side="left", padx=4)
    ttk.Button(buttons, text="Prepare backends", command=lambda: in_thread(do_prepare)).pack(side="left", padx=4)
    ttk.Button(buttons, text="Run build + validate", command=lambda: in_thread(do_run)).pack(side="left", padx=4)
    ttk.Button(buttons, text="Clean", command=lambda: in_thread(do_clean)).pack(side="left", padx=4)
    ttk.Button(buttons, text="Quit", command=root.destroy).pack(side="right", padx=4)

    log("memsafe build & validate. Pick a backend and press 'Run build + validate'.")
    log("'auto' tries: " + ", ".join(ic.AUTO_ORDER) + ".\n")
    root.mainloop()
    return 0


def build_parser():
    p = argparse.ArgumentParser(description="Self-adjusting, generic build + validate runner.")
    p.add_argument("--backend", choices=["auto", "local", "wsl", "docker", "podman"], default="auto")
    p.add_argument("--preset", "-p", action="append",
                   help="Lane to run (repeatable). Names come from testing/infra_lanes.json.")
    p.add_argument("--image", default="ubuntu:24.04",
                   help="Default container image for docker/podman (lane file 'image' overrides). "
                        "Accepts a tag or a digest pin (image@sha256:...).")
    p.add_argument("--wsl-distro", default=None, help="WSL distro name (default: WSL default).")
    p.add_argument("--jobs", "-j", type=int, default=0, help="Parallel build jobs (0 = auto).")
    p.add_argument("--list-backends", action="store_true", help="Print backend + lane availability and exit.")
    p.add_argument("--prepare-backends", action="store_true",
                   help="Bring already-installed dormant backends to a reachable state, then exit.")
    p.add_argument("--install-missing", action="store_true", help="Print install prompts when no backend is usable.")
    p.add_argument("--yes-install", action="store_true", help="With --install-missing, attempt the install.")
    p.add_argument("--step-timeout", type=int, default=DEFAULT_STEP_TIMEOUT,
                   help="Per-command wall-clock guardrail in seconds (0 = none).")
    p.add_argument("--no-output-timeout", type=int, default=DEFAULT_NO_OUTPUT_TIMEOUT,
                   help="Kill a command after this many seconds with no output (0 = none).")
    p.add_argument("--no-gui", action="store_true", help="Never launch the GUI; force CLI mode.")
    return p


def main() -> int:
    if len(sys.argv) == 1 and ic.maybe_relaunch_gui(Path(__file__)):
        return 0
    args = build_parser().parse_args()
    if not args.jobs:
        import os
        args.jobs = max(1, os.cpu_count() or 1)
    args.step_timeout = args.step_timeout or None
    args.no_output_timeout = args.no_output_timeout or None
    if len(sys.argv) == 1 and not args.no_gui and ic.host_os() == "windows":
        try:
            return gui_main(args)
        except Exception:  # noqa: BLE001 - fall back to CLI if Tk unavailable
            pass
    return cli_main(args)


if __name__ == "__main__":
    sys.exit(main())
