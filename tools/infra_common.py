#!/usr/bin/env python3
"""Shared helpers for the memsafe self-adjusting build/validate tools.

Backend detection (local toolchain / WSL / Docker / Podman), OS-specific install
prompts, backend preparation, command running with guardrails, a versioned JSON
result writer, and Windows double-click -> GUI relaunch. Adapted in spirit from
the AI SSTL reference tooling (sstl_tool_common.py).

The module is GENERIC: it contains no library-specific type names, headers, or
test names. Everything library-specific lives code-side in testing/infra_lanes.json
and in the test sources themselves.
"""
from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


# ---- Versioning ----------------------------------------------------------
# RESULT_SCHEMA_VERSION is an integer owned by the JSON result contract. It
# increments ONLY for incompatible changes to summary.json so AI/CI consumers
# can reject an unknown breaking schema instead of duck-typing. (SP5.)
RESULT_SCHEMA_VERSION = 1
# TOOL_VERSION records the runner implementation version. "0+local" marks an
# unreleased/working-tree build; a release process can stamp a real version.
TOOL_VERSION = "0+local"

# Sentinel printed by the remote (WSL/container) script when, after a best-effort
# toolchain bootstrap, no C++ toolchain is reachable. Used to classify the
# failure as backend-infrastructure (fall back) rather than validation (stop).
SENTINEL_NO_TOOLCHAIN = "INFRA_NO_TOOLCHAIN"


# ---- Command result ------------------------------------------------------
@dataclass
class RunResult:
    code: int
    output: str

    @property
    def ok(self) -> bool:
        return self.code == 0


# ---- Paths ---------------------------------------------------------------
def repo_root() -> Path:
    """run-test-infra/ : the parent of this tools/ directory."""
    return Path(__file__).resolve().parent.parent


def testing_dir() -> Path:
    return repo_root() / "testing"


def win_to_wsl(path: Path) -> str:
    """Translate C:\\a\\b -> /mnt/c/a/b for use inside WSL."""
    s = str(path)
    if len(s) >= 2 and s[1] == ":":
        drive = s[0].lower()
        rest = s[2:].replace("\\", "/")
        if not rest.startswith("/"):
            rest = "/" + rest
        return f"/mnt/{drive}{rest}"
    return s.replace("\\", "/")


# ---- Command running (with guardrails) -----------------------------------
def run(cmd: List[str], cwd: Optional[Path] = None,
        log: Optional[Callable[[str], None]] = None,
        timeout: Optional[float] = None,
        no_output_timeout: Optional[float] = None) -> RunResult:
    """Run a command, streaming + capturing combined output.

    Guardrails (so the runner can never hang indefinitely):
      * timeout            : hard wall-clock limit for the whole command.
      * no_output_timeout  : kill if no new output line arrives for this long.
    Either may be None to disable. Returns RunResult(code, captured_output).
    """
    emit = log or print
    emit("+ " + " ".join(str(c) for c in cmd))
    try:
        proc = subprocess.Popen(
            cmd, cwd=str(cwd) if cwd else None,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", bufsize=1)
    except FileNotFoundError as exc:
        emit(f"  (command not found: {exc})")
        return RunResult(127, "")
    except OSError as exc:
        emit(f"  (could not start: {exc})")
        return RunResult(126, "")

    captured: List[str] = []
    last_output = [time.monotonic()]
    start = time.monotonic()
    done = threading.Event()

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            captured.append(line)
            last_output[0] = time.monotonic()
            emit(line.rstrip("\n"))
        done.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    killed = None
    while not done.wait(0.2):
        now = time.monotonic()
        if timeout is not None and (now - start) > timeout:
            killed = f"exceeded timeout of {timeout:.0f}s"
        elif no_output_timeout is not None and (now - last_output[0]) > no_output_timeout:
            killed = f"no output for {no_output_timeout:.0f}s"
        if killed:
            emit(f"  (guardrail: {killed}; terminating sub-process)")
            _terminate(proc)
            break

    proc.wait()
    t.join(timeout=2)
    out = "".join(captured)
    if killed:
        return RunResult(proc.returncode if proc.returncode not in (0, None) else 124, out)
    return RunResult(proc.returncode if proc.returncode is not None else 1, out)


def _terminate(proc: "subprocess.Popen") -> None:
    try:
        proc.terminate()
        try:
            proc.wait(timeout=5)
            return
        except Exception:  # noqa: BLE001
            pass
        proc.kill()
    except Exception:  # noqa: BLE001
        pass


def find(name: str) -> Optional[str]:
    return shutil.which(name)


# ---- Backend probing -----------------------------------------------------
def host_os() -> str:
    s = platform.system().lower()
    if s.startswith("win"):
        return "windows"
    if s == "darwin":
        return "macos"
    return "linux"


def find_compiler() -> Optional[str]:
    for c in ("c++", "g++", "clang++", "cl", "clang-cl"):
        p = find(c)
        if p:
            return p
    return None


def find_generator() -> Optional[str]:
    """CMake -G generator name, or None to let CMake pick its default."""
    if find("ninja"):
        return "Ninja"
    return None


def local_usable() -> Tuple[bool, str]:
    if not find("cmake"):
        return False, "cmake not found on PATH"
    if not find("ctest"):
        return False, "ctest not found on PATH"
    comp = find_compiler()
    if not comp:
        return False, "no C++ compiler found (c++/g++/clang++/cl)"
    return True, f"cmake + {Path(comp).name}"


@lru_cache(maxsize=1)
def ctest_supports_junit() -> bool:
    """Best-effort probe: older CTest versions may not have --output-junit.

    JUnit is a useful CI artifact, but it must not make an otherwise usable
    backend fail. The runner adds --output-junit only when the target CTest
    advertises support.
    """
    if not find("ctest"):
        return False
    try:
        proc = subprocess.run(["ctest", "--help"], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True,
                              errors="replace", timeout=20)
    except Exception:  # noqa: BLE001
        return False
    return "--output-junit" in (proc.stdout or "")


def container_engine_reachable(engine: str, timeout: int = 20) -> Tuple[bool, str]:
    if not find(engine):
        return False, f"{engine} not found on PATH"
    try:
        proc = subprocess.run([engine, "info"], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True,
                              errors="replace", timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        return False, f"{engine} info failed: {exc}"
    if proc.returncode == 0:
        return True, f"{engine} engine reachable"
    return False, f"{engine} installed but engine/daemon not reachable"


# Internal/system distros that are not valid Linux validation targets.
INTERNAL_WSL = ("docker-desktop", "podman-machine", "rancher-desktop")
# Substrings that mark a `wsl --list` line as a diagnostic, not a distro name.
WSL_DIAGNOSTIC_MARKERS = (
    "access is denied", "denied", "error", "wsl/", "0x", "not installed",
    "no installed", "usage:", "the system cannot", "operation",
)


def _looks_like_distro_name(name: str) -> bool:
    """A plausible distro name: short, no sentence punctuation, no diagnostics."""
    low = name.lower()
    if any(m in low for m in WSL_DIAGNOSTIC_MARKERS):
        return False
    if any(m in low for m in INTERNAL_WSL):
        return False
    # Distro names do not contain '.', ':', or end with sentence punctuation,
    # and have no internal spaces beyond a couple of words (e.g. "Ubuntu-22.04"
    # is allowed; "Access is denied." is not because it contains "denied").
    if name.endswith(".") or name.endswith(":"):
        return False
    if len(name) > 64 or len(name.split()) > 3:
        return False
    return True


def wsl_distros() -> List[str]:
    """Return user-installable WSL distros, validating the exit code AND names.

    `wsl --list --quiet` is the documented enumeration command, but WSL emits
    diagnostics (e.g. "Access is denied.") to stdout on failure, so a text-only
    parser that ignores the return code can accept an error string as a distro
    name. We require a zero exit code and name plausibility. (SP8.)
    """
    if not find("wsl") or host_os() != "windows":
        return []
    try:
        proc = subprocess.run(["wsl", "--list", "--quiet"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20)
    except Exception:  # noqa: BLE001
        return []
    if proc.returncode != 0:
        return []
    out = proc.stdout or b""
    text = out.decode("utf-16le", "ignore") if b"\x00" in out else out.decode("utf-8", "replace")
    names = []
    for ln in text.replace("\x00", "").splitlines():
        n = ln.strip().replace("\r", "")
        if n and _looks_like_distro_name(n):
            names.append(n)
    return names


def wsl_launch_ok(distro: Optional[str], timeout: int = 60) -> bool:
    """No-op launch probe: prove the distro actually starts and runs a command.

    Microsoft documents `wsl --distribution <name>` to run a specific distro;
    running `true` exits 0 only if the distro is genuinely reachable. (SP8.)
    """
    cmd = ["wsl"] + (["-d", distro] if distro else []) + ["--", "true"]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=timeout)
    except Exception:  # noqa: BLE001
        return False
    return proc.returncode == 0


def wsl_usable() -> Tuple[bool, str]:
    if host_os() != "windows":
        return False, "WSL is only available on Windows"
    if not find("wsl"):
        return False, "wsl.exe not found"
    distros = wsl_distros()
    if not distros:
        return False, "no usable WSL distro installed (or wsl --list failed)"
    target = distros[0]
    if not wsl_launch_ok(target):
        return False, f"WSL distro '{target}' did not start (launch probe failed)"
    return True, f"WSL distro: {target} (launch probe OK)"


def detect_backends() -> Dict[str, Tuple[bool, str]]:
    return {
        "local": local_usable(),
        "wsl": wsl_usable(),
        "docker": container_engine_reachable("docker"),
        "podman": container_engine_reachable("podman"),
    }


AUTO_ORDER = ("local", "wsl", "docker", "podman")


# ---- Failure classification (SP6) ----------------------------------------
# Conservative: only well-known backend-infrastructure signals classify as
# "infra" (eligible for auto fallback). Anything else on a backend that passed
# preflight is a validation failure (real code defect) and must STOP the run.
_CONTAINER_INFRA_PATTERNS = (
    "cannot connect to the docker daemon", "is the docker daemon running",
    "error during connect", "manifest unknown", "manifest for",
    "pull access denied", "repository does not exist", "no such host",
    "error pulling image", "failed to resolve reference",
    "cannot connect to podman", "unable to connect to podman",
    "short-name resolution", "connection refused", "network is unreachable",
)
_WSL_INFRA_PATTERNS = (
    "access is denied", "is not installed", "wsl/service",
    "the system cannot find", "0x8",
)


def classify_failure(backend: str, result: RunResult) -> str:
    """Return 'infra' (fall back) or 'validation' (stop). (SP6.)"""
    out = (result.output or "").lower()
    if SENTINEL_NO_TOOLCHAIN.lower() in out:
        return "infra"
    if backend in ("docker", "podman"):
        if any(p in out for p in _CONTAINER_INFRA_PATTERNS):
            return "infra"
    if backend == "wsl":
        if any(p in out for p in _WSL_INFRA_PATTERNS):
            return "infra"
    return "validation"


# ---- Backend preparation (--prepare-backends, SP / Q3) -------------------
def prepare_backend(name: str, log: Callable[[str], None] = print) -> Tuple[bool, str]:
    """Bring an already-installed but dormant backend to a reachable state.

    NEVER installs software (that is --install-missing --yes-install). Starts
    Docker Desktop, `podman machine start`, or wakes the default WSL distro.
    """
    if name == "local":
        ok, detail = local_usable()
        return ok, detail
    if name == "wsl":
        if host_os() != "windows" or not find("wsl"):
            return False, "WSL not available on this host"
        # A no-op launch wakes the default distro.
        ok = wsl_launch_ok(None)
        return (ok, "WSL default distro woken" if ok else "WSL default distro did not start")
    if name in ("docker", "podman"):
        ok, detail = container_engine_reachable(name)
        if ok:
            return True, detail
        if name == "podman":
            if not find("podman"):
                return False, "podman not installed"
            log(f"+ {name} machine start")
            run(["podman", "machine", "start"], log=log, timeout=180)
            return container_engine_reachable("podman")
        # docker: try to start Docker Desktop on Windows/macOS.
        if not find("docker"):
            return False, "docker not installed"
        started = _start_docker_desktop(log)
        if not started:
            return False, "could not start Docker Desktop automatically"
        # Poll for the daemon to come up.
        for _ in range(30):
            ok, detail = container_engine_reachable("docker")
            if ok:
                return True, detail
            time.sleep(2)
        return False, "Docker Desktop started but daemon not reachable yet"
    return False, f"unknown backend '{name}'"


def _start_docker_desktop(log: Callable[[str], None] = print) -> bool:
    osname = host_os()
    try:
        if osname == "windows":
            candidates = [
                os.path.expandvars(r"%ProgramFiles%\\Docker\\Docker\\Docker Desktop.exe"),
                os.path.expandvars(r"%LocalAppData%\\Docker\\Docker Desktop.exe"),
            ]
            for c in candidates:
                if Path(c).exists():
                    log(f"+ launching {c}")
                    subprocess.Popen([c], close_fds=True)
                    return True
            return False
        if osname == "macos":
            log("+ open -a Docker")
            subprocess.Popen(["open", "-a", "Docker"], close_fds=True)
            return True
        # Linux: try to start the service (best effort, may need privileges).
        log("+ systemctl --user start docker (best effort)")
        subprocess.run(["systemctl", "--user", "start", "docker"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception:  # noqa: BLE001
        return False


# ---- JSON result writer (SP5) --------------------------------------------
def write_summary_json(path: Path, payload: dict) -> None:
    import json
    path.parent.mkdir(parents=True, exist_ok=True)
    body = dict(payload)
    body.setdefault("schema_version", RESULT_SCHEMA_VERSION)
    body.setdefault("tool_version", TOOL_VERSION)
    path.write_text(json.dumps(body, indent=2, sort_keys=False), encoding="utf-8")


# ---- Install prompts (printed when no backend is usable) -----------------
@dataclass
class InstallOption:
    label: str
    commands: List[str] = field(default_factory=list)


def install_options() -> List[InstallOption]:
    osname = host_os()
    if osname == "windows":
        return [
            InstallOption("Local toolchain (winget)", [
                "winget install -e --id Kitware.CMake",
                "winget install -e --id Ninja-build.Ninja",
                "winget install -e --id LLVM.LLVM",
            ]),
            InstallOption("WSL (Ubuntu)", [
                "wsl --install -d Ubuntu",
                "wsl -d Ubuntu -- sudo apt-get update",
                "wsl -d Ubuntu -- sudo apt-get install -y cmake ninja-build g++ clang",
            ]),
            InstallOption("Docker Desktop", ["winget install -e --id Docker.DockerDesktop"]),
            InstallOption("Podman", ["winget install -e --id RedHat.Podman",
                                      "podman machine init", "podman machine start"]),
        ]
    if osname == "macos":
        return [
            InstallOption("Local toolchain (Homebrew)", ["brew install cmake ninja llvm"]),
            InstallOption("Docker / Podman", ["brew install --cask docker",
                                               "brew install podman",
                                               "podman machine init", "podman machine start"]),
        ]
    return [
        InstallOption("Local toolchain (apt)", [
            "sudo apt-get update",
            "sudo apt-get install -y cmake ninja-build g++ clang",
        ]),
        InstallOption("Local toolchain (dnf)", [
            "sudo dnf install -y cmake ninja-build gcc-c++ clang",
        ]),
        InstallOption("Docker / Podman", ["sudo apt-get install -y docker.io",
                                          "sudo apt-get install -y podman"]),
    ]


def print_install_prompts(log: Callable[[str], None] = print) -> None:
    log("")
    log("No usable backend was found. Install ONE of the following, then re-run:")
    for opt in install_options():
        log("")
        log(f"  # {opt.label}")
        for c in opt.commands:
            log(f"    {c}")
    log("")


def run_install(log: Callable[[str], None] = print) -> int:
    opts = install_options()
    if not opts:
        return 1
    opt = opts[0]
    log(f"Attempting: {opt.label}")
    rc = 0
    for c in opt.commands:
        rc = run(c.split(), log=log, timeout=1800).code or rc
    return rc


# ---- Windows double-click -> GUI relaunch --------------------------------
def launched_from_terminal() -> bool:
    try:
        return bool(sys.stdin and sys.stdin.isatty())
    except Exception:  # noqa: BLE001
        return False


def maybe_relaunch_gui(script: Path) -> bool:
    """On Windows, if double-clicked (no args, no console), relaunch under
    pythonw so the Tkinter GUI shows instead of a flashing console."""
    if host_os() != "windows" or len(sys.argv) > 1:
        return False
    if Path(sys.executable).name.lower() == "pythonw.exe":
        return False
    if launched_from_terminal():
        return False
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    if not pythonw.exists():
        return False
    try:
        subprocess.Popen([str(pythonw), str(script.resolve())], close_fds=True)
        return True
    except Exception:  # noqa: BLE001
        return False
