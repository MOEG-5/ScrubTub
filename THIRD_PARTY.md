# Third-party software

ScrubTub is licensed under GNU GPL version 3 only. Its dependencies retain their
own copyright notices and applicable licenses; they are not claimed as original
ScrubTub code.

Binary releases include a generated `DEPENDENCIES.md`, `DEPENDENCIES.json`, and
`licenses/` directory beside this file. The inventory identifies the exact
packages shipped, including Qt plugins and transitive runtime libraries.

The companion **source archive** on the same GitHub release contains:

- The matching ScrubTub source, including packaging and build scripts.
- FFmpeg, mpv, dav1d, libplacebo and RapidFuzz source archives.
- Matching distribution source packages, including their patches and build rules,
  for every additional bundled dependency.
- Dependency versions, source checksums, and the configurations used for the
  private media builds.

You may replace the shared libraries and media tools with compatible modified
versions. ScrubTub imposes no additional restrictions on modification, reverse
engineering for debugging those modifications, or redistribution permitted by
the relevant licenses. The source archive explains how to rebuild the software.

## Principal components

| Component | Applicable open-source terms |
| --- | --- |
| Qt | LGPL-3.0 / GPL-3.0, with separately licensed third-party code; see the complete Qt notices |
| FFmpeg / ffprobe | GPL-3.0-or-later for the configured release build; individual source files retain their notices |
| mpv | GPL-2.0-or-later sources, distributed under GPL-3.0-or-later when combined with this FFmpeg build |
| dav1d | BSD-2-Clause |
| libplacebo | LGPL-2.1-or-later |
| RapidFuzz C++ | MIT |
| SQLite | Public domain; see the bundled package notice |
| libass | ISC and included component notices |
| LuaJIT | MIT and included component notices |

These are summaries, not replacements for the full notices. Embedded code inside
Qt and other dependencies is covered by the respective component notices and
source packages. Debian notices may refer to the included `licenses/common-licenses/`.

## Reference material in the source repository

`idea/thumbfast-master/` is third-party reference material under MPL-2.0. Its
original source header and LICENSE are preserved. It is not installed into the
ScrubTub application bundle. Do not interpret the root GPL license as relicensing
those reference files.
