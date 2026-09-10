# Implementation handoff — ScrubTub video catalogue

Status: agent implementation handoff, 2026-09-06. Product behavior follows
[TECH_SPEC.md](TECH_SPEC.md); direction and milestone order follow
[PLAN.md](PLAN.md). All agent-run results below use the allowed disposable
corpus (≤20 derived media files, ≤1 GiB, fresh profiles). **Owner-run
large-library gates (10k/100k) remain pending and must not be treated as
passed.**

## What runs today

| Area (TECH_SPEC §1) | State |
| --- | --- |
| Roots | Add/rescan/pause/resume/cancel; duplicate and overlapping roots rejected; symlinked roots rejected; offline roots keep prior availability |
| Browsing | Virtual grid (reuseItems), posters, card metadata, details panel with path/size/duration/resolution/codec/views/status/tags |
| Preview | Cached storyboard hover (instant, zero source reads) + shared paused-player session with dwell/coalescing/PTS validation; Auto and Cached-only modes |
| Playback | Open in default player (`QDesktopServices`), accepted-handoff view counting, keyboard activation |
| Ratings | 0–5 stars, persisted, star controls with accessible names |
| Tags | Filename/folder/technical auto tags per §7 table, manual add/remove, suppressions, reset, atomic regeneration, provenance markers |
| Search | Debounced live fuzzy search (RapidFuzz Levenshtein, class-ranked relevance), token AND, diacritic folding, visible validation (256 chars/16 tokens) |
| Filters | Size, width/height, resolution presets (shorter-side buckets), duration, rating/unrated, tags any/all/excluded, availability, views |
| Sorting | All §1 keys with stable ID tie-breaker and unknowns-last |
| Safety | Read-only scanning verified by tree-snapshot checks; Trash-only mutation with identity re-check and no permanent-delete fallback; DB/cache in app-owned profile dirs |
| Persistence | Catalogue, ratings/tags/views, roots, jobs, and UI settings restore on restart |

## How to build and test (Linux dev machine)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # 8 suites, ~65 s
./build/bin/scrubtub                               # the app
```

Test suites: `filesafety` (no-source-writes, symlinks, harness),
`probe` (stream selection, subtitle-duration trap, timeout),
`catalogue` (scan/reconciliation/annotations/offline/lock),
`extract` (poster/storyboard/rotation/truncation), `preview` (provider),
`hoversession` (paused seeks, coalescing, cached-only),
`search` (§7/§8 tables, pagination, suppressions), `fileops`
(trash/backup/restore/cache). GUI smoke runs under a disposable Xvfb
display per AGENTS.md.

## Measured results (agent corpus, this machine)

Machine: i5-8600K (6 logical CPUs), 15.4 GiB RAM, CachyOS, GCC 16.2.1,
Qt 6.11.1 (FFmpeg backend), FFmpeg n9.0.1. Details in
[BENCHMARKS.md](BENCHMARKS.md).

- **Paused-player seek benchmark (milestone 0):** accuracy PASS (delivered
  frame contains the requested PTS; ≤20 ms), **latency p95 358–377 ms FAILS
  the ≤200 ms gate** (keyframe-distance dependent, 142–360 ms). Consequence
  per the owner's decision: cached storyboard scrubbing is the default
  feedback and the product ships an Auto/Cached-only switch. The native
  session is implemented and frame-accurate; only its latency misses the
  gate on this machine.
- **ffprobe stream duration:** the fixture's subtitle stream is longer than
  the video stream; the catalogue correctly stores 667 500 ms
  (video stream), and `QMediaPlayer::duration()` was confirmed to report
  the subtitle value (669 360 ms) — the hover session clamps to the stored
  video-stream duration instead.
- **Hover scrubbing:** the default hover backend is a persistent mpv
  subprocess (owner's thumbfast reference), reused across hovers with a
  private IPC socket. Keyframe previews measured 30–51 ms during motion;
  exact refinement follows after settling (223–587 ms from scrub on the
  fixture). Sources unload on leave; cached feedback stays until live frames
  arrive. Cached previews remain the fallback when mpv is unavailable. See BENCHMARKS.md for limits. Cache hygiene: orphaned artifacts are swept at
  startup, previews of vanished videos are purged at scan completion, root
  removal removes their artifacts, and an optional "Check folders on
  startup" refresh (default on) reconciles moves/additions/deletions.
- **Storyboard extraction:** single ffmpeg process per sample with `-copyts`;
  actual delivered timestamps recorded in `cache_entries.sample_times`.
  Per-sample cost matches the §11 preliminary measurement (~230–520 ms);
  a 24-frame storyboard lands near the 10 s budget on this SSD machine.
  (Candidate follow-up: drive storyboard extraction through the same
  persistent-mpv mechanism as the hover session.)
- **Validation:** the prior full agent run passed 8/8 ctest suites. The
  live-seek correction passed the targeted hoversession and preview suites,
  plus isolated GUI checks; see BENCHMARKS.md.

## Remaining release blockers (owner validation pending)

1. **Windows build and package.** PLAN.md requires clean-machine Windows
   (bundled Qt/QML/plugins, media backend, ffprobe/ffmpeg, no dev
   prerequisites, portable ZIP or offline installer decided in milestone 0 —
   not yet possible from this Linux-only agent environment; the CMake project
   is cross-platform but unverified on Windows).
2. **Linux packaging.** Currently a dev-machine build (system Qt/FFmpeg).
   Bundle non-system dependencies or document base-system requirements;
   produce a portable archive; record sizes.
3. **Owner-run 10k synthetic benchmark and 100k stress mode** (§11): startup,
   search p95/p99, scrolling trace, memory, cancel gate, unchanged-rescan
   gate. The tooling hooks exist (`search` is O(N) worker-side at the 10k
   target — a `ponytail:` marker notes where a token index should replace it
   if the benchmark fails).
4. **Second release system:** seek coverage and hover-session behavior on the
   second target OS (spec §6/§11).
5. **Storyboard batching** (optional optimization): subprocess-per-sample
   meets the ≤10 s budget only marginally; a bounded batch/short-clip
   single-pass variant can cut it further.
6. Optional: directory watching, AI-tagging experiment (§13) — deferred
   unless requested.

## Milestone-0 decision record

- C++20/Qt Quick/CMake confirmed per PLAN.md; RapidFuzz pinned 3.3.4
  (system package with hash-pinned FetchContent fallback); SQLite via CMake.
- License: GPL-3.0-only (LICENSE in place; SPDX headers on all sources).
- Cached-scrubbing fallback has benchmark evidence (see BENCHMARKS.md); the
  native paused-player path ships as the Auto-mode enhancement with
  automatic disable when seeks exceed the latency budget.
