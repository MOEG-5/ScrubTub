# Preparing a GitHub release

Repository: <https://github.com/MOEG-5/ScrubTub>.

Release packaging produces paired application and corresponding-source downloads,
a dependency inventory, full copyright/license notices, and SHA-256 checksums.
The packager refuses missing sources/notices, changed runtime files, and
unaccounted-for libraries. It does not publish a GitHub Release automatically.

## Linux build

Docker keeps the release toolchain and distribution libraries separate from the
owner's system. The repository is mounted read-only; output goes to a fresh
`out/` directory. Never use the repository root as the Docker build context.

```sh
docker build -f scripts/release/Dockerfile.linux -t scrubtub-release-builder scripts/release
mkdir -p out/release
docker run --rm --user "$(id -u):$(id -g)" \
  --mount "type=bind,src=$PWD,dst=/repo,readonly" \
  --mount "type=bind,src=$PWD/out/release,dst=/work" \
  scrubtub-release-builder bash /repo/scripts/release/build-linux-release.sh
```

The base Debian 13 image is pinned by digest. Exact installed package versions
are recorded with the source materials. Apt is used only inside the disposable
builder; the host's mpv, FFmpeg, ffprobe and other packages remain unchanged.
The resulting Linux build targets x86-64 with glibc 2.41 or newer. Do not describe
it as supporting all Linux distributions. The desktop session needs X11 or XWayland.

Outputs under `out/release/artifacts/`:

- `scrubtub-0.1.0-linux-x86_64.tar.gz`
- `scrubtub-0.1.0-linux-x86_64-sources.tar.gz`
- `SHA256SUMS.txt`
- `RELEASE_NOTES.md`, ready to use as the GitHub release description

Extract the whole application archive and run `bin/scrubtub`. Keep its `lib/`,
`qml/`, plugin directories and `bin/media/` together. Settings → About and licenses
opens the included notices directory.

## Private media build

`scripts/release/build-media.sh` builds under `out/media/` by default.
`SCRUBTUB_MEDIA_ROOT` selects another private build root; the release container
uses `/work/media`. No system prefix is modified. Dependencies needed to build
locally are listed explicitly in `Dockerfile.linux`; `SCRUBTUB_BUILD_JOBS`
controls concurrency (default eight).

| Component | Pinned version | Use |
| --- | --- | --- |
| FFmpeg / ffprobe | 9.0.1 | Metadata and JPEG preview extraction |
| mpv | 0.41.0 | Silent live scrubbing and PNG output |
| dav1d | 1.5.4 | Software AV1 decoding |
| libplacebo | 7.360.1 | Required mpv core dependency without optional GPU backends |
| RapidFuzz C++ | 3.3.4 | Header-only search code, included in the source inventory |

FFmpeg keeps broad built-in decoder, demuxer and parser support. Its only
encoders are MJPEG and PNG; its only muxers are image2/image2pipe. It retains
local file/pipe protocols and extraction filters. Video/audio encoding,
network protocols, capture devices and hardware acceleration are disabled.
Audio decoding/probing and basic resampling remain for metadata compatibility
and mpv core requirements; no optional audio-device backends are included.

mpv's optional window and audio outputs are disabled. LuaJIT is retained because
removing Lua removes the `--load-scripts`, `--osc` and `--ytdl` options ScrubTub
uses to isolate previews. Those features remain disabled at runtime, together
with user configuration. Mandatory libass/font dependencies remain intact.

All three tools share one set of FFmpeg libraries. SHA-256 checks cover the
private upstream downloads. Flags, versions and enabled encoder/decoder lists
are preserved in `private-media/build-manifests/` in the Linux source archive.

Tool discovery prefers an explicit `SCRUBTUB_FFMPEG_PATH`,
`SCRUBTUB_FFPROBE_PATH`, or `SCRUBTUB_MPV_PATH`, then `media/` beside the app, then
PATH for development. Overrides are authoritative even when invalid.

## Corresponding sources and notices

`collect-linux-compliance.py` matches each staged ELF to the installed package
by its build ID, and matches other package files by content hash. It downloads
the exact Debian **source version**, including the `.dsc`, upstream source,
Debian patches and build rules. It verifies the source members against the
`.dsc` SHA-256 entries. A differently patched upstream tarball is not accepted
as a replacement for a distribution source package.

The archive also includes private media sources/configurations and RapidFuzz,
which is compiled into the application and is not visible to `ldd`. Debian's
complete copyright files and referenced common license texts are copied into
`share/scrubtub/licenses/`. This includes embedded third-party notices in Qt.
`THIRD_PARTY.md` describes the principal licenses.

To rebuild a supplied Debian dependency, extract its `.dsc` with
`dpkg-source -x package.dsc`. Its `debian/` directory supplies patches, build
dependencies and build rules; use Debian's normal `dpkg-buildpackage` workflow.
The builder's installed-package inventory and apt source configuration are
included. Exact byte-for-byte reproducibility is not claimed.

`export-source.py` includes the current application source and release scripts,
including uncommitted changes, with a file hash manifest. It excludes Git
history and refuses tracked local media or build output. `verify-release.py`
checks the paired source tree against the runtime inventory before packaging.
The source archive must be uploaded alongside its binary and kept available
with it; a generic link to an upstream homepage is not the source companion.

The old `collect-media-sources.py` is a private-media helper used by the Windows
collector. Running that helper alone does not complete release packaging.

## Validation

The application suite uses `vids/` in place through `MediaFixtures` and checks
its fingerprint. Synthetic cases generate their own original clips through
`makeSyntheticVideo()` and use disposable caches/profiles. Keep a full developer
FFmpeg on PATH to generate those clips: the private runtime deliberately cannot
encode test videos.

`mediarelease` compares private probing against the developer ffprobe and
extracts posters across the corpus. `releasesmoke` generates an original clip
and checks probing, posters, storyboards and live scrubbing, so hosted CI has a
media test without uploading or copying the private corpus. The hosted jobs do
not have `vids/`; those corpus-specific cases skip explicitly.

The Linux builder runs the full CTest suite with the private-tool overrides,
plus release-gate tests and a packaged-app startup check in an isolated Xvfb display. The gate tests cover missing sources, altered binaries,
stripped notices and unaccounted-for executables. GUI checks require Xvfb or
another dedicated disposable session and a separate application profile.

## Windows

The manually dispatched `windows-validation.yml` workflow builds in MSYS2
UCRT64, targeting 64-bit Windows 10 (1809 or later) and Windows 11
([Qt platform requirements](https://doc.qt.io/qt-6/supported-platforms.html)). It stages Qt and the private tools, collects required DLLs, and checks
media executable startup with a Windows-only PATH. It runs the original-clip
smoke test, then maps staged files to their installed MSYS2 packages.

`collect-windows-compliance.py` downloads each exact, signed MSYS2 source-only
tarball and verifies its signature with the MSYS2 keyring. These archives contain
the upstream sources, patches and PKGBUILD recipes. Missing package notices or
unmapped files stop the workflow. When a binary package omits notices, the
collector reads license texts from its verified source archive; LuaJIT notices
come from the exact Git commit selected by its build recipe. The resulting ZIP and source tarball pass the
same inventory gate used on Linux before being uploaded as workflow artifacts.

A successful build and smoke test are required before offering a Windows
binary. Also validate the extracted application with a disposable catalogue on
Windows without development tools on PATH. The workflow deliberately does not
publish a GitHub Release by itself.

## Publishing

Use the generated `RELEASE_NOTES.md` for the exact artifact pair being uploaded.
Create the release from the corresponding reviewed source commit and attach the
binary, its platform-specific source archive and `SHA256SUMS.txt`. Keep each
platform's source companion; Windows and Linux dependency builds differ.
Do not reuse the earlier CachyOS validation archive as the release binary.

Upstream references: [FFmpeg licensing](https://ffmpeg.org/legal.html),
[mpv copyright](https://github.com/mpv-player/mpv/blob/v0.41.0/Copyright),
[Qt obligations](https://www.qt.io/development/open-source-lgpl-obligations), and
[MSYS2 source packages](https://www.msys2.org/wiki/Creating-Packages/).
