# Dependency manifest — development environment

Recorded 2026-09-06 on the reference development machine (see archive/BENCHMARKS.md for
historical hardware measurements). Application license: **GPL-3.0-only** (LICENSE).

| Component | Version (this machine) | License notes for distribution |
| --- | --- | --- |
| Qt 6 | 6.11.1 (Core, Gui, Qml, Quick, QuickControls2, Network, Test) | LGPL-3.0 open-source edition; dynamically linked; include licenses/notices and offer build materials per Qt licensing documentation |
| mpv | 0.41.0 (runtime hover previews) | Record the bundled build configuration and corresponding license/source materials |
| SQLite | 3.53.4 (system, via CMake `SQLite3`) | Public domain |
| RapidFuzz C++ | 3.3.4 (system package; CMake falls back to pinned GitHub tag `v3.3.4`, SHA-256 `a0dd2ef361cac165e12076696e7c7e8d069a2908abd9599ad4bd190de33f9881`) | MIT |
| FFmpeg / ffprobe | n9.0.1 (system; bundled copy for distribution must be built with documented flags) | This system build is configured `--enable-gpl --enable-version3` (GPLv3-compatible); a redistributable build must record its own configuration and license per FFmpeg legal guidance |
| CMake | 4.4.2 (build tool, ≥ 3.28 required) | Build-time only |
| Ninja | 1.13.2 (build tool) | Build-time only |
| Compilers tested | GCC 16.2.1, Clang 22.1.8 | Not distributed |

Distribution notes:

- The unmodified GPLv3 text ships as `LICENSE`; source files carry
  `SPDX-License-Identifier: GPL-3.0-only` headers.
- Distributed Qt modules are dynamically linked. Final release packages must
  include the used runtime components, required notices, and corresponding
  source/build materials. The release collectors generate that inventory from
  the actual staged files and reject missing source or notice material.
- The shipped FFmpeg build and its optional components are documented and
  license-checked separately from this development-machine table.
- No dependency is vendored into Git; the RapidFuzz FetchContent fallback pins
  an upstream release by URL and hash.

The private optimized media build pins FFmpeg 9.0.1, mpv 0.41.0, dav1d 1.5.4
and libplacebo 7.360.1. See [RELEASING.md](RELEASING.md) for the feature policy,
source collection, verification gates and platform workflows. The development
machine table above is not the Linux release dependency lock: Linux releases
use the isolated Debian builder and ship their exact `DEPENDENCIES.json`.
