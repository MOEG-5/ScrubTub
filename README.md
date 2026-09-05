# itub — video catalogue (working title)

A free desktop video catalogue for Windows and Linux. Add folders, browse
recursively discovered videos, scrub thumbnail previews, search and filter,
assign ratings and tags, and open videos in the system's default player.
Source media is never modified; the only explicit file operation is
Move-to-Trash, behind confirmation.

Status: implementation in progress per [PLAN.md](PLAN.md) and
[TECH_SPEC.md](TECH_SPEC.md).

## License

GPL-3.0-only — see [LICENSE](LICENSE). Distributed derivatives must meet GPL
requirements; official binaries will be offered free of charge.

## Building (Linux development)

Requirements (exact pinned versions in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)):
CMake ≥ 3.28, a C++20 compiler, Qt 6.8+ (Core, Gui, Qml, Quick,
QuickControls2, Multimedia, Test), SQLite 3, RapidFuzz C++ 3.3.4, FFmpeg/ffprobe.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Sanitizer build used by the native contract checks:

```sh
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DITUB_SANITIZERS=ON
cmake --build build-san
```

## Tests and benchmarks

```sh
ctest --test-dir build --output-on-failure     # agent-scale checks (≤20-entry corpus)
./build/bin/hoverbench testvideo1.mp4 10 300 650 123.456 500   # paused-player seek benchmark
```

Agent test runs create their own disposable fixtures and profiles inside the
build directory; they never touch the repository root fixture or any real
library. Large-scale (10k/100k) benchmark modes are prepared for the owner and
documented in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Layout

| Path | Contents |
| --- | --- |
| `src/` | application sources (Qt Quick UI, catalogue, media workers) |
| `bench/` | standalone benchmark executables |
| `tests/` | Qt Test suites and fixture support |
| `docs/` | dependency manifest, benchmark results, release notes |
