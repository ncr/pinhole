# Colour and HDR

This is a design to carry out one phase at a time. It is meant to land as
`docs/hdr.md` and to be kept true as the phases land.

Jacek asked for full HDR support. This document says what that can mean on
his MacBook today and later, and how to get there in phases that each land
alone. Each phase is tested on this machine and costs the grid nothing.

The code was read from `origin/main` at 6f1eae1. The machine was read only:
`hyprctl` (read commands), `modetest`, `v4l2-ctl`, and the files in
Pinhole's cache. Nothing was configured. Facts from the web carry their
source; the list is at the end.

## What there is today

### The machine

- **The panel.** Apple sells it as "Wide color (P3)". It sustains 1000 nits
  full screen and peaks at 1600 for HDR, against 500 for SDR [apple-spec].
- **The display driver** is `apple` (DCP). Its eDP-1 connector has EDID
  (an empty blob), DPMS, link-status, non-desktop, TILE and vrr_capable.
  It has no `HDR_OUTPUT_METADATA`, no `Colorspace` and no `max bpc`. The
  CRTC has `CTM` and `VRR_ENABLED`, with no gamma or degamma LUT. The
  planes take `AB4H` (fp16), `XR30`/`AR30` (10-bit), 8-bit RGB, NV12 and
  P010. HDMI-A-1 and DP-1…6 have the same properties: nothing for HDR.
- **What DCP is told.** Every RGB plane, fp16 included, goes to the
  firmware as `DCP_XFER_FUNC_SDR`. The driver's own comment names a DCP
  format "used for HDR" (`b3a8`) that it does not implement
  [asahi-plane]. The 6.19 report (February 2026) speaks of "very early
  HDR experiments" and lists HDR as missing. The 7.0, 7.1 and 7.2 reports
  say nothing more [asahi-6.19, asahi-7.0, asahi-7.2].
- **The panel's gamut is not settled on this kernel.** The 6.19 report says
  the built-in displays stopped looking oversaturated from 6.18 on, because
  DCP now converts sRGB to the panel [asahi-6.19]. A later commit,
  "Only assume RGB planes on internal displays are sRGB", makes RGB planes
  on eDP `DCP_COLORSPACE_NATIVE`, which reads as the opposite
  [asahi-e49109f]. `linux-aurora` 7.2.11 is not Asahi's own build. Whether
  sRGB content on this panel is shown as sRGB or stretched to P3 is checked
  by eye in H0. Pinhole cannot fix it either way. It must be right for
  what the compositor says the output is.
- **External displays.** HDMI works downstream. DP alt mode exists only on
  the `fairydust` branch, for developers [asahi-6.19, asahi-m2]. No source
  shows HDR on an external display through DCP, and the connectors here
  have no HDR properties. **An external HDR monitor is no way round this
  today.**
- **Hyprland 0.57.0** on eDP-1: `colorManagementPreset: srgb`,
  `currentFormat: XRGB8888`, `sdrMaxLuminance 80`, `hardwareDetails: hdr
  false, bt2020 false`. `render:cm_enabled` is true and `cm_auto_hdr` is 0.
  It implements `wp_color_management_v1`, with parametric descriptions,
  `windows_scrgb`, `extended_target_volume`, `ext_linear`, `st2084_pq`,
  `hlg`, and the sRGB, BT.2020 and Display P3 primaries. ICC is refused
  outside debug mode, and only the perceptual intent is accepted
  [hypr-cm]. Its shader converts primaries for any tagged surface. It tone
  maps when the surface's peak is above the output's (`maxLuminance <
  dstMaxLuminance * 1.01` and an ICtCp tone map, read from the binary's
  strings). So a P3 or HDR surface is shown correctly reduced on this SDR
  output. The `hdr` preset needs EDID HDR metadata or `supports_hdr = 1`,
  and a connector that can carry it, which DCP's cannot [hypr-monitors,
  aquamarine-drm].
- **Video decode.** `/dev/video0` is `avd`, a stateless decoder taking
  HEVC (Main 10 in its profile menu), H.264 and VP9. FFmpeg has no V4L2
  request hwaccel; the patch is a draft [ffmpeg-v4l2req]. The VA-API shim
  `libva-v4l2_request-avd` is installed, but is tested only on M1
  [omacom-va]. Qt Multimedia uses VA-API when it loads. Whether HEVC
  Main 10 decodes through it on this M2 Pro is unverified.

### Qt 6.12

- **Window colour space.** Since 6.10.2 the Wayland plugin reads the
  `QSurfaceFormat` colour space a window was created with, and tags its
  surface through `wp_color_management_v1` with named primaries and a
  named transfer function (sRGB becomes `gamma22`). It sets no
  luminances, and it never reads the compositor's preferred description
  [qt-cm, qt-qwaylandwindow]. Qt Quick never sets a colour space.
- **HDR swapchains.** These work on Vulkan only (GLES says `f == SDR`).
  `HDRExtendedSrgbLinear` (scRGB, fp16), `HDR10` and
  `HDRExtendedDisplayP3Linear` exist when the Vulkan surface offers the
  matching colour space [qt-rhivulkan]. Qt Quick picks one from
  `QSG_RHI_HDR` or the window property `_qt_sg_hdr_format` (`scrgb`,
  `hdr10`, `p3`), once, when the swapchain is made [qt-sgrhisupport].
  Mesa's Vulkan WSI has implemented `wp_color_management_v1` since 25.1,
  for every Mesa driver, honeykrisp included [mesa-wsi]. scRGB is offered
  only if the compositor advertises `ext_linear`, `extended_target_volume`
  and fp16 buffers. Mesa then tags the surface itself.
- **`QRhiSwapChain::hdrInfo()` on Linux is a constant**: 1000 nits peak,
  200 SDR white. Real values come only on Windows and Apple
  [qt-rhivulkan, qt-hdrinfo]. The display's headroom has to be read from
  Wayland by Pinhole.
- **Qt Quick does no colour management.** Its materials write sRGB-encoded
  values as they are. In an scRGB swapchain every Rectangle, Text and Image
  comes out on the wrong curve [qt-texturefrag]. A `ShaderEffect` or
  `QSGMaterial` can write values above 1. `QSGPlainTexture` uploads
  `Format_RGBA16FPx4` as `RGBA16F`. The atlas makes everything 8-bit
  [qt-plaintexture].
- **Images.** The JPEG and PNG handlers read ICC into `QImage::colorSpace()`.
  `QImage::convertToColorSpace` runs on Qt's thread pool and keeps values
  above 1 for float formats. QML `Image` has an undocumented `colorSpace`
  property that converts on the CPU at load time [qt-pixmapcache].
- **Video.** The FFmpeg backend has PQ and HLG shaders (BT.2390 style),
  and outputs linear on an scRGB swapchain [qt-videotexturehelper]. H2
  found them used only for P010, which is what hardware decoding gives;
  a frame decoded in software (10-bit planar) is shown as its codes, and
  the HLG shader breaks colours beyond sRGB. So Pinhole makes HDR frames
  SDR itself (H2). `QFFmpegVideoBuffer::maxNits()` does take a mastering
  peak as `10000 * max_luminance`, a bug, but nothing in 6.12 calls it
  [qt-ffmpegvideobuffer].
- `qt6-shadertools` (the `qsb` tool) was not installed, and Pinhole had no
  shaders of its own (since H2's `VideoView`, `hdrvideo.frag`, it is a
  make dependency).

### Pinhole

- **Decoding.** `photoprovider.cpp`'s `decodePhoto` reads HEIC through
  libheif into 8-bit RGBA, and everything else through `QImageReader`.
  `thumbnailloader.cpp` decodes the grid's thumbnails and mediums with
  turbojpeg into `RGBX8888`, and builds mosaics from them. The grid draws
  them with `QSGTextureMaterial` from the atlas. Until H1 the colour
  profile was never looked at, and library items' `thumbnail` was a
  `file://` URL, which the viewer and nine plugin files used in plain
  `Image`s. Since H1 every one of these is sRGB (below).
- **The window.** It uses Vulkan when there is a GPU (`main.cpp`), with an
  opaque, untagged sRGB surface.
- **pinholed.** `render.rs` makes folder thumbnails and mediums with
  `magick -thumbnail`, which keeps the ICC profile (checked: a P3 medium's
  thumbnail stays "Display P3"). `jpeg.rs`, the one JPEG, keeps the
  profile too. `cover.rs` took a video's frame with ffmpeg and no tone
  mapping, so an HLG folder video's cover came out flat (since H2 it is
  tone mapped as the window plays the video: BT.2390, below).
- **The video player** is `MediaPlayer` and `VideoOutput` in
  `PhotoViewer.qml`. The Live plugin plays motion in its own viewer layer.
- **Info** labels HDR10, HLG and Dolby Vision video.

### What is in Jacek's library (read from the cache)

- **Thumbnails** (40 sampled): 17 untagged, 16 Display P3, 6 "Camera RGB
  Profile", 1 sRGB. **Mediums** (30): 22 Display P3, 8 untagged.
- **Apple gain maps travel further than expected:**
  - 200 of 300 cached HEIC originals have Apple's auxiliary
    `urn:com:apple:photo:2020:aux:hdrgainmap`, and the same files also
    hold a `tmap` box (ISO 21496-1).
  - 72 of 200 cached **iCloud mediums** (JPEG, 2048 px) carry Apple's gain
    map as an MPF second image. In one sample it is 768x1024 grey, with
    XMP `HDRGainMap:HDRGainMapVersion 131072` and
    `HDRGainMapHeadroom 6.902454`, and the primary keeps Apple's MakerNote.
  - 33 of 300 **iCloud thumbnails** carry one too.
  - 85 of 112 HEIC `full`s (edits) have one.
- **So the original is not needed for HDR.** The medium already has the
  gain map, and the original's only sharpens it.

## What "full HDR support" can mean here

**Today, on this panel, no picture can be brighter than SDR white.**
Nothing between Pinhole and the panel can ask DCP for it. So, now:

1. **Every photo in the right colours.** iPhone photos are Display P3. Read
   as sRGB, as they are today, they come out duller than on the phone. They
   are converted to the window's sRGB, with colours outside it clipped.
2. **HDR content shown right in SDR:**
   - a gain-map photo shows its SDR base, which is Apple's own SDR
     rendition;
   - a photo whose primary image is PQ or HLG is tone mapped;
   - an HLG or Dolby Vision video is tone mapped, its cover too;
   - Info says what is HDR, and by how much.
3. **HDR kept when photos leave.** A HEIC sent as JPEG keeps its gain map.
4. **HDR output built, tested, and switched on by the display.** When the
   compositor says the output has headroom, the viewer shows gain-map
   photos and HDR video above SDR white, scaled to that headroom. On this
   panel that switches itself on the day Asahi and Hyprland expose HDR,
   with no change to Pinhole. Until then it is tested by forcing it on,
   where Hyprland maps it back to SDR, and by reading back what Pinhole
   hands the compositor.

Wide gamut beyond sRGB on this panel is possible if Hyprland is told the
output is P3 (`cm = "dp3"`). That is right only if DCP shows its input
unconverted (H0's check). Pinhole follows whatever the compositor says. It
never changes the monitor configuration.

## Capability matrix

"Panel, HDR" means the day Asahi exposes HDR on eDP-1 and Hyprland enables
its `hdr` preset there. "External HDR" means an HDR monitor whose
connector Hyprland can drive in HDR (not possible through DCP today).

| Piece | This panel today (SDR) | Panel, HDR | External HDR |
|---|---|---|---|
| P3 photos, grid | Converted to sRGB at decode (H1) | Same: the grid stays sRGB | Same |
| P3 photos, viewer | sRGB, clipped (H1); full P3 if the output is set to `dp3` (H3) | Full P3 in the stage (H3) | Full gamut of the output (H3) |
| Gain-map photos, viewer | The SDR base, correct (H1); Info shows the headroom (H2) | Above SDR white, up to the panel's headroom (H4) | Up to the monitor's headroom (H4) |
| Gain-map photos, grid | The base | The base (no HDR in the grid) | The base |
| PQ/HLG still photos | Tone mapped at decode (H2) | Shown in range (H4) | Shown in range (H4) |
| HLG, Dolby Vision 8.4 and HDR10 video | Tone mapped on the GPU by Pinhole's `VideoView` (H2: Qt Multimedia's is wrong) | In range, in the stage (H3) | In range (H3) |
| Video covers (folders) | Tone mapped by ffmpeg (H2) | Same (covers are SDR) | Same |
| Live motion | SDR, tone mapped (`VideoView`, H2); the stage drops to SDR while it plays (H3) | Same | Same |
| Exports (Send, "HEIC as JPEG") | JPEG with P3 profile and gain map (H5) | Same | Same |
| Background plugin | Photo converted to sRGB (H2) | Same (the wallpaper is SDR) | Same |
| Theme plugin | Palette from sRGB pixels (H2) | Same | Same |
| Info plugin | HDR kind and headroom of photos too (H2) | Same, plus "shown at N stops" | Same |

## Decisions

These are the open questions, decided.

- **Colour belongs in the core.** It is how every photo is drawn. So do
  HDR decoding and HDR output. Plugins get small read-only facts and one
  switch (below). Labels, the Live hand-off, and Background and Theme's
  conversions are the plugins' own.
- **The window stays sRGB, 8-bit and opaque.** The grid's textures are
  sRGB, converted when decoded on the loader threads, never per frame.
  Theme colours, text and plugin QML are sRGB, and Qt Quick cannot draw
  them right in a linear or P3 surface. Tagging the whole window P3 or
  scRGB would mean a full-window extra pass every frame, and would break
  Qt Multimedia's own HDR path (it writes linear when the swapchain is
  scRGB).
- **Wide gamut and HDR live in a stage.** The stage is a second Qt Quick
  window, a Wayland subsurface **under** the main window, with an scRGB
  swapchain. It draws only the backdrop, the settled photo and HDR video.
  The main window above it turns transparent where the stage shows, and
  keeps drawing the chrome and the plugins' viewer layers. The stage exists
  only when the display has headroom or a gamut wider than sRGB, or when
  forced for tests.
- **The stage shows only once the viewer has settled.** While the viewer
  opens, slides to another photo or is dragged away, the main window draws
  the SDR photo as today. Once it settles, the stage takes over at SDR
  (identical pixels) and fades its HDR in over 400 ms, as Photos on the
  iPhone does. Zoom and pan move both windows from the same `viewer`
  properties.
- **No HDR in the grid,** ever. Thumbnails stay 8-bit sRGB in the atlas,
  and grid performance is untouched by everything after H1.
- **Rendering intent** is relative colorimetric, with clipping to sRGB.
  Untagged images are sRGB. Any embedded ICC is honoured, "Camera RGB
  Profile" too.
- **The conversion engine is lcms2 and a matrix of Pinhole's own,** one
  conversion kept per profile per thread. The design said
  `QColorSpace`/`QColorTransform`, with lcms2 behind the same function if a
  480 px thumbnail cost more than 1 ms. H1 measured 300 of Jacek's
  thumbnails: `QColorTransform` 0.9–1.9 ms a tagged one, lcms2's own 8-bit
  transform 2.1–2.6 ms, against 0.62 ms to decode it. Every profile in his
  library is three curves and a matrix (Display P3, "Camera RGB Profile",
  sRGB, "Generic RGB", "Composite NTSC"). So lcms2 reads the profiles, and
  converts a profile of tables (A2B0) if one comes; a profile of curves and
  a matrix goes through a table per channel to linear light, the matrix,
  and a table of 16,384 steps back to sRGB, four pixels at a time on NEON:
  0.31 ms a tagged 480 px thumbnail, within one code of lcms2
  (`app/src/colour.cpp`). lcms2 becomes a dependency of the package.
- **The average colour is sRGB too.** The design kept it as pinholed
  computed it, a placeholder. But Colours finds photos by it, and the grid
  shows it until the converted thumbnail replaces it. pinholed puts each
  thumbnail's average through the thumbnail's profile
  (`core/src/colour.rs`), and a library from before has its averages made
  sRGB once, at the first round (26,475 of Jacek's 49,079 changed, about
  three seconds).
- **Thumbnails are `image://file/<path>`,** not `image://photo/`. The photo
  provider is asynchronous only, on its own two threads, and scales to fit
  `sourceSize`. The viewer's thumbnail is drawn in the opening's first
  frame (`asynchronous: false`), and plugins' `Image`s ask for crops and
  for one side only. `FileProvider` loads a file exactly as an `Image` loads
  a `file://` URL (Qt Quick's own `loadSize`, its thread or at once), and
  makes it sRGB. Plugins show the mediums they fetch the same way.
- **Headroom comes from Wayland, never from `hdrInfo()`.** It comes from
  `wp_color_management_surface_feedback_v1` on the main window's surface:
  `H = target_max_luminance / reference_luminance` of the preferred
  description, falling back to `max_luminance`. It is followed live when
  the compositor sends `preferred_changed`. `PINHOLE_HEADROOM=<stops>`
  overrides it for tests.
- **One swapchain format: scRGB.** If the Vulkan surface does not offer
  `HDRExtendedSrgbLinear`, there is no stage, logged once. There is no
  HDR10 fallback: Qt Multimedia's linear output and the photo shader
  would both need a second PQ path for no display we know of. (On this
  MacBook today the surface offers no fp16 at all, though it offers
  HDR10's colour space at 10 bits: H3.)
- **The stage needs a transparent main window,** which is chosen when the
  window is made. The display is read before the window is made. If the
  output gains headroom while Pinhole is open (another monitor, a
  brightness change into HDR), the stage starts the next time the window
  is made. Headroom changes within an HDR output apply at once.
- **A user switch, "Full HDR" ("Pełny HDR").** It is in the side panel,
  on by default, and shown only when the display has headroom. Off keeps
  the stage at SDR white (its gamut stays). Apple has the same choice.
  There is no other setting.
- **Gain maps.** Prefer ISO 21496-1 when it parses (it is per channel and
  more exact), and use Apple's legacy map otherwise. Every HEIC sampled here with
  one has both, so H4a (Apple only) already covers Jacek's library. The weight
  for a display headroom below the photo's is taken in the log domain for
  both kinds (below), so Apple's maps scale the way ISO's do.
- **Dolby Vision 8.4 plays as its HLG base layer.** The RPU is ignored.
  libplacebo turns an 8.4 RPU into BT.2020 PQ, which is a different
  rendering, not a necessary one [ffmpeg-filters].
- **libultrahdr** (`extra/libultrahdr` 2.0.2, MIT/Apache) becomes a
  dependency in H4b, to read Ultra HDR JPEGs, and in H5, to write them.
  libheif 1.23 has no gain-map API, so the HEIC `tmap` is parsed by
  Pinhole. Its gain-map PR is still open [libheif-1503].
- **`qt6-shadertools`** is a make dependency since H2's `VideoView`, for
  `qsb`; H3's stage shader (`photo.frag`) uses it too.
- **Private Qt API, once.** Placing the stage below the main window needs
  `QtWaylandClient::QWaylandWindow::subSurfaceWindow()->place_below()`.
  Qt's `lower()` ignores subsurfaces. It is behind one function
  (`placeSubsurfaceBelowParent`, `stage.cpp`), with a version check: on a
  Qt it was not built against, there is no stage.
- **JPEG XL stays out.** Pinhole does not read JPEG XL at all yet.

## The maths

Every shader works in linear light, where 1.0 is SDR white (scRGB).

- **sRGB and Display P3 decoding** (the same curve), extended to negative
  values and values above 1:
  `lin(c) = sign(c) * (|c| <= 0.04045 ? |c|/12.92 : ((|c|+0.055)/1.055)^2.4)`.
- **Display P3 to linear sRGB** (D65; scRGB keeps the negatives that make
  P3's extra colours):

      [ 1.2249401 -0.2249404  0.0000000 ]
      [-0.0420569  1.0420571  0.0000000 ]
      [-0.0196376 -0.0786361  1.0982735 ]

- **ISO 21496-1** (the same as Android's Ultra HDR and Adobe's spec)
  [libavif-h, android-uhdr]. All parameters are per channel and log2:
  - `G = lerp(gainMapMin, gainMapMax, pow(encoded, 1/gamma))`
  - `f = clamp((H - baseHdrHeadroom) / (alternateHdrHeadroom - baseHdrHeadroom), 0, 1)`
  - `w = sign(alternate - base) * f`, where `H` is the display's log2
    headroom
  - `hdr = (base + baseOffset) * exp2(G * w) - alternateOffset`
  - The gain applies in the base's primaries, linear, when
    `useBaseColorSpace` is set. Otherwise it applies in the alternate's.
- **Apple's legacy map** [apple-gainmap]. It is one channel at a quarter or
  half of the size, encoded with the Rec.709 curve:
  - Linearise: `g = e <= 0.081 ? e/4.5 : ((e+0.099)/1.099)^(1/0.45)`.
  - Headroom `Hi`, linear:
    - Version 2 maps carry `HDRGainMapHeadroom` in their XMP.
    - Otherwise it comes from MakerNote tags 33 and 48:
      - if maker33 < 1: maker48 <= 0.01 gives `-20*m48 + 1.8`, else
        `-0.101*m48 + 1.601`;
      - otherwise: maker48 <= 0.01 gives `-70*m48 + 3.0`, else
        `-0.303*m48 + 2.303`;
      - then `Hi = 2^max(stops, 0)`.
  - Full: `hdr = sdr * (1 + (Hi - 1) * g)`.
  - At a display headroom `Hd` below `Hi`, Pinhole uses
    `hdr = sdr * exp2(w * log2(1 + (Hi - 1) * g))` with
    `w = clamp(log2 Hd / log2 Hi, 0, 1)`. This is ISO's weighting applied
    to Apple's gain, and equals Apple's formula at full headroom. Apple
    does not document the partial case.
  - That `HDRGainMapHeadroom` is linear rather than in stops is to be
    confirmed in H4a against the MakerNote of the same file.
- **The fade** multiplies `w` by a value going from 0 to 1 over 400 ms, so
  the stage starts identical to the SDR photo.

## Phases

The phases go in order. H2 and H3 touch different files and can run side
by side. Each phase:

1. lands on `main` alone and is installed with `bin/install --dev`;
2. keeps `docs/` true (this file, `plugins.md`, `service.md`);
3. adds every new string to `pl.json`;
4. passes `app/bin/test`, `cargo test --release --workspace` when it
   touches `core/`, and `pinhole plugin check` on each plugin it touches;
5. is measured against H0's numbers. A regression over 5 % does not land.

Every run that opens a window goes through a script handed to Hyprland's
workspace 5, as CLAUDE.md says.

### H0: the yardsticks and the panel (no code)

- Record on today's `main`:
  - `app/build/pinhole --bench --library ~/.local/share/pinhole/fake`;
  - `PINHOLE_SELFTEST=viewer`'s stage times;
  - the thumbnail loader's decode rate, from the bench's fill phase.
- **The panel check, by Jacek, two minutes.** Show the same P3 photo of
  saturated reds and greens, or Apple's P3 test image, on the iPhone and in
  Pinhole after H1:
  - If Pinhole's is clearly more saturated than the phone's, DCP is
    stretching sRGB to the panel.
  - In that case `cm = "dp3"` in his own `monitors.lua` would make the
    whole desktop right. That is his call: Pinhole never touches it.
  - Write the answer here.
- **Recorded** on `main` (46f1b26, then f23e8db) with two yardsticks H0
  added: the bench's last line, what the loader spent on each thumbnail,
  medium and mosaic page; and `PINHOLE_SELFTEST=colour:<dir>`, the frames
  to compare colours by (H1). Six runs, interleaved with H1's:
  - `--bench`, offscreen, 1400x900: on the fake library (untagged), mean
    of the phases' sync50 0.134 ms and sync99 0.411 ms; on a copy of
    Jacek's library, 0.168 and 0.481 ms.
  - Filling: after each random jump, every cell in view sharp after 32 ms
    (median), 109 decodes a second.
  - The loader: a thumbnail of the copy 2.25 ms on its eight threads, a
    mosaic page 140–400 ms; alone, 0.62 ms to decode a 480 px thumbnail.
  - `PINHOLE_SELFTEST=viewer` (the copy, its renditions cached), medians
    of two rounds: HEIC medium 36–51 ms, HEIC original 241–266 ms, JPEG
    original 69–78 ms, Live Photo original 476–493 ms.
- **The panel check is not made yet.** The photo for it, found in the
  library by how much of it is red and green beyond sRGB:
  `IMG_4520.HEIC`, a night photo under red and green
  lights.
  A daylight one beside it:
  `IMG_3841.HEIC`, a saturated red on green.
  - Both screens at full brightness, Night Light and True Tone off.
  - Open it full window in Pinhole and in Photos on the iPhone, side by
    side.
  - The answer: not yet.

### H1: every photo in the right colours

Landed. What was built, and where it departs from the plan above it:

**Files.**
- New `app/src/colour.cpp`/`.h`:
  - `void toSrgb(QImage &, const QByteArray &icc)`, and `toSrgb(QImage &)`
    for what Qt's readers give (their `colorSpace()`'s ICC). It does
    nothing with no profile, with sRGB's (to within a code), and with one
    lcms2 cannot read or not of RGB. A `thread_local QHash<QByteArray, …>`
    keyed by the ICC bytes keeps each thread's conversions; Display P3 is
    nearly every hit.
  - The engine is lcms2 and a matrix of Pinhole's own (Decisions): lcms2
    reads the profile; three curves and a matrix are converted by tables
    and the matrix, on NEON four pixels at a time; anything else by
    lcms2's transform. An image over a megapixel is converted in bands of
    64 rows on Qt's thread pool.
  - `QByteArray iccOfNclx(const heif_color_profile_nclx *)`, an nclx as an
    ICC profile lcms2 makes from its primaries and curve, rather than a
    `QColorSpace`: the one path for both.
  - **An iPhone's edit says BT.709 and means sRGB's curve.** Of the
    cached HEICs, 363 of 373 `full`s and 107 of 112 originals carry an ICC
    profile; the other five of each say nclx 12/1/6 (P3, BT.709,
    BT.601). Decoded as they are, their codes are their Display P3
    medium's to within half a code on average (five checked). So
    transfer 1, 6, 14 and 15 are taken as sRGB's curve, as 13 and 2 are;
    8, 4 and 5 are linear, 2.2 and 2.8; PQ and HLG are left as they are
    for H2.
- `photoprovider.cpp`: `decodeHeif` reads the ICC (`prof`, `rICC`) first,
  else the nclx, and converts once scaled; it now takes libheif's plane
  writable, so the photo at its own size is converted where it is, with no
  copy. A `QImageReader` image is converted by its own `colorSpace()`; a
  CMYK JPEG by Qt.
- `thumbnailloader.cpp`: `TJPARAM_SAVEMARKERS` 4 (the ICC alone) and
  `tj3GetICCProfile`; `decodeJpeg` converts each thumbnail and medium, and
  each mosaic tile once it is 64 px.
- **`FileProvider`, `image://file/<path>`** (Decisions), in
  `photoprovider.cpp`, for an item's `thumbnail` (`plugins.cpp`, the item
  of `library.item`), the viewer's `thumbnail()` and `thumbnailAt()`, and
  the mediums Frame, Series and Contact sheet fetch. The provider pool
  needed no third thread: the strip's thumbnails load on Qt Quick's thread,
  as they did.
- **Plugin API 4 begins here**: `image://file` and a photo's `thumbPath`
  (the thumbnail's file, for a program; Contact sheet's fallback). Frame,
  Series and Contact sheet ask for 4. H2 adds the rest of API 4.
- `core/src/colour.rs`: a thumbnail's average colour through its profile,
  in pinholed (Decisions), and `sync::srgb_averages`, once per library, at
  the start of a round whether or not the source is ready.
- `PINHOLE_SELFTEST=colour:<dir>` (`selftestcolour.cpp`): photos in the
  grid at 3, 13 and 29 columns, full window as they open and once sharp,
  and Colours' finder and wall, for comparing frames. A window on a
  workspace not shown draws no frames of its own, and offscreen draws no
  grid, so it steps the grid itself and draws each frame with a grab.

**Performance**, measured on Jacek's library copy (H0 has the numbers
before):
- A 480 px thumbnail: 0.62 ms to decode, 0.31 ms more to make sRGB when it
  is tagged (183 of 300 sampled; 0.19 ms over all of them). In the bench,
  on the loader's eight busy threads, a thumbnail of the library copy took
  2.25 ms before and 2.59 ms after (medians of six runs).
- A HEIC original (24 of the library's, three times each, alternating with
  the decode as it was, twice): +17 to +18 ms whole (355 → 372 ms, +5 %),
  +4 to +11 ms at the viewer's 3456x2234 (about 375 ms, +1 to +3 %). 12 MP
  alone converts in about 4 ms on the pool.
- The grid, `--bench` six times a side, on the fake library (untagged) and
  on the library copy (tagged): the frames are the same. Mean of the
  phases' sync50, fake 0.134 → 0.131 ms, copy 0.168 → 0.150; sync99 copy
  0.481 → 0.480. The fake's sync99 came out 0.411 → 0.447, and the
  29-column phases that make it, measured again six times a side, 1.017 →
  1.030 (+1.3 %): run-to-run noise, as the draw path is untouched. Every
  p99 frame interval within a millisecond of before.
- Filling: after each of the bench's random jumps every cell in view was
  sharp after 32 ms (median), before and after, in all 24 runs; 109
  decodes a second either way.
- `PINHOLE_SELFTEST=viewer` passes (ten runs a side). Its stage times
  scatter more than H1's cost: the HEIC original came 241–301 ms after it
  was asked for before H1, and 226–337 ms after, as it decodes beside the
  self-test's own whole decode of it and the neighbours' mediums. Four
  more runs with the conversion kept on the decoding thread (no bands on
  the pool) were no quicker (243–336 ms).

**Tests** (`app/tests/pinhole_tests.cpp`, `core/src/colour.rs`,
`core/src/sync.rs`):
- A JPEG of P3 green, a deep red and a grey, tagged with Apple's own
  Display P3 profile (from a real thumbnail, `app/tests/data`): through
  `decodePhoto`, the loader (`decodeThumbnail`) and `image://file` the
  green is sRGB (0, 255, 0) clipped, the red within 2 of lcms2's, the grey
  unchanged.
- An untagged JPEG comes back byte for byte, through Qt's reader and
  turbojpeg; one tagged sRGB is not touched.
- A gradient of every hue in Display P3, the "Camera RGB Profile" (the
  profile, not the photo, in the test data) and a profile of tables:
  within one code of lcms2's own transform, whole and in bands alike.
- HEICs made with `heif-enc`: nclx 12/1 and an ICC convert like the JPEG,
  nclx 1/13 is left alone, and scaled first the colours hold.
- `image://file` sizes as `file://` does.
- In Rust: the profile parser against lcms2 on both real profiles, a
  profile split over APP2 markers, and the averages made sRGB once.

**Proof on real photos** (frames from `PINHOLE_SELFTEST=colour`, before and
after, on the library copy; the reference is lcms2 converting the before
frame's codes, which are the photo's own):
- `IMG_4520.HEIC` (a night photo under red and green
  lights), its thumbnail in the grid at 3 columns: the after frame is
  lcms2's to 0.44 of a code on average (median 0); its reddest patch was
  (254.6, 6.0, 81.7) as P3 codes drawn as sRGB, is (255.0, 0.0, 76.0), and
  lcms2 says (255.0, 0.0, 75.9). Full window: 0.30 on average, 99 % within
  2.
- `IMG_3841.HEIC` (red on green, in daylight): a green patch (43.3, 237.8,
  173.4) is (0.0, 241.8, 168.3), lcms2 (0.0, 241.8, 168.4).
- `IMG_8903.HEIC` and `IMG_7725.HEIC` full window at their sharpest (the
  HEIC full size and original): 0.06 and 0.05 of a code on average, 99.9 %
  within 2.
- The grid at 13 and 29 columns (mosaics), every photo in view: 99.4 % and
  99 % of pixels are as before (untagged, sRGB) or as lcms2 makes P3 of
  them; the rest are other profiles' (Camera RGB) and edges.

**In the app:** iPhone photos look as saturated as on the phone, in the
grid, the viewer and every plugin. Theme and Background still read the
files' codes as they are: they are H2's.

### H2: HDR content shown right in SDR

Landed, after a commit of its own that keeps every self-test off the
person's own Pictures and clipboard (`selftest.h`): the Contact sheet's had
written to `~/Pictures/Pinhole` and replaced the clipboard. What was built,
and where it departs from the plan:

**Stills.** `photoprovider.cpp` decodes a HEIF whose primary's nclx says PQ
or HLG (transfer 16 or 18) as `heif_chroma_interleaved_RRGGBB_LE`, and
`colour.cpp`'s `toneMapToSrgb` makes it SDR at its own size, in bands on the
pool, before it is scaled: each code made linear by a table (PQ's EOTF in
nits; HLG's inverse OETF, then its OOTF at 1000 nits and gamma 1.2 on the
pixel's BT.2020 luminance), BT.2390's EETF to SDR white at 203 nits applied
to the largest channel (so hue stays; anything above the peak is SDR
white), the nclx's primaries to sRGB through XYZ, clipped, encoded. PQ's
peak is the content light level, else the mastering display's, else 1000.
- AVIF goes through libheif too (`.avif` was a photo to the folder source
  but nothing could show it). This x265 makes no 10-bit HEIC, so the test
  stills are 10-bit AVIF; the path is the same.
- Neither Jacek's library nor the cache has a PQ or HLG still: the two
  edits whose boxes say transfer 16 carry it on their `tmap` (the HDR
  alternate), not on the picture.
- A folder's grid thumbnail of one was ImageMagick's, which shows its
  codes as they are. Since H3, `render.rs` reads a HEIF's or AVIF's boxes
  for the primary item's nclx (`pitm`, `ipma`, `colr` in `ipco`), and a PQ
  or HLG one is made by ffmpeg as a video's frame is and tone mapped by
  `cover.rs`'s `tone_map` (`an_hdr_still_is_tone_mapped_as_a_video_frame_is`:
  an HLG grey of 0.75 is SDR white, 255, where ImageMagick made 191).
  Thumbnails made before stay as they were: no such still has been seen.

**Video covers.** `cover.rs` asks ffprobe for the curve with the length
(and a PQ stream's mastering peak). For `arib-std-b67` or `smpte2084`
ffmpeg scales the frame and gives it as 16-bit R'G'B' (a PAM of
`rgb48be`, full range), and `tone_map` makes it SDR in the window's very
maths (`hdrvideo.frag`): HLG's OOTF at 1000 nits or PQ's EOTF, BT.2390's
EETF on the largest channel, BT.2020 to sRGB, clipped. At first it was
ffmpeg's Hable at SDR white 203 nits (`tonemap` has no BT.2390), and an
iPhone clip's cover was a third darker than the video playing from it
(mean 74 of 255 against 106); now the cover is the playback's within 1.2 %
(JPEG and scaling). A folder's video's key has `cover::VERSION` (3), so
every folder video's cover is made again once. iCloud's videos have
Apple's thumbnails; the few covers Pinhole made of iCloud videos (only for
a black thumbnail, 33 in Jacek's library) are made again once since H3
where that is cheap: an HLG or PQ video's whose file is on this computer,
needing no fetch (the library's meta `covers` is `cover::VERSION` after;
`an_hdr_videos_cover_from_before_is_made_again_once`). The rest stay.

**Video in the viewer and Live motion: Qt Multimedia 6.12 is wrong, so
`VideoView` (`videoview.cpp`; at first `SdrVideo`).**
`PINHOLE_SELFTEST=hdrvideo:<dir>` plays
SMPTE bars made HLG and PQ (10-bit H.264, a PQ one with mastering
metadata), and Jacek's own iPhone HLG Dolby Vision 8.4 clips, through a
`MediaPlayer` and `VideoOutput` as the viewer has them, grabs the first
frame and compares it, block by block, with ffmpeg's tone mappings. What
Qt does (read in its 6.12 source too):
- Its HDR shaders are chosen only for P010. HEVC Main 10 is decoded by
  VA-API through `libva-v4l2_request` on this M2 Pro's AVD (P010, the very
  frame software decoding gives: checked with ffmpeg), so the iPhone
  clips get them. Anything decoded in software (H.264 10-bit, AV1, VP9, or
  HEVC without the shim) is 10-bit planar, whose shader has no transfer
  function: HLG and PQ are shown as their codes, flat and grey (2.2 of
  255 off ffmpeg's untouched frame on average, 10.6 at worst).
- With P010, the HLG shader's EETF never compresses (its `maxLum` is SDR's
  1.0 in HLG's terms, so the knee is at 1) and it takes no gamut into
  account: colours beyond sRGB go negative and come out as garbage. One
  of the two iPhone clips (a vivid screen) shows posterised cyan and brown
  over its magenta sky (worst block 187 of 255 off).
- `QFFmpegVideoBuffer::maxNits()` multiplies a mastering peak, already in
  nits, by 10,000 (the suspected bug, real), but no use of it was found
  in 6.12, and the PQ bars with mastering metadata come out exactly as
  those without: a stream's `maxLuminance()` is Qt's default (10,000 for
  PQ, 1500 for HLG). `hdrInfo()` plays no part in SDR.

So at first `SdrVideo` sat between the player and the output, mapping a
frame whose transfer is HLG or PQ and making it SDR on the CPU by the same
maths as a still. Right, but slow: reading back each hardware frame and
the maths per pixel took 612 % CPU for an iPhone's 1080p60 Dolby Vision
clip and 746 % for 4K60 HLG, which showed 29 of its 60 frames a second.

`VideoView` is what shows video now (the viewer, the Live plugin's motion,
the Frame plugin, any plugin's, which `plugin check` keeps from a
`VideoOutput`): a `QQuickItem` that is the player's video output and
draws its frames itself. The decoder's frame goes to the GPU as a
`VideoOutput`'s does (Qt's `QVideoFrameTexturePool`, the hardware frame's
own planes), and an HLG or PQ one (planar, NV12, P010 or P016) is drawn by
Pinhole's shader, `shaders/hdrvideo.frag` (baked by `qsb` at build), in the
still's maths: Y'CbCr by its matrix, its light, BT.2390's EETF on the
largest channel to SDR white times `headroom`, BT.2020 to sRGB, clipped to
the headroom, sRGB's curve. `headroom` is 1 now; H3's stage gives it the
display's, and the values above 1 stay, in extended sRGB. Any other frame
is drawn by Qt's own shaders, as a `VideoOutput` draws it. Only where the
GPU samples no 16-bit textures is an HDR frame made SDR on the CPU first
(`toneMapFrame`); `SdrVideo` stays for plugins of API 4, and hands its
frames to a `VideoView` untouched. Its sink, as a `VideoOutput`'s, also
says a frame came without giving it: a QML handler given each frame keeps
them in JavaScript until the collector runs, and the hardware decoder ran
out of its 64 surfaces within a second.

Through it every clip is within 6 of 255 of the formulas computed in
double precision from ffmpeg's own decode, block by block (iPhone Dolby
Vision mean 0.6, worst 2.7; 4K HLG 0.5 and 2.5), as with `SdrVideo`.
`PINHOLE_SELFTEST=hdrplay` plays each through a `VideoOutput` and a
`VideoView`, a frame drawn every 16 ms: both clips 60 frames a second
drawn, the frame given 28 ms behind the player's clock on average (as
the `VideoOutput`'s), a seek playing on, and the CPU the `VideoOutput`'s
within the noise (63 % against 71 % for the Dolby Vision clip, 70 against
72 for 4K HLG, most of it the test's own grabbing of every frame). All three of Qt's faults are worth reporting to Qt; not reported yet.

**`display.cpp`, `pinhole display`.** `color-management-v1.xml` through
`wayland-scanner`; `readOutputColours` binds `wp_color_manager_v1` (at most
the version generated) and each `wl_output` on a queue of its own, and
reads each output's description: primaries (named or given), curve,
luminances, target. Headroom is `log2(target max / reference)`, else max
over reference; wide is a gamut larger than sRGB's. `ColourDisplay` is
`pinhole.display` (and `display` to the window's QML), read as the window
starts and when screens come and go, for the screen the window is on;
`PINHOLE_HEADROOM` overrides. On this MacBook:

    eDP-1: srgb primaries, compound_power_2_4 curve; 0.2 to 80 nits, SDR white at 80; no HDR headroom; sRGB's gamut

(Hyprland gives sRGB's primaries as numbers, not by name.)

**`gainmap.cpp`.** No pixels: a HEIC's items and a JPEG's markers up to
the first scan, and the MPF second image's. The result is `{kind,
headroom}`, ISO's when it parses, else Apple's, else the curve's.
- A HEIC's Exif and XMP are found by their `cdsc` references: libheif
  gives an iPhone's Exif (which describes the picture and its edit
  together) to no handle.
- The ISO APP2 in a JPEG is named `urn:iso:std:iso:ts:21496:-1`.
- Apple's mediums carry ISO's metadata beside Apple's XMP: 81 of 200
  sampled mediums are HDR (the plan's 72 counted Apple's alone), 109 of 112
  cached HEIC originals have both kinds.
- **Linear, settled:** over 219 HEICs and 162 mediums, `log2` of
  `HDRGainMapHeadroom` equals the MakerNote's stops and ISO's alternate
  headroom within 0.001 EV.
- Reading one: 0.5 ms a medium, 6 ms a HEIC (libheif reads its boxes), on
  a thread, as the viewer's file arrives.

**API 4, the rest:** `pinhole.display`, the viewer's `hdr` (read for the
medium, then the full size), `pinhole.srgbProfile`
(`share/pinhole/sRGB.icc`, Qt's own sRGB profile, committed in
`app/assets`), and `SdrVideo` for a plugin's video (`VideoView` since
API 5). Info's format line
says "HDR, 1.8 EV"; Background's `background keep` and Theme's
`magickArgs` take the profile (`magick -profile`), so a P3 photo's
wallpaper and palette are in sRGB.

**Performance**, against the build before H2, interleaved:
- `--bench` on the fake library, six runs a side: the grid's frames as
  before. Mean of the phases' sync50 0.170 → 0.185 ms, all of it one
  phase that flips between two values run to run in both builds (idle at
  29 columns, 0.40 or 0.73); the other thirteen 0.152 → 0.156. Sync99
  0.502 → 0.468. Every cell sharp 32 ms after a jump (median), either way.
- `PINHOLE_SELFTEST=viewer` on the library copy, six runs a side, each
  build with its own plugins (H2's Live plugin needs H2's `SdrVideo`;
  without it the old build's motion never plays and its loop seeks look
  31 ms quicker): HEIC original 285 → 294 ms (median; means 291 → 292),
  Live Photo original 454 → 480 (means 479 → 482), video first frame 204
  → 197, loop seek 124 → 124. The HDR read runs on a thread of its own at
  low priority, never on the pool that decodes the same photo.

**Tests.** `aPqOrHlgStillIsToneMappedAsTheFormulasSay` (every pixel within
2 of the formulas from libheif's own 16-bit decode, the peak and above
white), `theEetfKeepsWhatIsBelowItsKneeAndPutsThePeakAtSdrWhite`,
`aGainMapIsReadFromAJpegsMetadata` (a medium's structure made in the
test, Apple's and ISO's; Adobe's; cut short anywhere),
`realGainMapsAgreeInEveryKind` (`PINHOLE_HDR_SAMPLE`),
`aDisplaysHeadroomIsItsTargetOverSdrWhite`,
`hdrVideoFramesAreMadeSdrAsTheFormulasSay` (planar and P010, HLG and PQ),
and in Rust `an_hlg_video_gets_a_cover_as_colourful_as_its_sdr_self`
(saturation 0.894 against the SDR clip's 0.929; 0.622 untouched).

### H3: the stage (wide gamut and HDR output)

Landed. What was built, and where it departs from the plan:

**When there is one.** `main.cpp` makes a `Stage` (`stage.h`) before the
window, from what `ColourDisplay` read of the outputs: wanted where the
display has headroom or a gamut wider than sRGB, or with
`PINHOLE_STAGE=force`; never with `PINHOLE_STAGE=off`. Not wanted (this
panel: `pinhole display` says sRGB, 80 of 80 nits), the window is made as
before: opaque, its colour the theme's, no stage, no shaders loaded. Wanted,
`Main.qml` makes the window transparent and draws its own background, and
once the window has drawn, its swapchain is asked whether the surface offers
scRGB (`isFormatSupported(HDRExtendedSrgbLinear)`). Without it there is no
stage, said once; forced, the stage runs on an ordinary swapchain, its
output clipped and in sRGB's curve, which tests its coming and going.

**This MacBook offers no scRGB today.** Hyprland's DRM backend advertises
fp16 (`AB4H`) dmabufs, but honeykrisp's WSI offers a Wayland surface no
`R16G16B16A16_SFLOAT` (8- and 10-bit formats only, in every colour space;
listed by a small Vulkan probe, `MESA_VK_WSI_DEBUG=sw` and `linear` alike).
So even forced, the stage on this panel would be an SDR one; with real HDR
it waits on Mesa. In a nested Hyprland with a headless output (below) Mesa
offers fp16, and everything here was tested there.

**The window.** `Stage.qml`, a `QQuickWindow` made by `Stage::make` and
made a child of the main window (`QWindow::setParent`: a Wayland
subsurface; QML's `Window` has no `parent` in 6.12, and a `Loader`'s window
reparented so crashed on exit). Before it is shown its `_qt_sg_hdr_format`
is `scrgb` and its `QSurfaceFormat` colour space linear sRGB: **Mesa 26.3
makes the fp16 swapchain but does not tag the surface**, which the
compositor would then take as sRGB's curve, so Qt tags it
(`wp_color_management_surface_v1.set_image_description`: `srgb` primaries,
`ext_linear`). Should a later Mesa tag it too, that is a second
`get_surface`, a protocol error: the thing to check when Mesa changes.

**Placed below** by `placeSubsurfaceBelowParent()`, the one use of Qt
Wayland's private classes (`QWaylandWindow::subSurfaceWindow()`,
`place_below`), only when `qVersion()` is the Qt it was built with; else no
stage. **The subsurface stays desynchronised** (Qt makes it so), not
synchronised as planned: synchronised, the stage's fade and its video would
show only when the main window happens to commit.

**The hand-over** (`Stage.qml`), frame by frame. Settled (open, grown,
`offset` and `dismiss` 0, not closing), the stage decodes the viewer's
sharpest image; then it is shown, empty and transparent, and placed below;
after two frames of the main window (which commits the stacking) it draws
its backdrop and photo; after two frames of its own, `handedOver`: the main
window stops drawing its backdrop, photo and video (`PhotoViewer.staged`),
the grid and its own background go, and the fade starts. Going back, the
main window draws the photo again first, and only after two frames of it is
the stage hidden and its texture let go.

**The photo: `StagePhoto`**, a `QQuickItem` with its own material
(`shaders/photo.frag`), not `image://stage/`: an image URL cannot carry the
matrix. It decodes the very file and size of the viewer's sharpest image
(`decodePhoto(…, &profile)`, which leaves the pixels in their profile) on a
thread of its own, keeps them as RGBA8, and draws each code through the
profile's own tables and matrix (`stageColourOf`, the same that `toSrgb`
converts by; a profile of tables is made sRGB first). **Filtered by hand**:
the four texels converted, the clipped values filtered in sRGB's curve as
the main window filters its sRGB texture, the unclipped in the curve
extended. With the texture's own filtering the hand-over was off by up to
15 codes at saturated edges; by hand, by one at most. Its geometry is the
viewer's frame's (`frameItem`), bound to it.

**Weights.** `fade` 0 to 1 in 400 ms from the hand-over; `hold` to 0 while
any plugin's `holdSdr` (400 ms each way); `full`, the side panel's Full HDR.
The gamut shown is `fade × hold`; the headroom used, `fade × hold × full`
of the display's, in stops. On an SDR stage both are 0.

**Video** is not switched between outputs. The stage's `VideoView`
`follows` the viewer's: each frame given to the viewer's is given to it too,
and the one it has at once, so a paused video shows. Its `linear` makes the
HDR shader (`hdrvideo.frag`) write linear light, and Qt's own shaders for
other frames their scRGB variant; its `headroom` is `2^(headroom × weight)`.

**Headroom from the compositor.** `ColourDisplay::followSurface` binds
`wp_color_manager_v1` on a queue of its own and hands it to Qt's, asks
`get_surface_feedback` for the window's surface and
`get_preferred_parametric`, and asks again on each `preferred_changed`; the
description's events are read as an output's (`DescriptionReader`). The
preferred description, once given, is what `display.headroom` and `wide`
are, and a change is said on stderr. `PINHOLE_HEADROOM` still overrides.

**`holdSdr`** is **API 6** (API 5 was taken by the kit): a plugin root's
property; Live Photos sets it while its motion plays. **Full HDR**
("Pełny HDR") is under a "Display" heading in the side panel, shown only
when `display.hdr`, kept in `window.conf` (`display/fullHdr`).

**Tested** in a nested Hyprland of its own (its config, its socket, its
HOME; the host window disabled and a headless output created inside it, so
nothing showed on Jacek's screen), whose output takes `cm = "hdr"`,
`supports_hdr = 1`: `pinhole display` there says "bt2020 primaries,
st2084_pq curve; 0.005 to 1000 nits, SDR white at 203; HDR, 2.30 stops".
`PINHOLE_SELFTEST=stage:<dir>` (`selfteststage.cpp`) reads back the stage's
swapchain image after rendering (`QRhi::readBackTexture`; Qt 6.12's
`grabWindow()` returns an fp16 swapchain as 8-bit codes), RGBA16F:
- the stage's swapchain scRGB, its window a child of the main one, placed
  below;
- the stage's first frame, where the main window has turned transparent,
  against the SDR photo the main window drew there: 311,685 pixels, mean
  0.22 of a code, worst 1;
- the backdrop the theme's in linear light (0.00439, 0.00439, 0.00699);
- the main window's alpha 0 over the photo and backdrop, its chrome opaque;
- faded in, 2.9 % of a Display P3 photo's pixels beyond sRGB (below 0);
- a Display P3 green (0, 255, 0) read back as (-0.2249, 1.0420, -0.0786);
- zoomed 2.2× and panned, the stage's photo and the viewer's frame the same
  rectangle at every one of 187 steps;
- a plugin's `holdSdr`, and Live Photos' motion played: the stage at SDR
  meanwhile, and back;
- an HLG clip through the stage's `VideoView` at the headroom (2 stops,
  4×): brightest 4.0; at 1×, the main window's frame to within a code;
- closed, the main window draws the photo again before the stage goes.

With no `PINHOLE_HEADROOM` the stage came by itself on that output (2.30
stops, from the surface's preferred description), and the output switched
to `cm = "srgb"` while the window ran (its config changed and reloaded), the
window said at once: "the window's display now: … SDR white at 80; no HDR
headroom". On that SDR output, forced, a `grim` of the output with the
stage faded in and one with the stage held back (the main window's SDR
photo) are the same to the bit.

`WAYLAND_DEBUG=client` shows it: `wl_subsurface.set_desync`,
`place_below(main surface)`, `create_parametric_creator`,
`set_primaries_named(1)`, `set_tf_named(5)` (ext_linear),
`set_image_description` on the stage's surface; `get_surface_feedback` and
`get_preferred_parametric` on the main window's.

Unit tests: the preferred description's events (203 of 1000: 2.30 EV; 80
of 80: 0; no target: the maximum), `stageColourOf` against `toSrgb` (every
code of each primary within a code, clipped; P3's green column), and the
stage wanted only where the display shows more, or forced.

**Performance.** On this panel the stage does not exist and the window is
made as before; what runs is one more registry round trip and the
preferred description's events, at start. Against main (51a9a8e),
interleaved on this panel:
- the first frame (`PINHOLE_SELFTEST=hidden`, the library copy, eight runs
  a side after a warm-up): from start 426 → 441 ms (median), 446 → 444 ms
  (mean);
- `--bench` on the fake library, nine runs a side: the phases' sync50
  0.201 → 0.169 ms, sync99 0.494 → 0.535 ms, all of the difference in two
  pinch phases that land at 0.45 or 0.8 ms run to run in both builds; the
  other twelve within 0.01 ms, and every p99 interval within 1.1 ms;
- `PINHOLE_SELFTEST=viewer`, three runs a side, all passing: HEIC original
  280 → 279 ms, Live Photo original 515 → 483, video first frame 203 →
  192, loop seek 125 → 124.

**In the app:** nothing changes on this panel. Forced on, the viewer's
photo is the same; on a display with headroom and fp16 swapchains, P3
photos show their full gamut and HDR video goes above SDR white.

### H4: gain maps in the stage

Landed (H4a and H4b together). What was built, and where it departs from
the plan:

**Reading the map** (`gainmap.cpp`, `readGainMap`): the pixels and how
they apply, `GainMap`, ISO's when it parses and its image decodes, else
Apple's (`PINHOLE_GAINMAP=apple` takes Apple's where both are, for tests).
- **HEIC, ISO:** the `tmap` item's metadata is read here (libheif 1.23 has
  no API for it), whole: per channel the gain's range (log2), gamma and
  both offsets, the two headrooms, `useBaseColourSpace`, one denominator
  or several; a map whose base is the HDR one (backward) is not used. Its
  two `dimg` inputs are the primary and the map. **On an iPhone the map
  is Apple's own auxiliary image** (`urn:com:apple:photo:2020:aux:hdrgainmap`):
  one picture, described twice, Apple's curve in its XMP and MakerNote and
  ISO's in the `tmap` (min 0, max = alternate headroom, gamma 0.834,
  offsets 1e-5, the base's colour space). libheif decodes it, as an image
  item or as the primary's auxiliary image, so risk 8 did not happen.
- **HEIC, Apple's:** that auxiliary image, `Hi` as H2 reads it.
- **Rotation:** an iPhone gives the map the primary's own `irot`, which
  libheif applies; a JPEG's map is turned as Qt turns the primary
  (its Exif orientation).
- **JPEG:** libultrahdr's `uhdr_dec_probe` reads ISO's APP2 or Adobe's
  `hdrgm` XMP (Apple's mediums, Android's and Lightroom's Ultra HDR) and
  hands over the map still compressed, which Qt decodes; nothing else of
  the JPEG is decoded twice. Without ISO's, Apple's map is the MPF second
  image. Some mediums say `useBaseColorSpace` 0; a one-channel map's gain
  is the same in any primaries that share white, so that matters only for
  three channels, which are applied in the photo's own primaries (the
  alternate's are not read).
- Over 112 cached originals and 400 mediums (`realGainMapsAgreeInEveryKind`,
  `PINHOLE_HDR_SAMPLE`): all 230 with a map decode it, of the kind the
  metadata says, the photo's shape once upright.

**Drawing it** (`StagePhoto`, `shaders/photo.frag`). On an scRGB stage
the decode thread reads the map after the photo; it goes to a texture of
its own (RGBA8, mipmapped with the photo), bilinear filtering being the
upscale. The shader gets the kind, `Hi`, ISO's parameters, the matrix back
to the photo's primaries and one weight, applied to the photo in linear
light after its gamut is mixed:
`(rgb + base offset) * 2^(G w) - alternate offset`, with `G` from the
code by the kind (Apple's: `log2(1 + (Hi - 1) g)`). A channel at or above
0 is not taken below 0 by the offsets (as libultrahdr has it); a colour
beyond sRGB's, below 0 already, stays. The weight is `hdrWeight` (the
fade, `holdSdr`, Full HDR) times the map's own at the display's headroom
(the maths above), so at 0 the shader returns the photo untouched. The
medium's map shows at once and the original's with the original, as the
photo does: each decode carries its own.

**Tested.**
- `aGainMapIsAppliedAsLibultrahdrDoes`: Ultra HDR JPEGs made with
  libultrahdr (a one-channel map, and a three-channel one with
  per-channel boosts, a minimum below 1 and offsets of 1/64), read by
  `readGainMap` and applied by `GainMap::apply`, against libultrahdr's
  own decode to linear half float at 1, ¾ and all of the alternate
  headroom: mean 0.03–0.05 %, worst 0.19 %.
- `isoGainMapMetadataIsReadWhole`: an iPhone's real `tmap`, a
  three-channel one over one denominator, cut short anywhere; Apple's gain
  and weight.
- `PINHOLE_SELFTEST=gainmap:<dir>` (`selfteststage.cpp`) in a nested
  Hyprland whose headless output is HDR (as H3; the monitor rule needs
  `supports_hdr = true` and the luminances), on a folder of an iPhone HEIC
  (ISO's map, then Apple's), two iCloud mediums (ISO's, one of them not in
  the base's colour space), an Apple-only medium and an Android-style Ultra
  HDR JPEG (libultrahdr, three channels, Display P3). Each read back from
  the stage's swapchain, RGBA16F, with `PINHOLE_HEADROOM` changed live:
  - at 0 stops (×1), the photo with its map is the photo with its map off,
    to the bit (every pixel of the frame);
  - at 1 stop and at the photo's own (1.5 to 3 stops), the frame against
    the maths computed on the CPU from that base and the map: mean
    relative error 0.03–0.05 %, 99 % within 0.2 %, worst 1.5 % (the
    three-channel map's sharpest edges); the brightest 9.4× SDR white;
  - in the log domain, `log2(C/B) / log2(D/B)` over the pixels half a
    stop or more above their base: the ratio of the weights, 0.4200 for
    0.4200 (median, 80 % within ±0.0005), for every photo and kind.
  - The gain drawn (`<photo>-gain.png`) lies on the photo, upright.
- H3's `stage` self-test still passes on these photos, except the
  hand-over of the Apple-only medium (1536×2048, mean 1.7 codes, worst 66):
  H3's own build (f0a506e) gives the very same numbers, so it is not H4's;
  open below.

**Performance.** Nothing runs without a stage: no map is read, no texture
made, and the grid and the window's own photo are untouched. In the stage,
one more texture fetch per pixel and a map decoded after the photo on the
stage's own thread (an iPhone HEIC's 2142×2856 map, a medium's 1024×768).

**In the app:** on this panel, nothing. On an HDR output, iPhone and
Android HDR photos go above SDR white, up to the display's headroom, and
fade in with the stage.

**Open:** the stage's hand-over differs from the main window's photo for
some photos shown smaller than their pixels (found on an Apple-only
medium; H3's as well).

### H5: HDR kept when photos leave

- `core/src/jpeg.rs`. For a HEIC with a gain map (found with `heif-dec
  --with-aux`, which writes the auxiliary images, or with `heif-info`):
  - make the base as today (`magick`, P3 profile kept);
  - turn Apple's map into ISO form: for each pixel `L = log2(1 + (Hi - 1)
    * g)` over `[0, log2 Hi]`, with gamma 1 and offsets 0;
  - write an Ultra HDR JPEG with libultrahdr's API-4 encoder (compressed
    base, compressed map, metadata), through a few `extern "C"`
    declarations by hand, as `main.cpp` does for Vulkan.
- Android, Chrome and Apple since iOS 18 read it.
- Copy and Export send originals as they are, and keep everything already.
- **Tests.**
  - `jpeg` of a HEIC with a map is a JPEG that libultrahdr decodes back to
    the map within 2/255.
  - Its base is byte for byte today's JPEG.
  - A HEIC without a map is as today.

## Risks, and the answers

1. **Asahi may not expose HDR for a long time.** *Answer:* H1, H2 and H5
   are whole on their own and are most of what Jacek will see. H3 and H4
   are tested end to end by forcing them, and cost nothing while they
   sleep (no stage, no alpha, no shaders loaded).
2. **The panel may be stretching sRGB to P3 on this kernel.** *Answer:*
   H0's check. If it does, everything on the desktop is equally off, and
   the fix is Hyprland's `dp3` or the kernel, not Pinhole. Pinhole is
   right for what the compositor says.
3. **honeykrisp plus Hyprland may not offer an fp16 scRGB surface.**
   *Answer:* H3's first test. Without it there is no stage, and H4 waits.
   That is no loss on this panel today. *Found:* on this panel's Hyprland
   it does not (honeykrisp's WSI offers no fp16 format there, though the
   compositor advertises `AB4H`); in a nested Hyprland on a headless output
   it does, and H3 and H4 are tested there.
4. **Private Qt API for the subsurface's stacking.** *Answer:* one
   function, a check against the Qt version built with, and no stage
   otherwise. Upstream Qt should honour `lower()` for subsurfaces, and a
   patch is worth sending.
5. **Two windows out of step.** *Answer:* the stage only shows while the
   viewer is settled, and the hand-over waits for the stage's frames with
   the photo (the subsurface desynchronised: H3). Zoom and pan drive both from the
   same properties. A plugin layer pinned to the photo can lag one frame
   while panning, 8 ms at 120 Hz.
6. **Qt Multimedia's HDR.** `hdrInfo()` is a constant on Linux; the
   `10000 *` is real but unused. *Answer:* H2 found Qt's SDR tone mapping
   wrong both ways and makes HDR frames SDR itself (`VideoView`'s shader).
   For the stage (H3), `VideoView`'s `headroom` maps to the display's
   headroom instead of SDR white, in the same shader. Both are worth reporting upstream.
7. **The CPU cost of conversion at decode.** *Answer:* measured in H1.
   Both `QColorTransform` and lcms2's own transform cost more than the
   decode itself, so the common profile is converted by Pinhole's own
   tables: half a decode for a tagged thumbnail, on the loader's threads,
   and the grid's frames as before (H1).
8. **libheif cannot decode the hidden `tmap` image.** *Answer:* Apple's
   legacy map is in every such file here, and H4a covers it. *Found:* on an
   iPhone the `tmap`'s map is Apple's auxiliary image, which libheif
   decodes (H4).
9. **Hyprland's colour management changes under us** (protocol version 2
   and 3 renamed transfer functions; `experimental:wp_cm_1_2` exists).
   *Answer:* `display.cpp` binds the version it was generated for, and
   accepts both `srgb` and `compound_power_2_4`, and both `gamma22` and
   `ext_srgb`.
10. **Hardware decode of 10-bit HEVC.** It is not part of HDR, but HDR
    video is 10-bit. *Answer:* Qt Multimedia already decodes HEVC Main 10
    through the VA-API shim on AVD, as P010, the same frame as software
    (H2).

## Sources

- [apple-spec] MacBook Pro (16-inch, 2023) technical specifications,
  https://support.apple.com/en-us/111838
- [asahi-6.19] https://asahilinux.org/2026/02/progress-report-6-19/
- [asahi-7.0] https://asahilinux.org/2026/04/progress-report-7-0/
- [asahi-7.2] https://asahilinux.org/2026/08/progress-report-7-2/
- [asahi-m2] https://asahilinux.org/docs/platform/feature-support/m2/
- [asahi-plane] https://raw.githubusercontent.com/AsahiLinux/linux/asahi/drivers/gpu/drm/apple/plane.c,
  and fp16 scanout, https://github.com/AsahiLinux/linux/commit/54dd41a
- [asahi-e49109f] https://github.com/AsahiLinux/linux/commit/e49109f
- [hypr-cm] https://raw.githubusercontent.com/hyprwm/Hyprland/main/src/protocols/ColorManagement.cpp
- [hypr-monitors] https://wiki.hypr.land/configuring/core/monitors/ and
  https://wiki.hypr.land/configuring/core/monitors/colors/
- [aquamarine-drm] https://raw.githubusercontent.com/hyprwm/aquamarine/main/src/backend/drm/DRM.cpp
- [qt-cm] https://www.phoronix.com/news/Qt-color-management-v1
- [qt-qwaylandwindow] https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/wayland/qwaylandwindow.cpp
  (L256–271) and `qwaylandcolormanagement.cpp`
- [qt-rhivulkan] https://github.com/qt/qtbase/blob/v6.12.0/src/gui/rhi/qrhivulkan.cpp
  (L9178–9224); GLES: `qrhigles2.cpp` L7263
- [qt-hdrinfo] https://doc.qt.io/qt-6/qrhiswapchainhdrinfo.html
- [qt-sgrhisupport] https://github.com/qt/qtdeclarative/blob/v6.12.0/src/quick/scenegraph/qsgrhisupport.cpp
  (L1545–1597)
- [qt-texturefrag] https://github.com/qt/qtdeclarative/blob/v6.12.0/src/quick/scenegraph/shaders_ng/texture.frag
- [qt-plaintexture] https://github.com/qt/qtdeclarative/blob/v6.12.0/src/quick/scenegraph/util/qsgplaintexture.cpp
  (L179–218), and `qsgrhiatlastexture.cpp`
- [qt-pixmapcache] https://github.com/qt/qtdeclarative/blob/v6.12.0/src/quick/util/qquickpixmapcache.cpp
  (L535–540)
- [qt-videotexturehelper] https://github.com/qt/qtmultimedia/blob/v6.12.0/src/multimedia/video/qvideotexturehelper.cpp
  (L664–688)
- [qt-ffmpegvideobuffer] https://github.com/qt/qtmultimedia/blob/v6.12.0/src/plugins/multimedia/ffmpeg/qffmpegvideobuffer.cpp
  (L144–158)
- Qt's FFmpeg hardware decoders,
  https://doc.qt.io/qt-6/advanced-ffmpeg-configuration.html
- [mesa-wsi] https://www.phoronix.com/news/Mesa-Vulkan-WSI-HDR-CM and
  https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/vulkan/wsi/wsi_common_wayland.c
- [libheif-1503] https://github.com/strukturag/libheif/pull/1503, and
  https://github.com/strukturag/libheif/releases
- [libavif-h] https://raw.githubusercontent.com/AOMediaCodec/libavif/main/include/avif/avif.h
- [android-uhdr] https://developer.android.com/media/platform/hdr-image-format
- [apple-gainmap] https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos
- libultrahdr, https://github.com/google/libultrahdr
- Dolby Vision 8.4, https://ott.dolby.com/browser_test_kit/help_files/topics/r_resources.html
- [ffmpeg-filters] https://ffmpeg.org/ffmpeg-filters.html (libplacebo,
  zscale, tonemap), and https://libplacebo.org/options/
- [ffmpeg-v4l2req] https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/20847
- [omacom-va] https://github.com/omacom/libva-v4l2_request, and AVD's
  status, https://www.phoronix.com/news/Asahi-Linux-AVD-Firmware-M3
- [grim-png] https://raw.githubusercontent.com/emersion/grim/master/write_png.c
  and https://github.com/emersion/grim/issues/95
- [weston-ini] https://man.archlinux.org/man/weston.ini.5
