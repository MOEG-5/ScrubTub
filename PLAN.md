# Video catalogue — implementation plan

Status: draft for the implementing agent, 2026-09-06. This task produces specifications, not the application. Read [TECH_SPEC.md](TECH_SPEC.md) for normative behavior and acceptance criteria. `itub` is a working directory name, not a final product name.

## Product brief

Build a free desktop video catalogue for Windows and Linux. Users add folders, browse recursively discovered videos, scrub thumbnail previews, search and filter, assign ratings and tags, and open videos in their system's default player. Performance is the first product priority. Source media must remain untouched unless the user explicitly requests a file operation.

Take interaction inspiration from [Video Hub App](https://videohubapp.com/en/guide/) and [MediaChips](https://github.com/fupdec/MediaChips), which document visual previews and metadata browsing. Build an independent implementation; their code, artwork, and branding are not implicitly licensed for reuse. “Faster than these apps” is a benchmark objective, not an established fact.

## Confirmed direction and remaining defaults

The owner clarified the following after the initial draft:

| Decision | Agreed direction | Implementation consequence |
| --- | --- | --- |
| License | Keeping distributed copies/derivatives open source matters most | Use GPL-3.0-only; sales remain permitted; official binaries are free |
| Scale/storage | Approximately 10,000 videos on SSD | 10k remains the product target; the owner runs large-library validation |
| Agent testing/privacy | At most 20 test-video copies; real catalogue stays private | Agent tests use only the supplied fixture and a bounded disposable corpus; larger runs belong to the owner |
| Hover | Prefer high time-resolution scrubbing from the original file; cached stills acceptable if live seeking is too costly | Prototype one paused native player; provide instant cached feedback and bounded original-file seeking |
| Tags | Filename/folder tags first; consider local AI feasibility and hardware needs | Ship deterministic tags; document a separate optional AI experiment |

Other explicit defaults: one local catalogue containing multiple roots; Windows 11 x64 and Ubuntu 24.04 x64 as initial release test targets; no account, telemetry, cloud service, or required Internet connection. The initial view count means launches requested through this app, not verified watches. Database/cache live on a local SSD. HDD/removable/mounted-NAS media must behave safely, but SSD is the performance reference. These defaults do not require another approval before implementation.

### License

Implement under **GPL-3.0-only** with the unmodified license text, correct project copyright notices, and GPL-compatible dependencies. Distributed modified/combined derivative applications must meet GPL requirements, including corresponding source and continued licensing freedoms. Mere aggregation with unrelated programs is different. Private modifications need not be published merely because they exist. The license permits charging for copies; the project will offer its own binaries free. See the [GPLv3 license](https://opensource.org/license/gpl-3-0) and [OSI FAQ](https://opensource.org/faq).

The implementing agent should add LICENSE and source headers/metadata as appropriate. The owner has selected the copyleft direction; a custom no-sales license is no longer a release dependency. GPL-3.0-only is the specific recommended form, avoiding an unrequested future-version grant.

Use dependencies whose terms permit this application license. Prefer dynamically linked Qt modules and include the required notices and source/build materials for the exact license option used; check module and transitive-component terms in [Qt's licensing documentation](https://doc.qt.io/qt-6/licensing.html). Distribute a documented, redistributable FFmpeg build and required materials; build flags and optional components change its licensing. See [FFmpeg's legal guidance](https://ffmpeg.org/legal.html). The application GPL does not replace independently licensed dependencies' terms.

## Recommended implementation

One shared C++20 codebase, Qt 6 Quick UI, Qt Multimedia for a single active paused hover player if its benchmark passes, SQLite catalogue, and FFmpeg/ffprobe subprocesses for indexing/cache extraction. Use CMake and native CI builds per operating system. Pin exact supported dependencies during the first milestone; do not rely on whatever a runner currently installs.

This is an engineering recommendation, not a claim that C++ automatically beats other stacks. It gives the agent a direct route to bounded native data structures, a GPU-rendered grid, platform dialogs, and worker isolation without a web server or bundled browser. Prove the result with measurements. Avoid separate Windows and Linux application implementations unless a platform API actually requires a small adapter.

The expensive work is discovery, video probing, decoding, searching, and image upload. Keep it off the UI thread, bound concurrency and caches, and prioritize visible results. Use subprocess extraction for background caches. Use one shared native player for original-file hover seeking, never one player per card. Verify paused seeks actually deliver suitably timed frames on both platforms; cached stills are the fallback for slow or unsupported media.

### Why C++ rather than Rust here?

Rust is a credible performant choice, and its compiler-enforced [ownership](https://doc.rust-lang.org/book/ch04-01-what-is-ownership.html) and [concurrency rules](https://doc.rust-lang.org/book/ch16-00-concurrency.html) prevent classes of memory and data-race bugs in safe Rust. That is valuable for agent-written code. It does not prevent logical errors such as deleting the wrong path, incorrect reconciliation, or stale search results; those still need the same contracts and tests. Native dependencies and unsafe interfaces also retain their own risks.

Keep C++20/Qt as the current recommendation because this design directly uses Qt's grid/model, platform integration, and paused multimedia APIs. Rust can retain Qt through [CXX-Qt](https://kdab.github.io/cxx-qt/book/), but that adds a language boundary, generated bindings, and Rust/C++ build integration. A Rust-native GUI is another viable route, with its own grid, accessibility, and media integration to validate. Neither route is disqualified on performance; the recommendation favors direct integration for this feature set, not a claim that Rust is unsuitable or slower.

Do not introduce a mixed-language core merely to split the difference. Use the C++ ownership and sanitizer requirements in TECH_SPEC.md. If the owner later prefers Rust, revise the UI/media/build choices together before implementation rather than replacing language names in this spec.

## Execution sequence

### 0. Establish the baseline and release constraints

Deliver a minimal CMake project, pinned dependency/build manifest, a window with a virtual grid, and a headless benchmark command. Confirm a clean checkout builds on both target systems. Add the standard GPLv3 license and verify dependency redistribution requirements before substantial work.

Prepare reproducible small and large benchmark modes, but agents execute only the small corpus defined below. Measure grid interaction, native model memory, search, startup, and preview decoding with at most 20 test entries. Benchmark one QMediaPlayer/QVideoSink doing paused seeks; any generated long-GOP/4K clips must fit the same corpus and disk limits. Save machine specifications and aggregate measurements. Hand the owner the 10k synthetic and real-library benchmark instructions without running them. If native paused seeking fails its small-corpus gates, record the concrete downside and ship cached scrubbing as permitted by the owner; do not build a custom player before profiling. Do not infer large-library performance from these checks.

Exit: both builds run, small-corpus benchmark commands are repeatable, file-safety design has an executable check, and large-scale validation is explicitly assigned to the owner.

### 1. Catalogue storage and safe discovery

Implement the versioned SQLite database, app-owned storage, folder picker, recursive enumeration, incremental reconciliation, cancellation, metadata extraction, error states, and offline-root handling. Display discovered files before extraction finishes. Add resumable job state; preserve annotations across refreshes.

Exit: repeated scans create no duplicate rows or needless probes; interrupted/unreadable scans never imply mass deletion; source contents and app-controlled source metadata remain unchanged. Corrupt files do not stop the scan.

### 2. Browsing, posters, and timeline previews

Implement paged card details, a virtual grid, lazy posters, prioritized storyboard generation, bounded disk/RAM caches, pointer-to-time mapping, and one shared paused original-file hover session. Add keyboard navigation, accessible card actions, progress, pause/resume, and missing-file placeholders.

Exit: browsing works during scans, cached fallback changes require no source reads or subprocesses, original-file seeking has bounded work and no audio, stale jobs cannot attach images to a replaced video, and budgets hold with decoding active. Cached previews of disconnected media remain usable.

### 3. Search and metadata editing

Implement live fuzzy search, all required structured filters and sorts, deterministic filename/folder/technical tags, manual tags, suppressed automatic tags, ratings, default-player launch, and view counters. Persist edits immediately and refresh search state after successful transactions.

Exit: combined filters return the same complete result set as the reference test; typo and Unicode examples pass; rescans do not resurrect suppressed tags or lose ratings; launch semantics match the spec.

### 4. Explicit file operations and recovery

Implement clearly separate “Remove folder from catalogue” and “Move video to Trash/Recycle Bin” actions. Add catalogue backup/export/restore and cache controls. Test changed paths, file replacements, read-only sources, locked files, unavailable trash, full disks, and interrupted database writes.

Exit: all source mutations go through one reviewed action path with fresh user confirmation; failed trash operations do not fall back to permanent deletion. A backup preserves all annotations when restored to a fresh profile.

### 5. Release validation and GitHub handoff

Minimize end-user setup: Windows users must not separately install Qt, FFmpeg, SQLite, a compiler, or an SDK. Bundle the required runtime libraries, QML modules, plugins, and media tools with the application. Use Qt's [Windows deployment tooling](https://doc.qt.io/qt-6/windows-deployment.html) and check additional third-party dependencies separately. Bundle only used runtime components, not the Qt development environment.

Provide a portable Windows ZIP if the chosen toolchain can satisfy all prerequisites on a clean supported Windows installation. Otherwise provide one offline installer that includes and handles any required official compiler-runtime redistributable; never send users to download missing DLLs or install development tools. Do not label an archive fully portable if it depends on an already-installed Visual C++ runtime. Decide this in milestone 0 and test the actual package, including paused-preview backend dependencies. A package may contain an executable and private DLL/plugin folders; a single-file executable is not required.

For Linux, bundle non-system application dependencies where practical and document the supported base-system requirements and default-player integration. Begin with a portable archive; add AppImage/other packaging only if it materially reduces setup on the supported target. Users need an OS-associated video player for external playback; the catalogue itself must not require VLC or another specific player. Record compressed download and installed sizes in release results.

Deliver source, build instructions, dependency notices/licenses, an owner-approved project license, privacy/file-safety documentation, a short user guide, changelog, checksums, and benchmark results. Exclude private media, generated previews, local databases, and absolute personal paths from Git and artifacts. Publication is a later task; this planning task does not create a public repository or upload the provided video.

Exit for agent handoff: clean-machine smoke tests, file safety, and applicable small-corpus performance checks pass on available target systems. Large-library release gates remain pending owner validation; this does not authorize agents to expand the corpus or inspect private media. Record any unsupported environment; do not mark an untested Windows path as verified from Linux.

## Validation strategy

Use CTest and small native checks, plus Qt's existing test facilities for UI/model behavior where useful. Keep tests centered on failure-prone contracts: source writes, reconciliation, cancellation, normalization, filter completeness, persistence, and timestamp mapping. Do not add a separate test framework just for this project.

### Agent media limits and privacy boundary

Agents may read the supplied `testvideo1.mp4` and create **at most 20 derived test-media files in total**, counting ordinary copies, reflinks, and generated/modified variants together. The untouched original is outside that derived-file count. Use at most 20 entries in an agent-run test catalogue. Reuse this small corpus across checks; do not cycle through a larger collection to evade the limit. Twenty full copies of the supplied file use 645,962,580 bytes (about 616 MiB). Keep all generated test media, test databases, previews, and logs within a combined 1 GiB budget; build dependencies/artifacts are separate. Enforce the file-count and output-byte limits during creation, clean up only tracked test-owned artifacts, and do not accumulate separate corpora across runs.

Agents MUST NOT search for, enumerate, scan, open, or preview the owner's real videos, library directories, catalogue databases, thumbnail caches, backups, or recent-file history. Do not obtain filenames, tags, hashes, screenshots, or file-level logs from that library. Testing must use a fresh disposable application profile with explicit fixture roots and no access to the owner's existing profile. This boundary applies to helper processes and GUI automation as well as direct tools. Product access to private media during the owner's independent use is not permission for an agent to inspect it.

Leave all larger catalogue runs, including synthetic 10k/optional 100k runs, to the owner. Provide opt-in commands/instructions that the owner runs outside agent tools. No real-library path may be hardcoded or auto-discovered by the benchmark. Results intended for sharing contain aggregate counts, timings, memory, and sanitized errors only; no paths, names, tags, frames, or media hashes. Logs remain local, and the owner decides whether to share a short summary. Never request a library dump to fill an untested performance cell.

The workspace contains only `testvideo1.mp4` (32,298,129 bytes). Read-only ffprobe inspection found H.264 video at 1920×1080, 50 fps, video-stream duration 667.500 seconds, container duration 667.573696 seconds, AAC audio, and a subtitle stream reporting a longer duration. This is useful for verifying that the preview timeline follows the video stream.

During implementation, make ordinary copies or copy-on-write reflinks within the limits above into a newly created temporary fixture directory; never hard-link media for mutation tests. Include names such as `Summer_Trip-2024_1080p.mp4`, `summer.trip.final.mp4`, `Café 東京 01.mp4`, and a filename containing quotes and shell metacharacters. Nested directories test folder tags. Do not rename or delete the original fixture.

Copies exercise names, paths, counts, and scanning, but do not represent diverse decode costs. Allocate some of the 20 fixture slots to short generated videos with portrait rotation, variable frame rate, no audio, differing dimensions/durations/codecs, multiple video streams, and truncated content. Bound generator duration, resolution, bitrate, and output size. Leave cases that cannot fit to owner testing and identify the gap. Larger synthetic DB runs avoid copying videos but still belong to the owner under this testing split. Duplicate hot files are not representative of a real library or cold storage.

GUI automation must use a disposable virtual display or dedicated disposable desktop. Follow the supplied AGENTS.md instructions: discover available tools, choose a free display, keep it alive through the test, clean it up, and never attach automation to the user's real desktop. If unavailable, run non-GUI checks and report the missing GUI coverage.

## Completion checklist for the implementing agent

- Every required feature in TECH_SPEC.md has a working, tested user path.
- No automatic source write, rename, embedded-tag update, sidecar, or media-directory cache exists.
- Agent results identify their ≤20-entry corpus, cold/warm conditions, background work, memory, cache size, and OS coverage; owner-only large-scale results are marked pending until supplied.
- Test-media limits and privacy boundary are enforced; the real catalogue never enters agent tools or context.
- Confirmed choices and reviewable defaults remain distinguishable; precise-scrubbing fallback has benchmark evidence if used.
- Delivered binaries work without a developer environment and use the system default player.
- Database upgrades and backups preserve ratings, tags, exclusions, and view counts.
- The final handoff explains what runs, how to build/test it, measured limitations, and remaining release blockers.

## Deferred unless requested

Cloud sync, accounts, mobile/macOS releases, media conversion, embedded video metadata editing, automatic source renaming, duplicate deletion, plugins, custom metadata schemas, semantic search, and production content AI are outside the initial release scope. High-resolution paused hover seeking is in scope, with the explicitly permitted cached fallback. Assess optional AI using TECH_SPEC.md section 13 before promising hardware requirements or adding a runtime/model dependency. Directory watching can follow manual/startup refresh if demand justifies its OS-specific complexity; scans must work without it.
