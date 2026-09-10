> Implementation update (2026-09-07): ScrubTub uses mpv for live hover previews. The Qt Multimedia prototype described below was rejected and removed; cached previews cover unavailable mpv.

# Video catalogue — technical specification

Status: implementation draft, 2026-09-06. Confirmed owner direction and milestone order are in [PLAN.md](PLAN.md). MUST indicates a requirement; numeric defaults are proposed settings to validate, not measured performance.

**Agent testing boundary:** PLAN.md's media/privacy limits govern every check in this document. At most 20 derived test-media files and 20 catalogue entries, a combined 1 GiB of generated test data, and a fresh disposable profile. The supplied original is read-only. Agents never access the owner's real catalogue or media. Larger benchmarks are prepared for the owner to run independently; the application's 10k design target remains unchanged.

## 1. Product behavior

| Area | Required behavior |
| --- | --- |
| Roots | Add multiple directories; recurse; show discovery/probe/preview progress independently; pause, resume, cancel, and rescan |
| Browsing | Virtual thumbnail grid; filename, duration, dimensions, size, rating, views, and availability; details panel with full path and tags |
| Preview | Horizontal hover shows a paused frame from the original file when feasible; instant cached fallback; show the actual frame timestamp |
| Playback | Open a selected video using the OS default application; keyboard activation and explicit Open action |
| Ratings | Unrated or 1–5 stars; clear a rating; persist independently of the media file |
| Tags | Automatic filename/folder/technical tags; add/remove manual tags; suppress individual automatic tags; batch editing for selected items |
| Search | Live, case-insensitive Unicode-aware filename/path/tag search with typo tolerance; cancel stale searches |
| Filters | Size, width/height and resolution presets, duration, rating/unrated, tags, root/folder, views including zero, and availability |
| Sorting | Name, size, duration, dimensions/pixel count, rating, views, date added, modified time, last opened; relevance while searching |
| View counts | Display and filter nonnegative counts; increment after OS launch request acceptance; allow setting/resetting a count |
| Safety | Source reads by default; explicit Trash action only; database/cache outside source directories |
| Persistence | Restore catalogue, filters/sort, roots, ratings/tags/views, and unfinished jobs after restart |

Combine different filter categories with AND. Tag selection supports explicit “all” or “any” matching plus excluded tags; exclusions always apply. Bounds are inclusive. Unknown metadata is NULL, displayed as unknown, and does not match numeric ranges; offer an explicit unknown option. Default browsing includes unprobed and unavailable entries with status indicators.

Display file size in IEC units (MiB/GiB) with bytes available in details; store bytes. Store duration as integer milliseconds, display `hh:mm:ss` as needed. Resolution uses display-oriented dimensions after rotation; presets use the shorter side (for example 1080 means 1080–1439 pixels), with exact dimensions available. Do not infer resolution from filename tags. Every sort has a stable video-ID tie-breaker; unknowns sort last.

## 2. Application structure

Use C++20, Qt 6 Core/Gui/Qml/Quick/QuickControls2 plus Multimedia for the benchmarked hover path, SQLite's C API, FFmpeg/ffprobe CLI, and RapidFuzz C++ for bounded edit-distance matching. Use Qt Test only for tests. Prefer Qt and standard-library facilities to additional dependencies. Exact versions and licenses are pinned in milestone 0.

Rust was considered; the language decision and integration tradeoff are recorded in PLAN.md. For C++, use value types, standard containers, RAII, and Qt's established QObject ownership. Use smart pointers for non-QObject owning resources; avoid custom allocation and manually managed owning raw pointers. Bind asynchronous callbacks to valid lifetimes, respect QObject thread affinity, and pass owned results across threads. Enable compiler warnings and run the small-corpus native checks under [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) and UndefinedBehaviorSanitizer where supported. Treat findings as failures; run performance measurements separately in unsanitized release builds. These checks reduce risk but do not provide Rust's compile-time guarantees or replace file-safety tests.

| Component | Responsibility |
| --- | --- |
| Qt Quick UI and C++ list model | Viewport-sized delegates, interaction, stable IDs, staged model updates |
| Catalogue/query worker | SQLite reads/writes, transactions, search snapshot, filter/sort results |
| Scanner | Incremental enumeration and file stat; submits bounded discovery batches |
| Media job scheduler | Queued probes/posters/storyboards, process limits, cancellation, timeouts; arbitration with one active hover decoder |
| Hover session | Single shared paused player, latest requested time, delivered frame timestamp, source release on exit |
| Image provider/cache | Asynchronous image reads, decoded-image limits, immutable cache keys |
| File actions | Only path allowed to request an external mutation; OS launch and explicit trash |

These are responsibilities, not a request for plugin interfaces, services, or one class per row. Keep a single desktop process plus bounded media subprocesses. SQLite is authoritative; in-memory search data and images are disposable. Do not add an HTTP server, ORM, message broker, GPU requirement, or custom playback engine. The native paused hover session is a preview only; full playback still uses the default external player.

Qt's [GridView](https://doc.qt.io/qt-6/qml-qtquick-gridview.html) supports native list models and delegate reuse. Set reuse explicitly, keep a small offscreen buffer, and reset hover/selection image state when a delegate is reused. No full-catalogue QML object list. Follow Qt's [performance guidance](https://doc.qt.io/qt-6/qtquick-performance.html) for simple delegates and asynchronous images; verify actual rendering costs with profiling.

The UI thread MUST NOT enumerate files, probe media, decode images, perform SQLite I/O, fuzzy-match a catalogue, sort a large result, or wait for workers. Workers return immutable results with request generations. Ignore stale generations. Rate-limit scan status/model updates to at most 10 per second. Numeric edits should become visible immediately with an explicit error/revert if persistence fails.

## 3. File-safety contract

The app MUST NOT write, truncate, rename, chmod, set timestamps/xattrs, embed tags, create sidecars, or delete user media during discovery, metadata extraction, tagging, rating, previewing, cache cleanup, upgrades, or removal of a catalogue root. Scan roots authorize reading, not writing. Cache/database outputs go to fresh app-owned directories obtained through [QStandardPaths](https://doc.qt.io/qt-6/qstandardpaths.html); keep the DB on a local filesystem.

This contract covers writes caused by catalogue code and its extraction commands. Ordinary OS reads can update access-time metadata, and a user-selected external player has its own behavior. Do not claim to suppress these by modifying timestamps afterward. If the owner needs bit-for-bit filesystem-metadata immutability, require read-only mounts/snapshots and separately constrain the player; record that as an additional requirement.

App-owned does not mean “anything beneath a directory with our name.” Create a private profile directory, track generated cache entries, reject symlink/reparse escapes in writable paths, and only overwrite tracked outputs or app-created temporary files. Never recursively delete unknown contents. Do not allow a cache location equal to or nested under a media root in v1. User-requested backup export to an existing file requires overwrite confirmation.

Pass executable paths and argument arrays to QProcess; never interpolate shell commands. Use absolute source paths, explicit stream selection, `-nostdin`, bounded logs/output, and controlled temp destinations. Restrict media protocols to those needed for local files; reject playlists/network streams and unsupported external references. Source files are untrusted inputs: time out probes/decodes, terminate their process trees, and never run media as executables. Document that process isolation alone is not a security sandbox; test read-only source permissions and add platform containment if the stated threat model requires hostile-codec isolation.

No symlink, junction, reparse-point traversal, or special-device/FIFO reads by default. Reject linked roots or require the real directory to be added explicitly. Keep original native path spelling and an OS-appropriate comparison key; Linux paths are case-sensitive. Do not lowercase all paths or use lossy Unicode conversion. Unsupported native filename encodings must produce a visible skip/error, never an operation on a substituted path.

### Explicit deletion

“Remove folder from catalogue” changes only app state. “Move video to Trash/Recycle Bin” names the full path(s), count, and size and requires confirmation for that selection. Resolve IDs internally; re-stat and verify identity immediately before the operation. If a file changed or became a link since confirmation, abort that item and ask for a fresh action.

Use [QFile::moveToTrash](https://doc.qt.io/qt-6/qfile.html#moveToTrash) where supported. Failure MUST leave the catalogue entry and show the reason. No fallback to permanent deletion, no recursive directory deletion, and no mutation retry after a restart. Preserve the annotations in a trashed/missing state until explicitly purged from the catalogue. Handle per-item results for a batch. A filesystem race between a final path check and a platform trash call is a known platform limitation: keep the interval short, fail closed on observable changes, and do not claim atomic identity protection from a path-based API.

## 4. Database and identity

Use one local SQLite database with foreign keys enabled, WAL, a bounded busy timeout, and transactional writes. Use `synchronous=FULL` initially for annotation durability and benchmark batched scan writes. SQLite WAL requires cooperating processes on the same host and is unsuitable for a shared NAS database; see [SQLite WAL documentation](https://sqlite.org/wal.html). Media may remain on mounted network storage.

One application instance owns a profile via a lock. Begin with one DB worker/connection; add a separate reader only if measured contention warrants it. Never hold a write transaction while probing or decoding. Apply versioned migrations transactionally after a verified backup; refuse to open newer unsupported schemas. On corruption, preserve the original and offer restore, never silently replace the catalogue.

Logical schema (implement constraints and indexes; no ORM required):

| Table | Fields and invariants |
| --- | --- |
| `roots` | ID, native path, comparison key UNIQUE, volume identity if available, status, scan generation, last complete scan, scan options |
| `videos` | Stable integer ID, root ID, relative native path/key UNIQUE within root, file identity, size bytes, mtime precision, revision, selected stream, duration ms, coded/display width/height, codec, rating NULL or 1–5, views ≥0, added/last-opened time, availability, probe status/error, seen generation |
| `tags` | ID, normalized text UNIQUE, display label |
| `video_tags` | Video ID, tag ID, origin (`manual`, `filename`, `folder`, `technical`), composite uniqueness |
| `tag_suppressions` | Video ID + tag ID UNIQUE; hides automatic assignments only |
| `jobs` | Video ID, revision, kind, state, bounded retry count, error; UNIQUE video/revision/kind |
| `cache_entries` | Video ID, revision, profile/version, kind, relative owned path, byte size, actual sample timestamps, coarse access time; UNIQUE logical artifact |
| `settings` | Versioned small settings values, including tag rules and last UI state |

Store path payloads losslessly for the platform, using native bytes/BLOBs if required; display text is separate from identity. Store timestamps in UTC and use signed 64-bit values with explicit units. Constraints reject negative sizes/durations/views and zero/negative known dimensions. Add indexes on root/path, availability, common numeric sort/filter fields, tag-to-video and video-to-tag lookups, and queued jobs. Verify query plans; do not add every possible compound index speculatively.

Each real path is a separate catalogue item even when contents duplicate. Reject identical or overlapping selected roots with a clear explanation to avoid duplicate traversal. Do not hash entire videos on normal scans. A fast revision uses file identity where available, size, and mtime; provide Force refresh because same-size edits with preserved timestamps can escape stat-based detection.

Preserve ID/annotations for an unchanged path whose file identity still agrees. Reconcile external renames only with a unique, trustworthy identity on the same volume and supporting stat evidence; do not merge copies or ambiguous hardlinks. Native IDs can be reused, especially after long absences. On ambiguity keep the old row missing and create the new row, with an explicit relink action. File replacement must invalidate extracted data; do not silently transfer personal annotations to a demonstrably different file. Root relocation is an explicit action with a dry-run summary, never fuzzy path guessing.

Back up using SQLite's consistent backup mechanism, including all annotations, root mappings, suppressions, and settings. Thumbnails are optional and regenerable. Do not copy only the live `.db` while ignoring WAL. Restore validates schema/integrity into an app-owned temporary location before replacing the active database; preserve the current backup. Missing original paths remain relinkable after restore.

## 5. Scanning and background jobs

Enumerate regular files with a case-insensitive extension allowlist: mp4, m4v, mkv, webm, avi, mov, wmv, flv, mpg, mpeg, and ogv initially. Extension is a discovery hint; ffprobe must identify a real video stream. Skip hidden directories by default with a root option to include them. Ignore app-owned folders and temporary fixture/cache paths. Unsupported/corrupt candidates get an error state, not endless retries.

Process directories incrementally; bound discovery batches to 500 and pending in-memory media jobs to 256. Persist remaining jobs rather than storing one live object per task. Commit batches at 250 rows or 100 ms, whichever comes first. Present rows after stat; fill metadata and previews later. These are tuning defaults, not immutable constants.

A scan generation marks seen entries. Mark absent files missing only after the root and relevant subtree were completely enumerated. Root offline, permissions failure, cancelled scan, and disconnected NAS are incomplete scans: retain prior availability for unvisited areas and show root/subtree uncertainty. Never remove annotations because a drive disappeared. Skip unchanged revisions; force refresh bypasses the shortcut. Interrupted jobs return to queued on startup; cache writes use app-owned temp files and atomic rename after validation.

For ffprobe, request only required fields and choose the default non-attached-picture video stream, otherwise the first real video stream. Use that stream's positive finite duration, then a documented container fallback. Do not use the longest subtitle duration. Preserve unknown if neither is usable. Read rotation/display matrix and derive display dimensions. Decode only the selected video stream; audio/subtitles are unnecessary for previews.

Initial media budget: at most two active probe/decode processes combined; at most one active decoder including the hover player; one media job per HDD/NAS/removable root unless configured otherwise. Limit FFmpeg decoder/filter threads explicitly; do not let each process consume all cores. Run extraction at low priority where available. Priority order: active hovered item, visible posters, nearby posters, remaining probes, background storyboards; reserve enough probe work to prevent starvation. Suspend/cancel a background decode to give active hover priority. The Qt player backend also consumes decoder threads and buffers; measure these in the same budget. Background work can pause without disabling search or cached browsing.

Start with a 30-second probe timeout and a 60-second per-preview-job timeout, both configurable for slow media. A timeout is visible/retryable and does not silently exclude the file forever. Exponential retries are unnecessary: one automatic retry for a transient I/O failure, then explicit retry. Cancel obsolete revisions, bound stderr to 64 KiB per failure, and never block the UI waiting for subprocess exit. Stat before/after extraction and discard output if the source revision changed.

## 6. Thumbnails and hover timeline

Poster: one approximately 320-pixel-wide frame near 10% of selected-video duration, or the first decodable frame for unknown/very short duration. Preserve aspect ratio, apply orientation, and cap decoded output dimensions. Fallback hover storyboard: default 24 evenly spaced samples from the first frame to just before video end; fewer for very short clips. A default profile may offer 12/24/48 samples, with resulting storage and rebuild costs visible.

For a thumbnail content rectangle `(left, width)` and finite positive duration `D`, map the pointer to `u = clamp((x-left)/width, 0, 1)`, requested time `u*D`, then request that time from the active hover session; meanwhile select the nearest available cached sample time. Guard zero width. Show the actual sampled timestamp and approximate position; do not label a sparse cached frame as an exact arbitrary-time seek. At the right edge show the final available sample. On leave return to the poster. Unknown duration has a poster but no misleading timeline.

Generate posters before full storyboards. Generate a missing fallback storyboard through low-priority work when the original-file preview is unavailable or through explicit precompute; do not compete with the active precise seek. Coalesce repeated requests by video/revision/profile. While pending, retain the poster and a subtle loading indication. In cached-only mode, pointer motion across a ready storyboard requires zero FFmpeg calls and zero source-media reads. Original-file mode intentionally reads the selected video through one bounded decoder.

Start with seek-based, selected-stream extraction using FFmpeg's documented [input seeking behavior](https://ffmpeg.org/ffmpeg.html). Avoid decoding entire multi-hour videos merely to select 24 frames. Benchmark subprocess-per-seek overhead against bounded batches on representative GOPs. Choose the simplest measured method that meets the import budget; record actual timestamps rather than assuming seek requests were exact. Failure at one timestamp may leave a partial usable storyboard.

Use a compressed JPEG atlas plus timestamp metadata, or equivalently compact independently cached frames if measurement favors them. At 24 × 320×180 pixels, one decoded RGBA storyboard is approximately 5.3 MiB; do not hold one for every visible tile. Load/decode the hovered storyboard and a small recent LRU; visible cards normally need only posters. Cap total decoded image RAM at 128 MiB, account for GPU uploads separately, and cap disk thumbnails at 5 GiB by default. No unbounded Qt image cache alongside a separately bounded application cache.

At 10k videos a 250 KiB compressed storyboard averages about 2.4 GiB total, before posters/metadata; at the optional 100k stress scale it is about 24 GiB. Lazy generation and eviction are therefore requirements. The cache is disposable; eviction removes only owned artifact entries, never ratings/tags. Do not precompute the entire library into a cache too small to retain it and repeatedly regenerate evicted entries. Offer an explicit precompute action with a storage estimate; normal background generation stops at its budget. LRU access timestamps are batched, not written on every mouse move. Changing thumbnail quality creates new versioned keys and retires old entries safely.

### Original-file paused scrubbing

The preferred behavior matches an embedded player paused at the hovered time. Long-GOP interframe codecs may require decoding forward from an earlier keyframe, so an SSD alone cannot guarantee cheap random seeks. Hardware decoding can help some media but adds backend/driver variance. Avoid pre-transcoding originals or storing full low-resolution proxy videos in v1; those impose substantial indexing and disk costs.

Prototype the existing native facility first: one shared [QMediaPlayer](https://doc.qt.io/qt-6/qmediaplayer.html) with a [QVideoSink](https://doc.qt.io/qt-6/qvideosink.html), attached only to the active hovered card. Qt exposes seekability, position, pause, and delivered video frames; these APIs do not themselves promise frame-exact seeking. Use the delivered frame's presentation timestamp and duration for validation, not only `positionChanged`. Validate Qt Multimedia's [backend/platform behavior](https://doc.qt.io/qt-6/qtmultimedia-index.html) on both release systems.

Activate after 120 ms of hover dwell. Disable audio/subtitle tracks and attach no audible output. Load the selected stream, initialize decoding if needed while the output is hidden, and pause at the requested position. Never let the displayed timeline run on its own. Use asynchronous APIs; the backend does decoding. Keep at most one seek in progress plus one replaceable latest target, coalesce pointer updates at 60 ms initially, and do not create a new player or decoder for each event. A completed old seek must not overwrite a newer settled target. Cache feedback may update at display refresh speed independently.

Associate the session with video ID, revision, and hover generation. On changing cards invalidate old frames before switching source. On pointer exit show the poster immediately, stop/unload the source, and release file handles promptly; a selected-file trash action first tears down its hover session. Cancel/disable precise seeking if repeated seeks exceed the latency budget or the source is unseekable/corrupt/offline; continue with cached samples and a concise status. Do not keep hammering slow media. Let users choose Auto (default) or Cached only.

The prototype must verify seek completion while paused, selected stream/rotation, timestamp accuracy, cancellation/source switching, no unintended audio, idle CPU, and handle release. If Qt's existing backend cannot meet the gates, cached fallback is authorized; a custom libav decoder is not automatically required. Ship one original-seek backend, not a framework of alternatives. Foreground decode errors must not damage the catalogue; native in-process codecs are not crash-isolated like extraction subprocesses, which is part of the feasibility assessment.

A thumbnail's physical width also limits pointer time resolution: a 10-minute clip across 300 pixels maps about 2 seconds per pixel. Provide keyboard steps of 1 second (Shift: 100 ms) on the focused timeline, clamped to valid duration, so precise decoding is actually usable. At end-of-file request the last representable video frame rather than a timestamp beyond the stream. Expose the actual delivered time; promise high time resolution only to the measured accuracy above.

## 7. Automatic and manual tags

Do not mutate the source filename. Normalize tag identity with Unicode NFKC and case folding; preserve a readable display label. Split filename stem and directories below the selected root at punctuation/separators into Unicode letter/digit tokens; split camel-case boundaries before case folding. Do not derive tags from the user's home path or from the selected root's own name by default.

Discard empty/single-character tokens, standalone numbers, and recognized codec/resolution/container tokens from filename-derived tags. Keep alphabetic words of at least two characters and meaningful mixed alphanumeric tokens. A small visible editable ignore list can suppress terms such as `final`, `copy`, and `test`; it is not a language model. Pure numeric years such as 2024 are ignored by default. Technical tags such as `codec:h264` and `resolution:1080` come from extracted metadata, with distinct provenance. Search still indexes the full filename, including ignored tag words and numbers.

Examples, with default rules and no parent folders: `Summer_Trip-2024_1080p.mp4` → `summer`, `trip`; `summer.trip.final.mp4` → `summer`, `trip`; `Café 東京 01.mp4` → `café`, `東京`; `testvideo1.mp4` → `testvideo1`. `Travel/Japan/Summer_Trip.mp4` adds `travel`, `japan` only when these are below the selected root. Technical tags are additional and do not depend on the filename spelling.

Multiple origins for the same visible tag collapse in display. Deleting an automatic tag creates a suppression so rescans/rule updates do not restore it. A manual assignment takes precedence over automatic suppression; removing a manual assignment must make clear whether an automatic origin remains. Regeneration replaces automatic assignments only, atomically; manual tags and suppressions survive. Provide an explicit reset-suppressed-tags action. Tag edits and ratings are catalogue-only operations.

## 8. Live search and filtering

Normalize searchable filename, relative path, and visible tags into an in-memory native snapshot when loading/updating the catalogue. Search also uses a diacritic-folded copy for convenient matching (`cafe` → `Café`), while identity/display preserves originals. Use Unicode scalar values, not edit distance over UTF-8 bytes. Avoid making long path strings or per-row QML objects for each query.

Debounce typing by 50 ms. Apply structured filters first, then require every query token to match at least one searchable token or substring. Exact/prefix/substring matches rank above typo matches. One- and two-character query tokens use exact prefix/substring matching only; lengths 3–5 permit edit distance 1, lengths ≥6 permit 2. Cap query input to a documented 256 Unicode characters and 16 tokens with visible validation, not silent truncation. Use [RapidFuzz C++](https://github.com/rapidfuzz/rapidfuzz-cpp) for cutoff-aware Levenshtein matching instead of writing a fuzzy library.

Sort relevance by match class, summed edit distance, filename before folder-only matches, then normalized filename and ID. Explicit user sorting overrides relevance. Return a complete ordered ID list and fetch card-detail pages of 200 by ID on demand. The UI sees page data and result count, not a full serialization of the catalogue. Check cancellation every bounded batch (initially 256 records); discard obsolete results even if cancellation raced completion.

Start with a worker-side scan of compact pre-normalized records at the agreed 10k scale. This deliberately trades O(N) query work for fewer moving parts. Mark that ceiling in implementation with a `ponytail:` comment. If the benchmark fails, add a token/vocabulary index and measure again; do not silently cap candidates, omit matches, or reduce typo tolerance to pass. Snapshot updates should copy affected chunks or apply worker-owned deltas rather than cloning all text per keystroke.

SQLite FTS5 is optional after profiling; its prefix/substring facilities do not by themselves provide edit-distance fuzzy matching. See the [FTS5 documentation](https://www.sqlite.org/fts5.html). Within the 20-entry corpus, tests must exercise multiple pages/batches using smaller test-supplied batch sizes, an eligible fuzzy match past the first batch, and filters that change the best-ranked results. Do not create extra media or large synthetic catalogues to test pagination boundaries.

## 9. Opening and view counts

Use `QDesktopServices::openUrl(QUrl::fromLocalFile(path))` or an equivalent native adapter. Only allow known catalogue videos that currently resolve to regular files with expected identity. Never build a shell command. Do not pass a hover timestamp to the default player: there is no universal seek protocol and this is not a promised feature.

Qt documents that a successful [openUrl](https://doc.qt.io/qt-6/qdesktopservices.html) return indicates the request was handed to the system, not that the external app successfully played the file. Increment views once per accepted user launch action; hovering, selecting, and failed handoffs do not count. Suppress duplicate dispatch from the same double-click event; two deliberate launches count twice. Store last-opened time with the increment in one transaction. If persistence fails after dispatch, report that the count was not saved; never repeat the launch to repair the count. A crash between handoff and commit can lose one increment and must not cause automatic replay.

The UI may say “Views” with explanatory tooltip “Times opened from this catalogue.” Provide a nonnegative integer editor/reset with confirmation, since external watches cannot be observed. A completed-watch tracker or launch-at-preview-time feature requires explicit player integration and a separate owner decision.

## 10. UI and settings

Layout: left roots/folders and filters; top search, sort, and thumbnail-size controls; central grid; optional right details panel. Provide an empty-state Add folder action. Expose scan progress as discovered/probed/previewed/error counts without pretending unknown totals are a precise percentage. Cards remain interactive while jobs run.

Support keyboard navigation, Enter to open, Space to select, accessible named star controls and tags, visible focus, adequate contrast, and screen-reader names. Hover scrubbing also has a focused-card slider/arrow-key equivalent with timestamp announcement. Stars/tag buttons must not accidentally trigger playback. Avoid heavy blur, continuous animations, animated cards when idle, and hidden essential hover-only controls.

Persist only useful settings: thumbnail size/sample profile, disk-cache cap, media worker limit/storage mode, auto-tag options, scan hidden directories, and last filters/sort. Default refresh on explicit Rescan and an optional startup refresh; no recurring full scan while idle. Paused work stays paused across restart. Expose errors and retry without modal interruption for every bad file.

## 11. Performance acceptance

All numbers are proposed product/release gates. Agents measure only applicable behavior on their ≤20-entry corpus; the owner validates all large-library conditions independently. Agent handoff may be complete while those owner-run gates remain pending, but release documentation must not call them passed. Initial reference: 4 physical CPU cores, 16 GiB RAM, local SSD for DB/cache, integrated graphics, 1920×1080 at 60 Hz, release build. Record exact CPU/storage/GPU/OS/toolchain versions and power mode. Seek coverage on both target systems. Source-drive scan throughput is measured separately from catalogue interaction.

| Scenario | Target |
| --- | --- |
| First useful grid, existing 10k catalogue | ≤1 s warm; ≤3 s cold process/OS-cache test under stated conditions |
| Input to settled search results including debounce, 10k | p95 ≤150 ms; p99 ≤300 ms for published query corpus |
| Structured filter/sort, 10k | p95 ≤100 ms |
| Scrolling | p95 frame time ≤16.7 ms, p99 ≤33.3 ms; no >100 ms UI stalls in the test trace |
| Hover change, storyboard decoded | p95 ≤16.7 ms |
| First cached storyboard from local SSD | p95 ≤100 ms; separate from cold extraction |
| Original-file seek, already loaded 1080p SSD media | p95 ≤200 ms from last pointer input to suitably timed frame, including coalescing |
| First original-file hover, cold open on same reference set | p95 ≤750 ms; poster/cached feedback remains immediate |
| Seek timing accuracy | Delivered frame contains target presentation time or is within 100 ms; record actual PTS/error, not requested position alone |
| Steady browsing memory, 10k | Main-process RSS/working set ≤250 MiB; report GPU memory separately |
| Background processing memory | Combined app, active hover decoder, and media children ≤750 MiB on the reference media set |
| Idle | No periodic media I/O; average CPU <1% of one logical core over 60 s |
| Cancel scan | UI acknowledgement ≤100 ms; cease new media work immediately; normal local active processes stopped ≤2 s |
| Unchanged rescan | Zero ffprobe/FFmpeg jobs for unchanged successfully indexed revisions |

Do not set a universal videos-per-second import promise: GOPs, codecs, file sizes, cache warmth, HDD seeks, and NAS latency materially vary. Publish discovery files/s, probes/s, previews/s, source bytes read, CPU time, and elapsed scan time for each fixture/storage class. For the supplied local video, target one usable cold poster within 2 s and a 24-frame storyboard within 10 s on the reference SSD machine; validate these in milestone 0 and record any required changes to the extraction approach. These thresholds are not claims about every file.

Provide an owner-run 10k synthetic benchmark and optional 100k stress mode, with fictional varied filenames, Unicode, tag distributions, filter selectivity, and at least 100 search queries spanning short/common/typo cases. Do not execute these larger modes in agent tools or CI configured for agent validation. Default test commands use at most 20 entries. Agents can exercise 100 query strings against that same small corpus without adding media. For owner-run measurements, document at least five startup/search sequences, p50/p95/p99 and peak memory where appropriate, a 60-second scrolling trace, and interaction while scanning/extracting. Define cold versus warm explicitly; no cache flushes on the user's active machine. Owner-run real-library validation uses a locally selected path and separate profile; export only aggregate metrics. Do not expose the library to the agent through screenshots, traces, logs, or dumps.

For comparisons, record reference-app version/settings, identical media and thumbnail density, generated-cache sizes, hardware, and query behavior. Measure first import, unchanged refresh, startup, search, scrolling, and RAM. If reference software cannot be tested, report that the comparative claim is unverified; meeting absolute budgets is still measurable. Never advertise “more performant” solely from framework choice.

### Preliminary sample measurement (planning only)

On 2026-09-06, this workspace reported an Intel i5-8600K (6 logical CPUs), about 16 GB installed RAM, CachyOS Linux, and FFmpeg n9.0.1. Ten independent single-threaded FFmpeg launches seeking into `testvideo1.mp4`, decoding one frame, scaling to width 320, and writing to a null output took 231.0–515.1 ms each (median 405.0 ms). The ordered target times were 10, 300, 650, 123.456, and 500 seconds, repeated twice; ordered timings in milliseconds were 402.2, 238.0, 371.8, 513.9, 449.1, 407.8, 231.0, 364.5, 515.1, and 462.4. Every command exited successfully.

Reproduce each target by measuring process wall time around this command, substituting the target and absolute source path through an argument array:

```text
ffmpeg -hide_banner -loglevel error -nostdin -threads 1
  -ss TARGET_SECONDS -i ABSOLUTE_SOURCE_PATH -map 0:v:0 -an -sn
  -frames:v 1 -vf scale=320:-2 -filter_threads 1 -threads 1 -f null -
```

This is evidence against spawning a fresh decoder on every mouse event on this machine. It does not establish the latency of a persistent Qt player, hardware decoding, frame accuracy, encoded image delivery, GUI rendering, cold-cache behavior, or other media. Storage/cache state was uncontrolled. The milestone-0 paused-player benchmark remains necessary. The original fixture was read only; no derived media or application implementation was created by this check.

## 12. Required checks and release gates

All agent-run checks below operate on the allowed disposable corpus, including mutation, backup/restore, sanitizer, and GUI tests. Simulate disk-full/write failures with an injected failing writer or a small bounded test volume; never fill the host SSD. Configure test cache limits far below the 1 GiB total-data ceiling; the product's 5 GiB cache default is not an agent-testing allocation. Enforce fixture count/byte budgets and fresh-profile isolation in the test entry point so running the suite cannot accidentally scan real libraries.

- **No source writes:** snapshot source file hashes, contents/size, mtime, permissions, directory entries, and relevant xattrs; exercise scans, tags, ratings, views, previews, refresh, cache clear, and root removal with an inert test player; compare afterward. Observe filesystem write events/syscalls to catch temporary writes later reverted. Test read-only mounted sources. Account separately for OS access-time updates.
- **Authorized mutation:** only a newly created disposable copy is trashed after the explicit action; siblings, original video, and symlink targets remain intact. Replaced paths, failed trash, locked files, and partial batches are handled without permanent-delete fallback.
- **Reconciliation:** repeat and interrupt scans, disconnect a root, deny access to a subtree, rename within a volume, replace content at the same path, introduce duplicates/overlapping roots, and force refresh a same-size/same-time edit. Verify annotation retention and no false mass missing state.
- **Extraction:** supplied video uses video duration (~667.5 s), not subtitle duration; test rotation, zero/unknown duration, multiple streams, corrupt input, output/time limits, cancellation, and stale revisions. Pointer edges/zero-width/short clips must not produce out-of-range frames.
- **Search and tags:** table-driven filename examples, Unicode normalization, short queries, typo cutoffs, combined range/tag filters, nulls, stable ties, manual/automatic overlap, suppressed tags after rescan, and a result beyond early candidate positions.
- **Persistence:** restart after edits, kill during cache writes/scan transactions, fill the destination disk, migrate/restore a backup, and reject a newer schema. Recover work without losing committed annotations or replaying external actions.
- **Platform:** Windows and Linux clean-machine package smoke tests for folder selection, paths with spaces/non-ASCII characters, default-player dispatch, writable app storage, and trash failure/success. GUI tests use only isolated disposable sessions as required by AGENTS.md. Windows validation starts without separately installed Qt, FFmpeg, SQLite, SDKs, or development-tool PATH entries, and with network access disabled. Verify the package supplies its Qt/QML/plugins, media backend, command-line tools, and licensed compiler-runtime prerequisites; an installer may include the official redistributable where necessary. No manual prerequisite downloads. Validate Linux against documented supported base-system requirements. All media tests remain within the 20-file/1 GiB boundary.
- **Distribution:** pinned sources and hashes, dependency-license inventory and notices, owner-approved app license, no private fixture media/cache in Git, offline normal operation, and published reproducible benchmark commands/results.

Agent implementation handoff is complete when the feature table and applicable small-corpus checks pass, evidence is linked, and owner-run benchmark instructions plus unverified gates are listed. Full-scale release validation belongs to the owner and must remain visibly pending until measured. The owner permits cached-only scrubbing if original-file seeking has a demonstrated major downside. Record failed gates and affected media/platforms when taking that fallback; do not silently claim precise seeking or AI support that was not implemented.


## 13. Optional local-AI feasibility, after the core gates

The owner is interested in AI tags but has not requested a mandatory model/runtime in the first release. Filename/folder tagging must work on every supported machine with no AI installed. A small local image classifier or image/text embedding model over sampled frames is a plausible experiment for broad objects/scenes/actions; this is a design hypothesis, not a measured quality or hardware promise. It will not reliably summarize events across an entire video from a few stills.

Start an optional experiment using 3–8 already cached frames per video and a small, owner-reviewable label vocabulary. Compare a compact classifier with a CLIP-style image/text model only if the classifier's vocabulary is inadequate. Do not start with a large video-language model, full-video decode, speech transcription, or scene-by-scene inference. Average/aggregate multiple frame scores with an explicitly evaluated rule; confidence scores need calibration and are not probabilities by default.

Measure on a CPU-only 8 GiB machine and a 16 GiB machine with available GPU acceleration. These are evaluation tiers, not asserted minimum requirements. Evaluate ONNX Runtime only once a compatible redistributable model is selected; its [execution providers](https://onnxruntime.ai/docs/execution-providers/) offer CPU and hardware-specific backends, whose availability and performance must be tested per platform. GPU is optional; no CUDA-only required path. Check code, weights, vocabulary, and model redistribution licenses independently for GPL compatibility before shipping anything.

Report model/download size, cold load time, peak extra RAM, VRAM where applicable, time per video, energy/CPU load, and projected 10k completion time. For illustration only, 2 seconds/video is about 5.6 hours for 10k videos; 10 seconds/video is about 27.8 hours, before extra decoding. Measure actual results before promising a scan duration. Proposed budget: ≤1 GiB additional host RAM for a CPU experiment, one background inference job, paused while the user actively scrubs or when browsing budgets are missed. If that is not achievable with useful quality, keep the feature experimental rather than raising base requirements.

Agent feasibility checks must remain within the same 20-media-file corpus and cannot establish representative model accuracy. Leave the larger quality evaluation (suggested: at least 100 varied clips with expected labels, including negative/ambiguous cases) to the owner, who keeps clips, labels, and predictions private and may share aggregate precision/recall. A proposed acceptance threshold is ≥90% precision for auto-suggested high-confidence tags; report recall and untagged coverage so abstention cannot disguise poor usefulness. Never benchmark accuracy on repeated copies of testvideo1.mp4. Model quality and vocabulary must fit the owner's collection before production inclusion; agents must not inspect it to perform this assessment.

If promoted later, require an explicit enable/model download action with size and license shown, keep all inference local, and offer suggestions before automatic bulk acceptance. Store provenance/model version, preserve manual tags and suppressions, make jobs resumable/cancellable, and unload the model when disabled. No AI inference at startup, per keystroke, or per mouse movement. Deliver a feasibility report first; adding a large dependency and model payload is a separate implementation milestone.
