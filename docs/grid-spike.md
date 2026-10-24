# Grid spike: 100k items at 120 Hz

Measured on 2026-10-24 on the M2 Pro's built-in display (3456×2234 at 120 Hz,
scale 2), Asahi with Mesa 26.3, Qt 6.12, in a 1704×1058 window. The library
is synthetic (`pinhole-fake`): 100,000 items with 480 px JPEG thumbnails of
60 KB each, the same size as the real iCloud ones. Run it with:

```bash
app/bin/build && app/build/pinhole --bench
```

## Result

At 120 Hz a frame has 8.33 ms. "Missed" counts frames over 12.5 ms.

| Phase | fps | p50 | p99 | max | missed | placeholders |
|---|---|---|---|---|---|---|
| Idle at the newest | 121 | 8.3 | 8.7 | 10.1 | 0 | 0% |
| Scroll 1,500 px/s | 120 | 8.3 | 9.3 | 29.6 | 2 | 0% |
| Scroll 5,000 px/s | 120 | 8.3 | 9.2 | 10.4 | 0 | 3% |
| Scroll 15,000 px/s | 120 | 8.3 | 8.9 | 10.8 | 0 | 10% |
| Three flings at 30,000 px/s | 119 | 8.3 | 9.2 | 57.4 | 2 | 4% |
| Scrub all 100k in 5 s | 120 | 8.3 | 9.0 | 39.9 | 2 | 98% |
| Random jumps | 120 | 8.3 | 9.1 | 22.4 | 2 | 2% |
| Pinch sweep 8 → 30 → 3 → 8 columns | 119 | 8.3 | 12.2 | 36.3 | 8 | 0% |
| Scroll 15,000 px/s at 30 columns | 120 | 8.3 | 10.2 | 24.9 | 4 | 11% |
| Zoom steps 8 ↔ 14, animated | 118 | 8.3 | 9.1 | 54.0 | 3 | 0% |

- After a jump to anywhere in the library, every visible cell is sharp after
  **44 ms** (median), 63 ms at worst.
- The grid's own work per frame (building the scene graph) is 0.02–0.1 ms
  typically and under 3.5 ms at the 99th percentile in every phase.
- **Baseline:** redrawing an unchanged frame for 30 s still missed one frame
  (32 ms, in the swap). The system alone stalls about once every 30 s; the
  phases above miss 0–8 frames over 4–6 s each.

## How the grid works

- Positions are arithmetic on (columns, scroll, visual scale), done in
  screen space each frame. Jumping anywhere costs the same as one row.
- Thumbnails are decoded by libjpeg-turbo on 8 threads, at full, 1/2, 1/4 or
  1/8 size (the smaller ones nearly free, by DCT scaling), picked from the
  cell's size on screen and the scroll speed.
- The grid tells the decoders every frame what it wants, most wanted first,
  replacing the previous list: anything that scrolled away is dropped.
- Textures go into Qt's 4096² atlas pages. All cells on one page are a single
  geometry node, and cells still waiting show their average colour, all in one
  more node: a settled screen is 2 draw calls.
- Uploads are capped at 8 MB per frame, and decoded thumbnails whose cells
  have meanwhile left the screen are not uploaded at all.
- Texture memory is capped at 1 GB, freed 48 textures per frame at most.
- Zoom changes the column count in steps with a continuous visual scale in
  between, around the pointer. Steps (Ctrl+wheel, +/−) glide cells to their
  new places over 220 ms; a pinch reflows at once, because gliding on every
  column change kept every cell permanently in flight.

## What changed along the way

| Change | Effect |
|---|---|
| Vulkan instead of OpenGL | zoomed-out fast scroll 96 → 119 fps; worst frame 385 → 66 ms |
| Drop decoded thumbnails that scrolled away; budget uploads in bytes | zoomed-out fast scroll: placeholders 100% → 14% |
| Uploads 24 MB → 8 MB per frame | fewer GPU stalls after jumps (one frame of 80 ms → ~20–30 ms) |
| Gradual eviction | removed a 13 ms frame that freed 859 textures at once |
| Capping uploads at 32 per frame | no gain, and 98% placeholders zoomed out: not kept |
| Atlas pages 2048 instead of 4096 | no difference |

The remaining slow frames spend their time in the render pass or the swap,
not in Pinhole's code, often in frames that changed nothing: GPU and driver
time on Asahi.

## The real library

`pinhole-sync all` built it in 11 minutes: the index in 5 (49,018 assets read
with overlap, 48,950 kept, exactly iCloud's count) and all 48,949 thumbnails
in 6 (137/s sustained, 3.1 GB, no failures, no throttling). Thumbnails are
named by iCloud's checksum, so a rebuild reuses them: the second run
downloaded nothing.

The same benchmark on it, in a half-width window (845×1058, sharing the
workspace), warm and then with every thumbnail and the index evicted from the
page cache:

| Phase | warm fps / missed | cold fps / missed | cold placeholders |
|---|---|---|---|
| Idle | 121 / 0 | 121 / 0 | 0% |
| Scroll 1,500 / 5,000 / 15,000 px/s | 118–120 / 0–2 | 119–120 / 0–2 | 0–11% |
| Three flings | 119 / 1 | 119 / 2 | 4% |
| Scrub all in 5 s | 120 / 0 | 120 / 2 | 81% |
| Random jumps | 120 / 1 | 118 / 7 | 2% |
| Pinch sweep | 120 / 6 | 117 / 19 (3%) | 0% |
| Scroll 15,000 px/s at 30 columns | 119 / 3 | 109 / 41 (10%) | 12% |
| Zoom steps | 120 / 0 | 120 / 2 | 0% |

- Loading the index cold: 48,950 items in 469 ms.
- Jumps cold: all visible cells sharp after 46 ms median, 78 ms at worst.
- Cold costs show only where decoding is heaviest (pinch sweep, zoomed-out
  fast scroll). The tiny-thumbnail tier below would take both off the disk.

## Not done or not verified

- **Real input.** Scrolling, inertia and pinch were driven by the script. The
  touchpad (pixel deltas, release velocity) and pinch gesture code is written
  but untested by hand.
- **Scrubbing shows colours, not pictures.** At 20 screens a second nothing
  can be decoded in time; 98% of cells show their average colour. Apple shows
  tiny pictures there. That needs a pre-built tier of tiny thumbnails (say
  64 px) packed into pages, so zoomed-out and scrubbing views need no
  per-cell decoding or uploading. The same tier would fix the 11% placeholders
  in fast scrolling at 30 columns.
- **Omarchy conventions** skipped for the spike: theme colours, translations,
  `?` help, the date scrubber, badges for video, Live Photo and favourites.
- The animated zoom step is one glide at a time; a step during a glide
  starts from the old layout, not from where the cells are.

## Input and zoom (second round)

Tuned against recorded touchpad input (`PINHOLE_INPUT_LOG=<file>`) and a
screen recording of Photos on an iPhone.

- **Scrolling** (`app/src/scrollphysics.*`, plain C++): the content follows
  the fingers with a Mac-like acceleration curve on top of Hyprland's
  `scroll_factor` (base gain 1.3, `PINHOLE_SCROLL_GAIN` overrides), coasts
  after a flick with AppKit's 0.998/ms deceleration, builds speed on repeated
  flicks, and stretches and springs back at the ends. Releases under
  300 px/s do not coast. `app/tests/scroll_replay.cpp` replays a recorded log
  through it.
- **Stopping a coast** with a resting finger uses libinput's hold gesture,
  which Hyprland sends through the pointer-gestures protocol but Qt does not
  bind; `app/src/touchpadhold.*` binds it on Qt's Wayland connection, on a
  queue of its own. Clicks and tap-to-click stop it too.
- **Zoom** has fixed levels (1, 2, 3, 4, 6, 8, 11, 15, 21, 29, 40 columns).
  A pinch never reflows under the fingers: the two levels around the
  current cell size are drawn scaled to it and cross-faded, the denser one
  underneath because, scaled up, it always covers the window. The photo
  between the fingers keeps its height; across, and near the library's ends,
  each level is kept edge to edge, so no empty band ever opens. Release
  settles on a level (pinch speed counts) in 0.3 s.
- **Mosaic pages** (`thumbnailloader.*`): below 100 device px per cell, and
  while scrubbing, cells come from pages of 256 photos as 64 px tiles in one
  1024 px texture, built on the decoder threads. At 40 columns this cut
  uploads from 23 to 0.2 per frame and decodes from ~2,500 to ~15 a second;
  scrubbing the whole library went from 93% placeholders to 3%.

Checked offscreen by the benchmark: the pinned photo moves 0 px vertically
through a pinch sweep, and no frame leaves part of the window uncovered
(also when pinching at the newest end). Not yet measured at 120 Hz on
screen, and the new zoom has not been tried by hand.
