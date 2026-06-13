#!/usr/bin/env python3
"""Remove generated build directories and Python caches for the memsafe infra.

Generated content (CMake build trees, __pycache__) is NOT part of the source
deliverable; it is environment-specific and regenerated on every run. (SP3.)
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import infra_common as ic  # noqa: E402


def clean(log=print) -> int:
    root = ic.repo_root()
    removed = 0

    # CMake build trees (local + remote lanes) and the JSON/JUnit results.
    for base in [ic.testing_dir() / "build"]:
        if base.exists():
            log(f"removing {base}")
            shutil.rmtree(base, ignore_errors=True)
            removed += 1

    # Python __pycache__ anywhere under the tree.
    for cache in root.rglob("__pycache__"):
        if cache.is_dir():
            log(f"removing {cache}")
            shutil.rmtree(cache, ignore_errors=True)
            removed += 1

    log("clean: done" if removed else "clean: nothing to remove")
    return 0


if __name__ == "__main__":
    sys.exit(clean())
