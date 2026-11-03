# M2 Pro validation, 3 November 2026

This records local checks on Apple M2 Pro hardware running Asahi/Arch Linux,
Qt 6.12.0, GCC 16.1.1 and 16-KiB kernel pages. Private photos, filenames,
source keys and raw artifacts stay under ignored `.tools/photo-search`.
No photo was uploaded to an analysis service or fetched from iCloud for
these checks. Model inference and image decoding ran locally.

## Checks completed

- The complete Qt suite passed **168 tests, 0 failed, 0 skipped** on Xvfb
  with explicit `QT_QPA_PLATFORM=xcb`, real local HDR/decode samples and
  OpenGL. This includes the four render-regression rows that offscreen mode
  deliberately skips. The earlier obsolete Polish translations were removed.
- The Python script suite passed **102 tests** with the optional model
  environment, real Chromium and real offline ANE compilation tests enabled. These tests
  do not submit accelerator jobs.
- Shared controls/accessibility passed **16/16** on Xvfb (the earlier 15/15
  also passed offscreen),
  including menu pointer opening, keyboard focus, large type, narrow bounds,
  dismissal and scrolling. This is not an Orca session; Orca is not installed.
- The isolated full-window accessibility self-test passed with a local
  protocol stand-in, private settings/Pictures and 384 copied thumbnails.
  It exercises settings/workshop/source dialogs, Tab containment, compact
  layout, Ctrl+A, export menus and two clicks of More. It caught and verified
  the fix for a long date overlapping More; no screen reader was started.
- The personal semantic search plugin passed its complete 384-photo real-host
  interaction test offscreen and on native Wayland on hidden workspace 5.
  See [the plugin notes](photo-search-plugin.md) for tested interactions and
  the explicit limitation to the indexed sample.
- Final startup checks passed with the isolated host and explicit CPU
  affinity reporting. With the service unavailable, inherited affinity gave
  a 347-ms median; CPUs 4–11 gave 285 ms in a separate ten-run set. Production
  scheduling was not changed. See [the full startup notes](../startup-performance.md).

An initial full Qt invocation inside the restricted command sandbox failed
its local socket/profile setup, then crashed in `Plugins::setSources` after
a fixture constructor returned early. `coredumpctl` identifies that stack;
no OOM event was found. Repeating with the required local-socket/profile
access passed. This was a failed test setup, not evidence of a photo-decoder
crash or data loss. No system settings or crash-notification rules changed.

## Real image samples

There were 494 cached HEIC files: header inspection grouped 335 with both
Apple gain-map and ISO tmap markers, one Apple-only candidate and 158 others.
A deterministic capped sample selected **17 HEICs**, including eight with
both map forms. Header candidates are not assertions about decoded metadata;
the real parser/decoder test supplies that evidence.

The existing non-thumbnail cache also contained 1,246 JPEG header candidates
with ISO gain-map metadata. Eight were selected. These are convenience
samples of already cached files, possibly including derivative copies, not
an independent representative HDR dataset.

`realGainMapsAgreeInEveryKind` ran over those **17 HEICs and 8 JPEGs**.
It detected nine HEIC maps and eight JPEG maps; **17/17** decoded with the
reported map kind and matching upright aspect ratio. Eight HEICs exposed
both Apple and ISO metadata. The maximum headroom difference printed as
**0.000 EV** (rounded to three decimals), below the test's 0.1-EV limit.
This closes the formerly unrun local-sample check for these supplied cases;
it does not establish coverage of every HDR variant.

`whatDecodingAndColourCostOnRealPhotos` also passed over **32 cached JPEG
images (30 tagged)** and **8 HEIC originals**. In that run the JPEG path
averaged 1.333 ms decoding, 0.538 ms colour conversion and 1.782 ms for the
loader path. HEIC decode/scale/sRGB averaged 415.2 ms at a 3456×2234 bounding
size and 424.8 ms at full size. These are per-sample averages from a single
local run, not p95, display latency or a comparison against another machine.
All sample files were read-only; test folders contain links or private copies.

## Remaining environment limits

ANE firmware timed out during the corrected SigLIP run and remains
quarantined for this boot. Existing CLIP embeddings can be searched, but
new device work requires a reboot plus a fresh smoke receipt. See
[the full encoder results](photo-search-full-encoder.md).

The required `bin/install --non-interactive` was attempted after the feature
commits. It stops before application activation because the pinned upstream
Blaze executable reports `jemalloc: Unsupported system page size`. No Cargo
application build was substituted, and the previous installation remains.
The already installed host predates the plugin API needed by the new personal
search prototype; the newly built Qt host was used for its validation.

Xvfb was downloaded and extracted inside `.tools`, without a system package
installation. Package `xorg-server-xvfb 21.1.25-2`, SHA-256
`9ff2d843b83dd61bf82746f3c675ca64f0d7ffa8c6e9b23281c005c83aedba7d`,
matched the trusted local pacman sync database. The temporary displays used
private Xauthority files and disabled TCP listening.
