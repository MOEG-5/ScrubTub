# ScrubTub — video catalogue

A free desktop video catalogue for Windows and Linux. Add folders, browse
recursively discovered videos, scrub thumbnail previews, search and filter,
assign ratings and tags, and open videos in the system's default player.
Source media is never modified; the only explicit file operation is
Move-to-Trash, behind confirmation.

## License

GPL-3.0-only — see [LICENSE](LICENSE). Distributed derivatives must meet GPL
requirements; official binaries will be offered free of charge. Dependencies
retain their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).

## Building (Linux development)

Requirements (recorded development versions in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)):
CMake ≥ 3.28, a C++20 compiler, Qt 6.8+ (Core, Gui, Qml, Quick,
QuickControls2, Network, Test), SQLite 3, RapidFuzz C++ 3.3.4,
FFmpeg/ffprobe (on PATH at runtime; bundled copies for distribution).
mpv (tested with 0.41.0) provides fast live previews. If mpv is unavailable,
only cached previews are shown. Preview mpv uses its own private socket and ignores user
mpv configuration.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Sanitizer build used by the native contract checks:

```sh
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSCRUBTUB_SANITIZERS=ON
cmake --build build-san
```

## Self-contained builds

Private media builds and staging instructions are in [docs/RELEASING.md](docs/RELEASING.md).
Builds stay under `out/` and do not replace system tools. Release packaging
collects matching dependency sources and complete notices, verifies the inventory,
and produces paired binary/source downloads. GitHub workflows build Linux and
Windows artifacts; neither publishes a release automatically.

## Testing

Use the repository's `vids/` folder directly without copying or changing its
contents. Keep the test profile and generated caches outside it, and use an
isolated display for GUI automation; never automate the owner’s desktop session.
The local media corpus is not distributed. Corpus-dependent tests skip when it
is absent; use your own videos if you want to run those cases.

For a Linux run inside that isolated session:

```sh
test_profile=$(mktemp -d /tmp/scrubtub-test.XXXXXX)
XDG_DATA_HOME="$test_profile/data" XDG_CACHE_HOME="$test_profile/cache" \
XDG_CONFIG_HOME="$test_profile/config" ./build/bin/scrubtub --add-root "$PWD/vids"
```

Native Qt Test suites live in `tests/` and are registered with CTest:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCRUBTUB_TEST_VIDS_DIR="$PWD/vids"
cmake --build build
ctest --test-dir build --output-on-failure
```

Media-dependent suites scan `vids/` in place and never write to it; tests that
need to mutate media generate their own small clips in a temporary directory.
`tst_performance` and `tst_catalogueupdates` need no media at all and run
everywhere; they fence the large-library hot paths (model bookkeeping, fuzzy
search) against regressions.

### Performance knobs

ScrubTub uses every core it can for media work: probes, poster extraction and
storyboard generation run as independent single-threaded subprocesses, so the
pool size is set from the CPU count rather than threading one decode. Override
it when the machine is busy with something else:

```sh
SCRUBTUB_JOB_CONCURRENCY=8 ./build/bin/scrubtub
```

Hover previews use one private mpv process that is started on demand and shut
down after a period of inactivity, so an idle window costs nothing while the
first hover still gets a live frame in well under the dwell timeout. Cached
storyboard tiles cover the gap in the meantime. `SCRUBTUB_MPV_PATH` and
`SCRUBTUB_MPV_HWDEC` override the player binary and hardware decoding.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | application sources (Qt Quick UI, catalogue, media workers) |
| `tests/` | Qt Test suites and fixture support |
| `docs/` | dependency and release documentation |

## Interface

Use the sidebar to browse all videos, rated or unrated videos, unavailable
files, or a particular folder. Fuzzy search, include/exclude tags, and duration,
size, and rating ranges live in the sidebar. Enter precise bounds or drag either
slider handle; a blank maximum means no limit. Tag filters accept comma-separated
tags. Resolution and minimum views follow the range filters.
The view name and video count sit above the filters; the three-line sort button
opens ordering options. Drag the sidebar edge to resize it, or use its hide
button and the footer’s Show sidebar button. Ctrl+B toggles it; Ctrl+F reveals search.
Adjust thumbnail size with the slider below the grid. Wheel scrolling builds
momentum; trackpads retain native scrolling.

Hover a thumbnail to scrub its timeline. Click to open the bottom panel,
use Details, Tags, and File info to inspect or annotate it, and double-click
to open it in the default player. The tabs disappear when no video is selected;
Escape clears selection while the grid has focus.
Settings holds scan controls, folder maintenance, preview preferences,
and catalogue backups. Ctrl+F focuses search; Ctrl+, opens Settings.

Existing iTub catalogue profiles are reused in place, keeping ratings and tags.
New profiles use the ScrubTub name. Build options and mpv environment overrides
now use the `SCRUBTUB_` prefix.
