# Third-Party Notices

This repository bundles third-party software. Each component listed below is the property of its
respective copyright holders and is distributed under its own license, which is reproduced in full
here and retained alongside the component in the source tree.

> The memory-safe C++ library itself — the header-only code under `include/memsafe/` — has **no
> third-party dependencies** and links against nothing but the C++ standard library. The component
> below is used **only** by the optional concurrency **test** lane and is never part of the
> distributed library.

---

## Relacy Race Detector

| | |
|---|---|
| **Component** | Relacy Race Detector (header-only) |
| **Copyright** | Copyright (c) 2008–2013, Dmitry S. Vyukov. All rights reserved. |
| **License** | BSD 3-Clause ("modified BSD") — full text below |
| **Upstream** | https://github.com/dvyukov/relacy · https://www.1024cores.net/home/relacy-race-detector |
| **Vendored snapshot** | commit `c0637790d8dd17a5ecd4d05cb0912ab9e2ef9554` (2026-05-19, "issue 49: relaxed wait") |
| **Location** | `testing/tests/concurrency/vendor/relacy/` |
| **Original license file** | `testing/tests/concurrency/vendor/relacy/LICENSE` (retained) |

**How it is used.** Relacy is included only by `testing/tests/concurrency/*.cpp` in the optional
`concurrency` test lane (enabled with `MEMSAFE_ENABLE_CONCURRENCY_TESTS=1`). It exhaustively
explores thread interleavings for the lock-free `SlotMap` and the `Sync*`/`Arc`/`Mutex` types. It is
a **test-only** dependency: it is not referenced by any header in `include/memsafe/`, is not required
to use the library, and is excluded from the default build.

### License

```text
Relacy Race Detector
Copyright (c) 2008-2013, Dmitry S. Vyukov
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
  - Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
  - Redistributions in binary form must reproduce the above copyright notice, this list of conditions
    and the following disclaimer in the documentation and/or other materials provided with the distribution.
  - The name of the owner may not be used to endorse or promote products derived from this software
    without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE OWNER "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE OWNER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
