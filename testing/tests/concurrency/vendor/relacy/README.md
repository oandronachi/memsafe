<!--
Purpose: Document the vendored Relacy Race Detector snapshot used by the memsafe concurrency tests.
Work package: CPP_MEMSAFE-0430-TEST, satisfying the Relacy/header-use rework for sync ownership.
Key invariants:
- The files under relacy/ are an upstream dvyukov/relacy header snapshot, not a local compatibility shim.
- The snapshot is used only by TEST artifacts in the optional concurrency lane.
- The upstream LICENSE file is retained in this directory and governs redistribution.
-->

# Relacy Vendor Snapshot

This directory contains a vendored header snapshot of Dmitry Vyukov's Relacy Race Detector from upstream dvyukov/relacy commit c0637790d8dd17a5ecd4d05cb0912ab9e2ef9554 (2026-05-19, "issue 49: relaxed wait").

The sync concurrency test includes `relacy/relacy.hpp` from this directory and runs Relacy's full scheduler over the two-thread `SyncOwner` borrow/borrow_mut model. There is no README-only or local-shim pass path for CPP_MEMSAFE-0430-TEST.

The upstream license is retained as `LICENSE` in this directory.
