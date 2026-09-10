# Benchmark results and methodology

## Milestone 0 — native paused-player seek feasibility (agent-run, small corpus)

**Question.** Can one shared `QMediaPlayer`/`QVideoSink` session deliver
suitably timed paused frames fast enough for the hover timeline
(TECH_SPEC.md section 11 gates: p95 ≤ 200 ms latency, delivered frame contains
the requested presentation time or is within 100 ms)?

**Environment.** CachyOS Linux (2026-09-06), Intel i5-8600K (6 logical CPUs),
15.4 GiB RAM, local SSD for fixture and build, integrated Intel UHD 630
graphics (VAAPI device present). Release build, GCC 16.2.1, Qt 6.11.1
(FFmpeg media backend), FFmpeg n9.0.1. Fixture: the supplied
`testvideo1.mp4` (H.264 1920×1080 50 fps, video-stream duration 667.5 s,
~30.8 MiB, read-only). Run headless (`QT_QPA_PLATFORM=offscreen`) — no window
system involved; GPU upload costs are therefore *not* included and are noted
as unmeasured.

**Historical command** (the Qt Multimedia benchmark was removed after choosing mpv):

```sh
./build/bin/hoverbench testvideo1.mp4 --repeats 2 \
    --out docs/bench/hoverbench-m0-run1.json
```

**Results** (targets 10, 300, 650, 123.456, 500 s × 2 repetitions;
runs 1 and 2 in `docs/bench/`):

| Metric | Value | Gate |
| --- | --- | --- |
| Seek accuracy (max \|delivered PTS − requested\|) | 20 ms (one frame; −16 ms at 123.456 s from ms rounding) | ≤ 100 ms — **PASS** |
| Delivered-frame PTS present | 10/10 seeks, both runs | required — **PASS** |
| Settled latency p50 | ~259–267 ms | — |
| Settled latency p95 | **358.5 / 360.2 ms** | ≤ 200 ms — **FAIL** |
| Latency by target | 10 s → ~250 ms, 300 s → ~142 ms, 650 s → ~258 ms, 123.456 s → ~356 ms, 500 s → ~346 ms | deterministic per position |
| Coalescing burst (12 rapid seeks @60 ms) | 4 frame deliveries, no decoder storm | desired behavior — PASS |
| Idle paused session | 0 frames in 2 s; no media I/O | PASS |
| Player-process RSS | ~330 MiB (Qt Multimedia + FFmpeg buffers, includes library overhead) | recorded; hover session is transient in the product |
| With `QT_FFMPEG_DECODING_HW_DEVICE_TYPES=vaapi` | p95 376.7 ms — no improvement | recorded |

**Interpretation.**

- Accuracy is excellent: the backend always delivered the frame *containing*
  the requested time (exact PTS start/end reported by `QVideoFrame`).
- Latency is deterministic and position-dependent — consistent with
  long-GOP forward decode from the previous keyframe, not random overhead.
  The fixture's GOP makes some positions cheap (142 ms) and others expensive
  (~360 ms). The p95 gate is missed by roughly 1.8× on this reference machine.
- No decoder thread knob is exposed by this Qt backend; VAAPI showed no
  benefit in the offscreen path. Both remain open to re-validation on the
  release target systems with a windowed RHI path.

**Conclusions for the product.**

1. Native paused seeking is *feasible and frame-accurate*; it is *not*
   within the p95 latency gate for this media class on this machine.
2. Per the owner's decision (PLAN.md), the catalog ships with instant
   cached-storyboard scrubbing as the default and may keep the paused-player
   path for media/positions where it meets the budget (hybrid), unless the
   owner prefers cached-only. The cached fallback now has concrete benchmark
   evidence: 231–515 ms per fresh FFmpeg-process seek (TECH_SPEC.md section 11
   preliminary measurement) and 142–360 ms per persistent-player seek — both
   are far too slow for pointer-following without cached samples.
3. `QMediaPlayer::duration()` reported 669,360 ms — the **subtitle** stream's
   duration. This confirms TECH_SPEC.md's rule: the catalogue stores the
   selected video stream's duration from ffprobe, and hover targets clamp to
   that duration, never to QMediaPlayer's container/subtitle-derived value.
4. Idle behavior is clean (no frames, no I/O), and the backend coalesces
   rapid seeks on its own; the product still applies its own 60 ms input
   coalescing and one-pending-target rule.

**Unmeasured here (owner/system validation).** GPU upload and rendering costs
with a real RHI/window path, cold-cache first hover, other media classes
(HEVC/VP9/AV1, 4K, VFR), Windows behavior, and network storage. These belong
to the owner-run gates in TECH_SPEC.md section 11.

## Milestone follow-up — mpv hover backend (2026-09-06)

**Correction:** the [live-seek investigation](LIVESEEK_INVESTIGATION.md)
reproduced working paused keyframe output in 12.5–17.1 ms with the original
mpv arguments. The no-repaint claim below is superseded. The original implementation
waited 220 ms before starting an exact seek and hid cached tiles in live
mode, so the following historical description overstates motion feedback.

After the milestone-0 verdict (QMediaPlayer paused seeks accurate but slow),
the owner pointed at the thumbfast technique (po5/thumbfast, MPL-2.0 — studied
for its approach, not copied). scrubtub now ships an mpv-backed hover session:
one persistent `mpv --no-config --idle --pause` subprocess per engaged card,
driven over its JSON IPC socket, writing the paused frame as raw BGRA to an
app-owned file that the session polls. Measured on the same reference machine
and fixture:

| Path | Latency (pointer settle → frame) |
| --- | --- |
| mpv exact seek + frame file update | **~185–260 ms** |
| QMediaPlayer exact seek (previous backend) | 250–360 ms (p95 358–377 ms) |
| Cached storyboard tile (default during motion) | ~0 ms (no source reads) |

Findings recorded along the way:

- mpv IPC is line-delimited JSON; omitting the trailing newline silently
  buffers commands forever (cost most of a debugging session).
- With the paused encoder (`--o=`), **keyframe-accurate seeks
  (`absolute+keyframes`) do not repaint**; exact seeks do. The session
  therefore seeks only on pointer settle (220 ms coalescing) and the cached
  storyboard carries the pointer-in-motion feedback — matching the spec's
  cached-fallback design exactly.
- `QMediaPlayer::duration()` reports the fixture's subtitle duration
  (669 360 ms); the hover session clamps to the ffprobe video-stream
  duration (667 500 ms) stored in the catalogue.

mpv is required for live previews; cached previews are used when it is unavailable.

## Live-seek correction — persistent process (2026-09-06)

The product now sends keyframe seeks during motion (at most every 50 ms),
then requests exact refinement after 100 ms without a changed target. One
seek is in flight with a replaceable latest target. Superseded exact frames
are discarded. The initial hover position is passed with file loading;
posters/cached samples remain visible until a matching live frame arrives.

One idle mpv process is prewarmed and reused across hovers. `stop` unloads
sources on leave without killing the process. The old `--o` encoder could
not reinitialize video output after stop/loadfile on mpv 0.41; native
`--vo=image` with uncompressed PNG output supports this lifecycle. Images
are consumed after `playback-restart`, paired with mpv's paused `time-pos`,
and deleted. There is no frame-file polling or blocking IPC connection wait.
IPC uses an owner-only temporary `scrubtub-hover-…/ipc.sock` directory (a unique
named pipe on Windows), never the desktop player's default socket. User
mpv configuration/scripts and media-controls integration are disabled.

**Measured through the updated HoverSession**, same supplied H.264 fixture,
Release build, Qt offscreen, software decoding, warm file cache, no catalogue
background jobs. Five observations, not a codec-wide percentile claim:

| Target (seconds) | First moving preview (ms) | Exact refinement (ms from scrub) |
| --- | --- | --- |
| 10 | 30 | 344 |
| 300 | 40 | 223 |
| 650 | 30 | 457 |
| 123.456 | 32 | 587 |
| 500 | 30 | 345 |

A repeat run measured moving previews at 30–51 ms and exact refinement at
232–425 ms. The ranges vary with system load; the table retains the first
run rather than selecting only the fastest measurements.

The original HoverSession took 445–568 ms to show any seek update and
produced no frames during a 600 ms motion burst. Moving previews now meet
the 200 ms gate on this fixture. **Exact refinement still misses that gate**;
keyframe previews are approximate and show their actual sampled position.
An initial un-prewarmed engagement measured 236 ms to its first preview,
excluding the QML 120 ms dwell. Unloading still requires reopening the
source/decoder on re-hover, though the mpv process remains alive.

The supplied thumbfast configuration's hardware mode was also compared:
`auto`/`auto-copy` selected Vulkan copy on this machine and did not reliably
improve these CPU-readable previews. Software decoding remains the default;
`SCRUBTUB_MPV_HWDEC=auto-copy ./build/bin/scrubtub` enables an owner comparison.
Thumbfast's small demux buffer, disabled readahead, two decoding threads,
fast scaler, and skipped loop filtering are used.

Validation: `ctest --test-dir build -R '^(hoversession|preview)$' -V`
checks motion progress, latest-target settling, actual quantized timestamps,
initial position, process reuse, private socket arguments, source switching,
file-handle release, cached-only mode, and missing-mpv cached fallback. A disposable Xvfb
session with one fixture copy also verified stationary initial hover,
changing positions during motion, unload on leave, and re-hover with the
same process. Additional GUI checks verified one-second keyboard seeking
with a visible details preview and that cached-only mode stops mpv. An
offscreen QML pixel test verifies that hovering selects a single atlas tile.
Atlas cropping uses a clipped/translated image:
`sourceClipRect` did not crop textures from the async image provider.
No real desktop/profile/library was used. Windows, other
codecs, cold storage, and a full thumbfast UI comparison remain unmeasured.

## Owner-run large-library benchmark instructions

The synthetic 10k/100k generator is **not yet implemented** (the agent corpus
limit forbids running it, and implementation time went to the feature set).
Owner validation options today:

1. **Point the app at a large real library** on a separate profile and
   measure: cold/warm startup, search input-to-settled (p50/p95/p99 over a
   personal query set), scrolling frame times (e.g. `QSG_RENDER_TIMING=1`),
   steady RSS, cancel-scan acknowledgment, unchanged-rescan ffprobe count.
   Export only aggregate metrics; never share filenames/tags/paths.
2. **Synthetic generator (to implement):** a small CLI that fills a second
   catalogue with fictional rows (varied Unicode names, tag distributions,
   durations) and replays ≥100 queries spanning short/common/typo cases per
   §11. Suggested location `bench/synth.cpp`, wired into the existing
   `QuerySpec` path. The 10k design target and the `ponytail:` marker in
   `CatalogueM3.cpp` (worker-side O(N) scan) are the agreed evaluation point.

Definition of cold vs warm for owner runs: cold = fresh process with the
thumbnail cache present but OS page cache dropped for the media volume
(no cache flushes on the active profile); warm = second run without drops.
Record CPU/storage/GPU/OS/toolchain and power mode with every table.
