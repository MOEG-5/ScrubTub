# Live-seek investigation — 2026-09-06

The findings below describe the implementation before correction; code line
references refer to that version. The fixes are now implemented and measured
in [BENCHMARKS.md](BENCHMARKS.md).

The mpv subprocess and raw-frame transport exist, but thumbfast's fast seek
scheduling does not. The main delay is imposed by scrubtub before decoding starts.

## Confirmed causes

1. **No live seeks during motion.** `HoverSession::scrub()` restarts a
   220 ms single-shot timer on every pointer update. Only that timer sends
   a seek, always with `absolute+exact`. The `exact=false` branch in
   `startMpvSeek()` is never called. Continuous movement can therefore defer
   live feedback indefinitely. See `src/media/HoverSession.cpp:208` and
   `src/media/HoverSession.cpp:353`.

   The supplied `idea/thumbfast-master/thumbfast.lua:648` instead sends a
   keyframe seek immediately, coalesces subsequent targets on a 50 ms tick,
   and refines with an exact seek after the pointer settles. Its fast-seek
   policy is conditional on the video's reported FPS (`:919`).

2. **The advertised cached feedback is hidden.** `VideoCard.qml` sets
   `liveFrameVisible` as soon as live mode engages, without waiting for a
   matching frame. Both the poster and cached storyboard image use
   `!liveFrameVisible` for visibility. The old live image can remain visible
   throughout movement while the decoder receives no seeks. See
   `src/ui/VideoCard.qml:69`, `:103`, `:180`, and `:318`.

3. **Initial hover loses the pointer target.** Engagement starts at zero;
   the source-ready handler does not scrub to the pointer's current position.
   Only a subsequent mouse movement requests a target. A stationary pointer
   after dwell can therefore leave the first frame displayed. See
   `src/media/HoverSession.cpp:134` and `src/ui/VideoCard.qml:313`.

## Reproduction on the supplied fixture

Read-only input: `testvideo1.mp4`, H.264 1080p50. Linux mpv 0.41.0,
FFmpeg n9.0.1. Temporary output directories; no catalogue or personal media
access, no desktop automation. These are warm local-file observations, not
cold-storage or rendering benchmarks.

A persistent mpv process used the current `engageMpv()` arguments, including
the same 320-pixel BGRA image2 output. Five sequential targets were 10, 300,
650, 123.456, and 500 seconds. IPC-to-file-update times were:

| Request | Observed range |
| --- | --- |
| `absolute+keyframes`, current decoder flags | 12.5–17.1 ms |
| `absolute+exact`, current decoder flags | 104.6–275.5 ms |
| `absolute+exact`, with thumbfast demux/decode/scaler tuning | 102.7–264.8 ms |

The tuning comparison added zero demux readahead, a 128 KiB demux buffer,
skipped loop filtering, disabled zimg, and disabled script/OSC/ytdl loading.
It did not test hardware decoding or the entire thumbfast UI. The small
sample does not establish a significant tuning benefit; the scheduling
difference is much larger.

Keyframe output was checked again with both synchronous and asynchronous
JSON commands. All five resulting frames in each mode matched, byte for
byte, a separate mpv process started directly at that target with
`--start=<target> --hr-seek=no`. This verifies actual correct image updates,
not just modification-time changes. Keyframe PTS values were 6.4, 298,
646.28, 118.24, and 495 seconds respectively: these are approximate previews,
which need exact refinement after settling.

A separate temporary Qt harness compiled the current `HoverSession.cpp`
directly and explicitly selected `/usr/bin/mpv`, using offscreen Qt and a
fresh profile environment. Engagement to first frame took 219 ms. The five
scrub-to-frame latencies were 498, 445, 492, 568, and 497 ms respectively,
including the application's settle delay. Ten targets spaced 60 ms apart
produced **zero frames during the 600 ms movement burst**, and one frame by
2.6 seconds after burst start. These measurements exclude the QML dwell,
rendering, and background catalogue jobs. The reported mpv timestamps in
this harness are synthesized by the current implementation, so they do not
constitute an accuracy check.

**The prior claim in `docs/BENCHMARKS.md` that paused keyframe seeks do not
repaint is contradicted by this reproduction.** The backend could supply
fast moving previews with its existing raw-frame transport.

## Other differences and verification gaps

- scrubtub kills the decoder on every hover exit and starts a new process after
  the next 120 ms dwell. The supplied thumbfast configuration has
  `spawn_first=yes`, `quit_after_inactivity=0`, and `hwdec=yes`.
  Startup and hardware configuration are therefore not equivalent. Keeping
  a source open indefinitely would conflict with scrubtub's file-handle release
  requirement; any lifecycle change should preserve unloading on exit.
- `startMpvSeek()` can block the UI waiting up to two seconds for IPC. It
  ignores the write result, while its caller still marks the target sent.
- scrubtub polls every 25 ms and reads the file being overwritten. Thumbfast
  polls about every 16.7 ms and renames the output before consuming it.
  These are secondary latency/consistency differences.
- `pollFrameFile()` sets delivered PTS to the last requested target instead
  of reading the decoded frame timestamp (`HoverSession.cpp:285`). Existing
  accuracy assertions therefore cannot prove mpv frame accuracy.
- `tests/tst_hoversession.cpp:70` allows 30 seconds for a nearby timestamp.
  Its rapid-scrub test checks only eventual delivery count, so zero deliveries
  during movement passes. Neither catches the principal responsiveness bug.
- The now-removed `bench/hoverbench.cpp` benchmarked a separate `QMediaPlayer`, not the
  application's mpv-backed `HoverSession`. Its results cannot establish mpv
  live-seek latency.

## Correction priorities

1. Restore immediate/coalesced keyframe requests during motion, followed by
   exact refinement on settle, with only the newest target retained.
2. Pass the current pointer target when engagement completes and preserve
   cached/poster feedback until a valid frame for that session arrives.
3. Report actual delivered timestamps and add a regression check requiring
   frame progress during continuous scrubbing plus final-target accuracy.
4. Then measure startup, IPC blocking, and output handoff before making
   further performance changes.

Exact seeking still requires decoding forward from a keyframe; it is not
expected to have keyframe-seek latency. This distinction is documented in
the [mpv seek options](https://mpv.io/manual/stable/#options-hr-seek).
Thumbfast-like responsiveness comes from fast approximate feedback during
motion, then precise refinement.

Follow-up: the correction is now implemented. See the persistent-process
section of [BENCHMARKS.md](BENCHMARKS.md) for current behavior and measured
results. The supplied thumbfast config was left unchanged.
