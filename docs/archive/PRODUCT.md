# ScrubTub

<!-- impeccable:product-schema 1 -->

## Platform

desktop

Native Windows and Linux application built with C++20 and Qt Quick/QML.
Impeccable's web/iOS/Android platform categories do not describe this project;
preserve the desktop stack and desktop interaction conventions.

## Users

Archivists cataloguing and maintaining video libraries. This primary audience
was confirmed by the owner during initialization.

## Product Purpose

Help archivists discover, identify, organise, and revisit videos in their
libraries. The catalogue is the central workspace: users browse thumbnails,
scrub to inspect content, search and filter, and maintain tags and ratings.

## Operating Context

The expected archive scale is a few thousand videos, confirmed by the owner.
Shared catalogues and multi-user collaboration are not required and are out of scope.

The existing application catalogues videos discovered recursively in folders.
Users inspect previews within the catalogue and open videos in the system's
default player. Folder reconciliation, availability information, catalogue
backups, and persistent annotations support library maintenance.

## Capabilities and Constraints

The following implementation facts are documented in README.md and the source:

- Search, sorting, metadata filters, ratings, tags, and video details.
- Folder discovery and scan controls, with optional checking on startup.
- Catalogue backup and restore, and generated-preview cache management.
- Source media remains unchanged except for an explicit, confirmed
  Move-to-Trash operation. There is no permanent-delete fallback.
- Existing iTub catalogue profiles are reused to retain libraries and annotations.
- The project is free software under GPL-3.0-only; see LICENSE.

The owner explicitly requires mpv for its preview performance. The rejected
Qt Multimedia implementation has been removed. Cached previews cover cases
where mpv is unavailable or cached-only mode is selected.

## Brand Commitments

- Product name: **ScrubTub**.
- The owner prefers a dark, minimalistic, clean interface.
- Infrequently changed settings may live in a settings menu or dialog.
- Further UI decisions are delegated within these constraints.

## Evidence on Hand

- README.md: current product, build, and workflow documentation.
- src/ui/: the implemented Qt Quick interface.
- tests/: native checks for catalogue operations, file safety, search,
  previews, and related behavior.
- docs/BENCHMARKS.md and docs/HANDOFF.md: measured preview results and
  outstanding validation. Small-corpus results do not establish large-library
  performance.
- PLAN.md and TECH_SPEC.md: specification history; their Qt Multimedia
  prototype direction is superseded by the owner's mpv decision.

## Product Principles

1. Keep cataloguing and library maintenance central to product decisions.
2. Make visual identification fast through responsive browsing and scrubbing.
3. Protect source media and preserve catalogue annotations.
4. Keep frequent work easy to reach and occasional configuration out of its way.

## Open Decisions

Preservation standards and product-specific accessibility requirements have
not been established. Do not infer these requirements from the audience label.
