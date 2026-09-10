---
name: ScrubTub
description: A quiet screening-room catalogue for video archives.
colors:
  amber: "#ddbe8b"
  amber-pressed: "#b99b6c"
  amber-text: "#e2c18c"
  selection: "#343027"
  canvas: "#191b19"
  rail: "#141613"
  panel: "#20221e"
  field: "#20221f"
  button: "#252724"
  button-hover: "#2e302d"
  button-pressed: "#373834"
  ivory: "#eeeee9"
  button-text: "#d1d0cc"
  muted: "#a4a69e"
  placeholder: "#9d9f97"
  field-border: "#373a34"
  image-border: "#31342e"
  image-well: "#10120f"
  ink: "#191918"
  error: "#edaaa0"
typography:
  headline:
    fontSize: "21px"
    fontWeight: 600
  title:
    fontSize: "17px"
  body:
    fontSize: "13px"
  filename:
    fontSize: "13px"
    fontWeight: 500
  label:
    fontSize: "12px"
  metadata:
    fontSize: "11px"
rounded:
  overlay: "4px"
  control: "6px"
  dialog: "10px"
spacing:
  tight: "4px"
  small: "8px"
  control: "12px"
  panel: "16px"
  section: "24px"
  workspace: "28px"
components:
  button-primary:
    backgroundColor: "{colors.amber}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    typography: "{typography.body}"
    height: "36px"
  button-secondary:
    backgroundColor: "{colors.button}"
    textColor: "{colors.button-text}"
    rounded: "{rounded.control}"
    height: "36px"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.button-text}"
    rounded: "{rounded.control}"
    height: "36px"
  navigation-selected:
    backgroundColor: "{colors.selection}"
    textColor: "{colors.amber-text}"
    rounded: "{rounded.control}"
    height: "36px"
  input:
    backgroundColor: "{colors.field}"
    textColor: "{colors.ivory}"
    rounded: "{rounded.control}"
    height: "36px"
  tag:
    backgroundColor: "{colors.button}"
    textColor: "{colors.ivory}"
    rounded: "{rounded.control}"
    typography: "{typography.metadata}"
    height: "28px"
  video-card:
    backgroundColor: "transparent"
    textColor: "{colors.ivory}"
---

# Design System: ScrubTub

## Overview

**Creative North Star: "The Screening Room"**

Warm charcoal surrounds the archive's own imagery. Ivory labels, muted metadata, and amber interaction states keep the catalogue calm and legible. Flat thumbnail shelves give each video space without wrapping its filename and metadata in a box.

This is a native Qt Quick desktop interface. The source of truth is `src/ui/Main.qml`, `VideoCard.qml`, and `ActionButton.qml`; sidecar HTML/CSS examples are illustrative translations for a documentation panel, not application implementation. Its generated tonal ramps are palette previews, not additional application tokens.

**Key Characteristics:**

- Warm dark surfaces with restrained amber emphasis.
- Flat image shelves and detached metadata.
- A stable filter sidebar and a tabbed bottom inspector.
- Compact native controls and brief state feedback.

## Colors

Amber is the primary accent; the surrounding neutrals lean warm and slightly green.

- **Primary:** `amber` marks focus, selection, rating, the scrub cursor, and the inspector's Open video action. `amber-pressed`, `amber-text`, and `selection` describe its interaction states.
- **Neutral:** `canvas` holds the catalogue; darker `rail` anchors navigation; `panel` separates the inspector and settings. `field` and the button shades identify controls without elevation.
- **Text:** `ivory` carries content, `button-text` carries actions, and `muted` carries supporting facts. `placeholder` is reserved for placeholders and quiet navigation labels. `ink` provides dark text on amber actions.
- **Boundaries:** `field-border` and `image-border` provide fine edges; `image-well` accommodates letterboxing. `error` identifies operation and search failures.

**The Amber State Rule.** Use amber to identify an action or state, not to decorate empty space.

## Typography

Qt supplies the platform font family; QML does not pin or bundle a typeface. Preserve native text rendering rather than introducing a display-font dependency. The recorded sizes are QML logical pixels, not CSS layout requirements.

The current view selector uses 21-pixel semibold type in the sidebar; section headings use title, sometimes with semibold weight. Controls and ordinary content use body, filenames use medium weight, and secondary facts use label or metadata. Long filenames elide in the grid and inspector; the inspector exposes the full name on hover. Do not impose a fixed line height absent from the implementation.

## Layout

The default window is 1440 × 900, with a 960 × 640 minimum. A sidebar starts at 264 wide and resizes through a native 6-wide SplitView handle, between 232 and 480 while reserving at least 420 for the catalogue. It holds the current view selector, count, three-line sort menu, fuzzy search, tag inclusion/exclusion, and three range filters. Its contents scroll at shorter window heights; reset, Add folder, and Settings stay anchored below it. Resolution and minimum views follow directly in the same scroll area. The sidebar hide control and Ctrl+B toggle visibility; a footer button restores it, and Ctrl+F also reveals search.

The catalogue starts with thumbnails: no heading or toolbar above the grid. Thumbnail wells have zero left/right margins, including between columns; filenames and metadata keep 8-pixel horizontal insets. Cards retain 8-pixel top and bottom insets. It divides available width into equal cells using a default target size of 280; it retains at least two columns when its available width reaches 420, otherwise one. Cell height is `round(cellWidth × 0.625) + 70`. Posters preserve their original aspect ratio inside the well.

Details, Tags, and File info form a 48-high bottom tab strip only when a video is selected. The tabs and their divider disappear entirely without selection. Selecting a video opens a 256-high panel, reduced to 220 below a window height of 760. Its poster column is 240 wide, or 176 below a window width of 1100. The remaining width holds the filename, Open video action, and scrollable tab content. Hiding the panel retains selection; choosing a tab opens it again. The 36-high footer keeps the scrubbing hint, thumbnail-size control, and sidebar restoration within reach. Settings retain their centered, nonmodal 560-wide dialog with height capped at 680 or the window height minus 60.

## Elevation & Depth

Custom surfaces use no shadows. Tonal differences, fine seams, and one-pixel image or focus borders separate regions. Controls change color over 90 ms. Mouse-wheel impulses feed Qt’s native flick motion, with 2200 logical pixels/s² deceleration and a 6500 pixels/s velocity cap. Trackpads retain native pixel scrolling. Hover previews pause while the grid moves, then resume on settling. Layout changes do not animate. Qt Basic controls retain their native implementation for menus, sliders, checkboxes, and standard dialog buttons.

## Shapes

Controls and thumbnail outlines share the control radius. Time badges use the smaller overlay radius, and settings use the dialog radius. Card bodies themselves remain square, transparent, and borderless. SVG line icons accompany actions; do not substitute text glyphs for those icons.

## Components

- **Actions:** `ActionButton` is 36 high with 12 horizontal padding, 17-pixel icons, and a 9-pixel icon/text gap. Primary actions use amber; ordinary buttons use the button surface; flat buttons are transparent at rest. Press, hover, and selection change the fill. Keyboard focus adds an amber border. Disabled backgrounds use 0.45 opacity; icon-only controls have accessible names and delayed tooltips.
- **Inputs and selects:** compact controls share the control radius. Inputs show a persistent fine border that turns amber on focus. Search uses the same bordered field as tag filters. Selects reserve room for an SVG chevron and show a focus border.
- **Navigation:** a labeled native select chooses the library view or folder. Bottom text tabs use the selection fill and amber text; avoid cryptic icon-only toolbars.
- **Tags:** 28-high action chips wrap with a 4-pixel gap. Activation opens tag actions; suppressed tags use muted text plus the explicit “(suppressed)” suffix. Tag entry is a text field, with Enter to add.
- **Video shelves:** imagery sits above the filename, resolution, size, and optional rating. A fine amber outline identifies the selected thumbnail. Hover reveals cached or live frames, a three-pixel time cursor, and a timestamp; approximate cached timestamps include “≈”. Leaving restores the poster. A 120 ms dwell engages the live preview session.
- **Range filters:** paired Min/Max text fields and a native two-handle RangeSlider share each filter group. Tracks are 4 high with 14-pixel circular handles. Duration uses seconds, size uses MiB, and rating spans 0–5 with 0 including unrated videos. Duration and size scales follow the unfiltered collection; typed bounds can exceed the scale. Keyboard focus scrolls sidebar and inspector controls into view; Ctrl+F reveals search. Blank maxima are unlimited, invalid ranges show inline feedback, and slider handles support keyboard adjustment.
- **Inspector:** a horizontal poster and keyboard-operable timeline sit alongside the active tab. Details exposes rating and common metadata; Tags exposes tag actions and entry; File info exposes the path, codec, and status. Occasional video actions stay in the Video options menu.
- **Empty states:** centered explanatory text distinguishes a new archive from a search with no matches; only the new archive offers Add folder.

## Do's and Don'ts

- **Do** preserve original video aspect ratios within image wells.
- **Do** keep visible focus, accessible action names, and keyboard equivalents for browsing and scrubbing.
- **Do** use the existing native controls and shared ActionButton for new actions.
- **Don't** add decorative card shells or shadows around the thumbnail shelves.
- **Don't** expose infrequent settings in the main browsing toolbar.
- **Don't** replace interface SVG icons with glyph approximations.
