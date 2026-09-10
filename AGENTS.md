# ScrubTub maintenance

The application is in good shape. Make focused fixes and small adjustments;
preserve working behavior and the existing interface unless asked to change it.
Use the code and tests as the implementation reference, README.md for build/use,
and docs/DEPENDENCIES.md when working on dependencies. docs/archive/ is historical,
not active instructions; do not read it by default or revive its milestone plans.

- Stack: C++20, Qt Quick/QML, SQLite, FFmpeg/ffprobe, and mpv live previews.
  Keep mpv; cached previews provide fallback. Preserve existing catalogue data
  and compatibility with older iTub profiles.
- UI: retain the dark, minimal interface and existing QML components, colors,
  spacing, keyboard access, and focus behavior. Read the relevant components
  rather than maintaining a duplicate design specification.
- Source media stays unchanged. Ratings, tags, databases, logs, and previews
  belong in app-owned storage. The product's explicit, confirmed Move-to-Trash
  action must never fall back to permanent deletion.

## Testing

- Use this repository's vids/ folder directly, as-is, for testing runs. Do not
  copy, reflink, hard-link, rename, move, delete, or generate derivatives of the
  test videos to construct a fixture corpus. Do not write test artifacts into
  vids/. Generated application previews belong in a separate test cache.
- Use a disposable application profile outside vids/ for databases, settings,
  caches, and logs. Do not use the owner's existing catalogue or other libraries.
- Media-dependent suites read vids/ in place through
  tests/testsupport/MediaFixtures.h (vidsDir/vidsFiles/smallestVideo/...). Point
  them at the corpus with `-DSCRUBTUB_TEST_VIDS_DIR=<path>` at configure time or
  the same-named environment variable at run time; they skip with a message when
  it is missing. Expectations must come from the directory listing or from
  ffprobe for the same file, never from copied fixtures or hard-coded durations.
- Cases that must mutate media (trash, truncated or appended files, offline
  roots, controlled folder trees) generate their own ORIGINAL clips with
  MediaFixtures::makeSyntheticVideo() and mutate only those. Never mutate,
  truncate or remove a file from vids/.
- Suites that read vids/ assert it is unchanged (vidsFingerprint() compared
  between initTestCase and cleanupTestCase).
- Run checks appropriate to the change. Documentation-only edits need no app run.
- GUI automation requires an isolated virtual display or an explicitly dedicated,
  disposable GUI session. Never attach to or interfere with the user's desktop.
  Discover available tools and choose a free display; keep it alive through the
  test and clean up afterward. Do not assume DISPLAY, XAUTHORITY, D-Bus, XDG
  runtime directories, or GUI tools are available. Host desktop/session metadata
  is read-only. If isolation is unavailable, use non-GUI checks and report the gap.
