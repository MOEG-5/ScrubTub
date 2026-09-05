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

**Command** (repeatable; tool `bench/hoverbench.cpp`):

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

## Owner-run large-library benchmark instructions

Prepared commands (do **not** run in agent tools; see TECH_SPEC.md sections
11 and 12 for the full protocol):

- Synthetic catalogue: `itub-bench synth --entries 10000 --queries 100`
  *(arrives with the catalogue implementation in milestone 1+; the tool is
  agent-built but owner-executed against private or synthetic large sets)*
- Real-library validation: choose a root and profile explicitly, export only
  aggregate metrics (counts, p50/p95/p99, memory). No filenames, tags, or
  paths in shared results.

Startup/search/scrolling sequences, cold/warm definitions, and the 60-second
scrolling trace protocol are defined in TECH_SPEC.md section 11 and recorded
in BENCHMARKS.md as they are implemented.
