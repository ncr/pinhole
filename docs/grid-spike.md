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
- **Cold disk.** The thumbnails were freshly written and largely in the page
  cache; a cold start reads them from the SSD.
- **Omarchy conventions** skipped for the spike: theme colours, translations,
  `?` help, the date scrubber, badges for video, Live Photo and favourites.
- The animated zoom step is one glide at a time; a step during a glide
  starts from the old layout, not from where the cells are.
