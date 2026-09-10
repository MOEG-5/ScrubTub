# ScrubTub — video catalogue

A free desktop video catalogue for Windows and Linux. Add folders, browse
recursively discovered videos, scrub thumbnail previews, search and filter,
assign ratings and tags, and open videos in the system's default player.
Source media is never modified; the only explicit file operation is
Move-to-Trash, behind confirmation.

The application is in maintenance: focused fixes and small improvements.
Contributor and testing guidance lives in [AGENTS.md](AGENTS.md).

## License

GPL-3.0-only — see [LICENSE](LICENSE). Distributed derivatives must meet GPL
requirements; official binaries will be offered free of charge.

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

## Testing

Use the repository's `vids/` folder directly without copying or changing its
contents. Keep the test profile and generated caches outside it, and use an
isolated display for GUI automation. See [AGENTS.md](AGENTS.md) for the test rules.

For a Linux run inside that isolated session:

```sh
test_profile=$(mktemp -d /tmp/scrubtub-test.XXXXXX)
XDG_DATA_HOME="$test_profile/data" XDG_CACHE_HOME="$test_profile/cache" \
XDG_CONFIG_HOME="$test_profile/config" ./build/bin/scrubtub --add-root "$PWD/vids"
```

Native Qt Test suites live in `tests/`. Their legacy fixture setup still
copies/generates media, so inspect the relevant cases before running them;
those cases do not yet follow the current as-is testing rule.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | application sources (Qt Quick UI, catalogue, media workers) |
| `tests/` | Qt Test suites and fixture support |
| `docs/` | dependency manifest and historical archive |

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
