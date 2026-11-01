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
- **Video.** The FFmpeg backend tone maps PQ and HLG in its shaders
  (BT.2390 style) to the swapchain's peak: 100 nits on SDR. It outputs
  linear on an scRGB swapchain [qt-videotexturehelper]. It takes the peak
  of a stream with mastering metadata as `10000 * max_luminance`, which
  looks like a bug (`max_luminance` is already in nits; unverified)
  [qt-ffmpegvideobuffer].
- `qt6-shadertools` (the `qsb` tool) is not installed, and Pinhole has no
  shaders of its own yet.

### Pinhole

- **Decoding.** `photoprovider.cpp`'s `decodePhoto` reads HEIC through
  libheif into 8-bit RGBA, and everything else through `QImageReader`. The
  colour profile is never looked at. `thumbnailloader.cpp` decodes the
  grid's thumbnails and mediums with turbojpeg into `RGBX8888`, and builds
  mosaics from them. The grid draws them with `QSGTextureMaterial` from the
  atlas. Library items' `thumbnail` is a `file://` URL, which the viewer
  and nine plugin files use in plain `Image`s.
- **The window.** It uses Vulkan when there is a GPU (`main.cpp`), with an
  opaque, untagged sRGB surface.
- **pinholed.** `render.rs` makes folder thumbnails and mediums with
  `magick -thumbnail`, which keeps the ICC profile (checked: a P3 medium's
  thumbnail stays "Display P3"). `jpeg.rs`, the one JPEG, keeps the
  profile too. `cover.rs` takes a video's frame with ffmpeg and no tone
  mapping, so an HLG folder video's cover comes out flat.
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
| HLG, Dolby Vision 8.4 and HDR10 video | Tone mapped by Qt Multimedia (checked in H2) | In range, in the stage (H3) | In range (H3) |
| Video covers (folders) | Tone mapped by ffmpeg (H2) | Same (covers are SDR) | Same |
| Live motion | SDR, tone mapped; the stage drops to SDR while it plays (H3) | Same | Same |
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
- **The conversion engine is `QColorSpace`/`QColorTransform`,** with no new
  dependency, and one transform cached per profile per thread. If H1's
  measurement shows a 480 px thumbnail costing more than 1 ms, lcms2 (already
  installed, 8-bit optimised transforms) replaces it behind the same
  function.
- **Headroom comes from Wayland, never from `hdrInfo()`.** It comes from
  `wp_color_management_surface_feedback_v1` on the main window's surface:
  `H = target_max_luminance / reference_luminance` of the preferred
  description, falling back to `max_luminance`. It is followed live when
  the compositor sends `preferred_changed`. `PINHOLE_HEADROOM=<stops>`
  overrides it for tests.
- **One swapchain format: scRGB.** If the Vulkan surface does not offer
  `HDRExtendedSrgbLinear`, there is no stage, logged once. There is no
  HDR10 fallback: Qt Multimedia's linear output and the photo shader
  would both need a second PQ path for no display we know of.
- **The stage needs a transparent main window,** which is chosen when the
  window is made. The display is read before the window is made. If the
  output gains headroom while Pinhole is open (another monitor, a
  brightness change into HDR), the stage starts the next time the window
  is made. Headroom changes within an HDR output apply at once.
- **A user switch, "Full HDR" ("Pełny HDR").** It is in the side panel,
  on by default, and shown only when the display has headroom. Off keeps
  the stage at SDR. Apple has the same choice. There is no other setting.
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
- **`qt6-shadertools`** becomes a make dependency in H3, for `qsb`.
- **Private Qt API, once.** Placing the stage below the main window needs
  `QtWaylandClient::QWaylandWindow::subSurfaceWindow()->place_below()`.
  Qt's `lower()` ignores subsurfaces. It is behind one function, with a
  version check: on a Qt it was not built against, there is no stage.
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

1. lands on `main` alone and is installed with `bin/install-local`;
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

### H1: every photo in the right colours

**Files.**
- New `app/src/colour.cpp`/`.h`:
  - `QColorSpace colourSpaceOf(const QByteArray &icc)`;
  - `QColorSpace colourSpaceOfNclx(const heif_color_profile_nclx *)`, for
    the primaries and transfer;
  - `void toSrgb(QImage &, const QColorSpace &)`. It skips sRGB and
    invalid spaces, and keeps a `thread_local QHash<QByteArray,
    QColorTransform>` keyed by the ICC bytes. Display P3 is nearly every
    hit.
- `photoprovider.cpp`:
  - In `decodeHeif`, read `heif_image_handle_get_color_profile_type`. For
    `prof`/`rICC` use `heif_image_handle_get_raw_color_profile`, and for
    `nclx` use `heif_image_handle_get_nclx_color_profile`.
  - Convert after scaling, so a 12 MP photo shown at 3456 px converts
    3456 px, not 12 MP.
  - `QImageReader` images already carry `colorSpace()`, so convert them the
    same way.
- `thumbnailloader.cpp`:
  - Set `TJPARAM_SAVEMARKERS` to keep APP2, then `tj3GetICCProfile`
    (libjpeg-turbo 3.2.1 has it).
  - `decodeJpeg` converts each thumbnail and medium before it is
    uploaded or put in a mosaic.
  - The average colour stays as pinholed computed it. It is a placeholder.
- `library.cpp`: an item's `thumbnail` becomes `image://photo/<path>`, so
  the viewer and every plugin `Image` get converted pixels with no change
  of their own. `thumbPath` stays a path.
  - The provider pool gets a third thread for the viewer's strip of
    thumbnails if the self-tests show waiting.
  - The QML pixmap cache keys by URL, so caching is unchanged.
- `viewer.cpp`: `thumbnail()` and `thumbnailAt()` return the same
  provider URLs.
- `docs/plugins.md`: `thumbnail` is now a provider URL. Use `thumbPath`
  for a file to hand to a program.

**Performance.**
- The conversion runs on the loader threads and in the provider pool,
  never on the render thread, and never per frame.
- The grid's draw path and atlas are unchanged.
- Budget: the bench's frame times within 5 %, and the fill phase's
  decode rate within 15 %. Over that, lcms2 replaces `QColorTransform`
  (`cmsCreateTransform` with `TYPE_RGBA_8`, cached the same way).

**Tests** (`app/tests/pinhole_tests.cpp`).
- Write a JPEG tagged with `QColorSpace(QColorSpace::DisplayP3).iccProfile()`
  holding P3 pure green and a P3 grey. After `decodePhoto` the green is
  sRGB (0, 255, 0) clipped, and the grey is unchanged.
- The same through `thumbnailloader`'s `decodeJpeg`.
- An untagged JPEG comes back byte for byte.
- A HEIC made with `heif-enc` and an nclx of primaries 12 (P3) converts
  like the JPEG.
- A "Camera RGB Profile" thumbnail from the real library (copied into the
  test data, 64 px) converts without error.
- `PINHOLE_SELFTEST=viewer` still passes, its times within budget.

**In the app:** iPhone photos look as saturated as on the phone, in the
grid, the viewer and every plugin.

### H2: HDR content shown right in SDR

**Files.**
- `photoprovider.cpp` and `colour.cpp`, for a still whose primary is PQ
  or HLG (an nclx with transfer 16 or 18; rare: one of 60 HEICs sampled
  has an nclx at all):
  - decode as `heif_chroma_interleaved_RRGGBB_LE`;
  - linearise;
  - tone map with BT.2390's EETF to SDR white = 203 nits for PQ, and with
    HLG's OOTF at a 1000-nit reference and system gamma 1.2 for HLG;
  - convert BT.2020 to sRGB, encode to 8-bit;
  - run it on the provider thread, in blocks over `QThreadPool`.
- `core/src/cover.rs`: `ffprobe` also reads `stream=color_transfer`. For
  `arib-std-b67` or `smpte2084` the filter becomes
  `zscale=t=linear:npl=203,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p`
  before the scale. ffmpeg here has `zscale` and `tonemap`, whose curves
  are clip, linear, gamma, reinhard, hable and mobius (no BT.2390). Covers made before are made again once:
  the cover's key gains a version.
- **Video.** No code unless the check fails. In a self-test, play an HLG
  clip and a PQ clip made with ffmpeg (`testsrc2`, tagged), and Jacek's
  own iPhone HLG and Dolby Vision 8.4 clips. Grab the frame and compare
  the mean of each colour bar with ffmpeg's `zscale`+`tonemap` of the same
  frame, within 6/255.
  - If a PQ clip with mastering metadata comes out black (the suspected
    `10000 *` bug), wrap the player's sink in a `QVideoSink` that corrects
    `maxLuminance` (Pinhole already links `multimedia-private`). Report it
    upstream.
  - Also try `QT_FFMPEG_DECODING_HW_DEVICE_TYPES=vaapi` with
    `LIBVA_DRIVER_NAME=v4l2_request` on an HEVC Main 10 clip and write
    down whether AVD decodes it. That is information, not a goal of this
    document.
- New `app/src/display.cpp`/`.h`, the Wayland side, read only in this
  phase:
  - `color-management-v1.xml` from `wayland-protocols` (staging), through
    `wayland-scanner` in `sources.pri`, as the gestures and idle-inhibit
    protocols are.
  - Bind `wp_color_manager_v1`. For each `wl_output`: `get_output` →
    `get_image_description` → `get_information`.
  - Collect `primaries_named`, `tf_named`, `luminances` and
    `target_luminance`, and work out `headroom` (stops) and `wide`
    (primaries wider than sRGB).
  - A `Display` object (`hdr`, `headroom`, `wide`) for QML.
- `pinhole display`: the same, from the command line, with no window. It
  prints each output's description. It is the oracle for H3's tests, and
  how Jacek sees what Hyprland reports.
- New `app/src/gainmap.cpp`/`.h`, metadata only in this phase. For a HEIC:
  - `heif_image_handle_get_list_of_auxiliary_image_IDs` and
    `heif_image_handle_get_auxiliary_type`, to find Apple's map;
  - the `tmap` item through `heif_item_get_item_type` and
    `heif_item_get_item_data`;
  - Exif MakerNote tags 33 and 48.

  For a JPEG:
  - the APP2 MPF index, and the second image's XMP
    (`HDRGainMap:HDRGainMapVersion`, `HDRGainMapHeadroom`);
  - Adobe's `hdrgm:` XMP;
  - ISO's APP2 `urn:iso:std:iso:ts:21496-1`.

  The result is `{kind: "apple" | "iso" | "pq" | "hlg" | "", headroom:
  stops}`.
- `viewer.cpp`: `Q_PROPERTY(QVariantMap hdr)` for the photo shown, read on
  the provider thread with the decode.
- **Plugin API 4** (`docs/plugins.md`):
  - `pinhole.display` (`hdr`, `headroom`, `wide`);
  - the viewer's `hdr` for the photo shown;
  - `pinhole.srgbProfile`, the path of an sRGB ICC installed with Pinhole
    (`/usr/share/pinhole/sRGB.icc`, written once from
    `QColorSpace(QColorSpace::SRgb).iccProfile()` and committed under
    `app/assets/`).
- `plugins/info`: a photo's line gains "HDR, 2.8 EV" ("HDR, 2,8 EV").
- `plugins/background`: `magick` gains `-profile <srgbProfile>` before
  `-resize`, so the wallpaper (an untagged sRGB surface) is in sRGB.
- `plugins/theme`: `Palette.magickArgs` gains the same, so the palette is
  taken from the colours the screen shows.

**Performance.**
- PQ and HLG tone mapping happens at decode, for the rare still that needs
  it.
- `gainmap.cpp` reads metadata only: a few kB of Exif and XMP, and no
  pixels.
- The grid is untouched.

**Tests.**
- A 16-bit PNG made into a PQ HEIC with `heif-enc` (nclx 9/16/9). Its
  decode matches a reference computed in the test from the same formulas,
  within 2/255.
- `cover.rs` unit test: an HLG `testsrc2` clip's cover has mean
  saturation within 10 % of the SDR clip's. Without the filter it is far
  below.
- `gainmap.cpp`:
  - An iCloud medium from the library (copied, cut to the MPF parts)
    gives `apple`, 2.79 EV. A HEIC original gives both kinds.
  - The HEIC's MakerNote headroom and its XMP headroom agree within
    0.1 EV. This settles linear against stops.
- `pinhole display` on this machine prints sRGB, gamma 2.2 or sRGB, and
  80/80 nits: no headroom.
- The video self-test above.

**In the app:** HDR videos and PQ/HLG stills look right in SDR, folder
video covers are no longer flat, and Info says which photos are HDR.

### H3: the stage (wide gamut and HDR output)

**Files.**
- New `app/src/stage.cpp`/`.h` and `app/src/Stage.qml`:
  - **The stage window.** A `QQuickWindow` whose parent is the main window
    (a subsurface on Wayland). Before it is shown, its
    `_qt_sg_hdr_format` is set to `scrgb`. Pinhole does not set its
    `QSurfaceFormat` colour space: Mesa tags the surface, and a second
    `get_surface` on it is a protocol error.
  - **Placed below** through the private API, behind one function.
  - **Content.** It holds the backdrop (the theme's background, made
    linear on the CPU) and the photo. Its geometry follows `viewer.zoom`,
    `panX` and `panY`, exactly as `frame` in `PhotoViewer.qml` does.
  - **Shown** only when `viewer.open && progress === 1 && offset === 0 &&
    dismiss === 0`.
  - **Sync.** The subsurface is set to sync. The main window turns its
    photo and backdrop transparent only after the stage's first
    `frameSwapped`, so both changes reach the screen in one commit.
- `app/src/shaders/photo.frag`, compiled with `qsb` in `sources.pri`. It
  takes:
  - the base texture, RGBA8, in the photo's own encoding: P3 is not
    converted for the stage;
  - a 3x3 to linear sRGB;
  - later (H4) the gain-map texture and parameters.

  It writes linear premultiplied scRGB.
- `photoprovider.cpp`: a stage request (`image://stage/<path>`) returns
  the decode without `toSrgb`, with its colour space as the texture's
  matrix. The main window's texture stays the sRGB one, for the motion.
- `main.cpp`: before the window is made, `Display` decides whether the
  stage is wanted (`headroom > 0 || wide`, or
  `PINHOLE_STAGE=force|off`). If so, the main window gets an alpha
  channel and `color: "transparent"` under its root, and `Main.qml`
  draws its own background.
- `display.cpp`: `get_surface_feedback` on the main window's surface,
  then `get_preferred_parametric` and `preferred_changed`. Headroom is
  followed live.
- **Video in the stage.** When the stage is up, the viewer's
  `VideoOutput` moves into `Stage.qml` (`MediaPlayer.videoOutput` is
  switched while the viewer is settled). Qt Multimedia then writes linear
  scRGB itself. Its peak is Qt's constant 1000 nits, and Hyprland tone
  maps the rest. While the viewer moves, the poster image is drawn in the
  main window as today.
- `PinholePlugin.holdSdr` (API 4). While any plugin's is true, the stage
  fades to SDR. The Live plugin sets it while motion plays, since its
  video is SDR in the main window.
- `SidePanel.qml`: the "Full HDR" switch, shown when `display.hdr`.

**Performance.**
- With no headroom and no wide gamut (this panel today), none of this
  exists. The window is made exactly as before, opaque and with no alpha.
- With the stage, the grid is `visible: false` while the stage is shown,
  so the frame cost is the stage's two quads plus the chrome.
- The stage keeps its own copy of the shown photo's texture (48 MB for
  12 MP), freed when the viewer leaves the photo.
- The main window's alpha costs the compositor one blend of the window,
  only on displays that have the stage.

**Tests**, all on this machine, with nothing it cannot do:
- `PINHOLE_STAGE=force PINHOLE_HEADROOM=2` self-test on workspace 5:
  - `QRhiSwapChain::isFormatSupported(HDRExtendedSrgbLinear)` on the
    stage. If false, this phase stops here: honeykrisp plus Hyprland do
    not offer fp16 scRGB, and the fix is upstream.
  - `grabWindow()` of the stage gives `Format_RGBA16FPx4`. The backdrop
    reads the theme's linear value. A P3 green test photo reads
    (-0.2249, 1.0421, -0.0786) × its level, within 1e-3. This is the exact
    value Pinhole hands Hyprland.
  - `WAYLAND_DEBUG=client` shows the stage's surface given
    `set_image_description` with `ext_linear`, the sRGB primaries and the
    extended target volume. It also shows the stage placed below its
    parent.
  - With the stage forced on SDR, a screenshot (`grim`, 8-bit, which is
    all it can write [grim-png]) of the settled viewer matches one with
    the stage off within 2/255. This proves the hand-over is seamless and
    that Hyprland maps scRGB back to the same SDR.
- **Headroom from a real compositor.**
  - A nested Hyprland, its own config, started on workspace 5 as a
    window, with its output given `supports_hdr = 1` and `cm = "hdr"`.
    `pinhole display` and the feedback inside it must report a headroom.
  - That a Wayland-backend output accepts the `hdr` preset is
    unverified: it is the first thing this step tries.
  - If it does not, Weston 15's headless backend with
    `color-management=true` and an HDR output is the second try
    [weston-ini]. It is not installed, and installing it needs Jacek's
    root password.
  - Jacek's own monitors are never configured.
- **The parsing of feedback events** is unit tested with synthetic
  luminances: 203/1000 gives 2.30 EV; 80/80 gives 0; a missing target
  falls back to max.
- `PINHOLE_SELFTEST=viewer` and `--bench` with the stage off are
  unchanged.

**In the app:** nothing changes on this panel unless the stage is forced
or Hyprland is set to `dp3`. With `dp3`, P3 photos in the viewer show
their full gamut.

### H4: gain maps in the stage

**H4a: Apple's maps.**
- `gainmap.cpp` decodes the map with the photo:
  - HEIC: `heif_image_handle_get_auxiliary_image_handle` and
    `heif_decode_image` to 8-bit monochrome;
  - JPEG: turbojpeg on the MPF second image.

  Rotation follows the base (libheif applies it to auxiliary images, to
  be checked; JPEG MPF maps follow the primary's Exif orientation).
- `image://stage/` returns the base and a second texture `R8` (a
  `QImage::Format_Grayscale8`, outside the atlas). The shader gets the
  kind, `Hi`, the display headroom, Full HDR, `holdSdr` and the fade, and
  applies the maths above. Bilinear sampling of the smaller map is the
  upscale.
- The medium's map shows at once, and the original's replaces it when it
  arrives, as the sharp image does today.

**H4b: ISO 21496-1.**
- Parse `tmap`'s binary metadata (version, per-channel fractions for min,
  max, gamma, base and alternate offsets, the two headrooms, and
  `useBaseColorSpace`), following libavif's reader.
- Decode its gain image through `heif_context_get_image_handle` on the
  `tmap`'s second `dimg` input. Whether libheif 1.23 decodes that hidden
  item is unverified, and is this phase's first test. If it does not,
  Apple's map stays in use for those files.
- Ultra HDR JPEGs (Android, Lightroom) go through libultrahdr:
  `uhdr_dec_set_out_img_format` SDR, `uhdr_get_decoded_gainmap_image` and
  `uhdr_dec_get_gainmap_metadata`. The base and map then go to the same
  shader.
- When both kinds are present, ISO wins.

**Performance.** The gain map is a quarter to a half the size, decoded on
the provider thread. One more texture fetch per pixel, only in the stage,
only for the photo shown.

**Tests.**
- A CPU reference (`applyGainMap(base, map, meta, headroom)` in
  `gainmap.cpp`) against `grabWindow()` of the stage at `PINHOLE_HEADROOM`
  0, 1, 2 and 3. Mean and maximum error over a 64x64 crop below 1e-2.
- **ISO.** The reference against libultrahdr's own decode of its test
  images (`uhdr_dec_set_out_max_display_boost`, half-float output) at the
  same boosts.
- **Apple.** The reference on a real iCloud medium at `Hd = Hi` against
  `sdr * (1 + (Hi - 1) * g)`, computed in the test directly.
- With `PINHOLE_STAGE=force` on SDR (headroom 0), the stage equals the
  base. Fading through 400 ms has no step above 1/255 between frames.

**In the app:** on this panel, nothing until HDR is exposed. Forced on,
Hyprland tone maps the highlights back into SDR. On an HDR output,
iPhone HDR photos glow as on the phone.

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
   That is no loss on this panel today.
4. **Private Qt API for the subsurface's stacking.** *Answer:* one
   function, a check against the Qt version built with, and no stage
   otherwise. Upstream Qt should honour `lower()` for subsurfaces, and a
   patch is worth sending.
5. **Two windows out of step.** *Answer:* the stage only shows while the
   viewer is settled. The subsurface is synchronised, and the hand-over
   waits for the stage's first frame. Zoom and pan drive both from the
   same properties. A plugin layer pinned to the photo can lag one frame
   while panning, 8 ms at 120 Hz.
6. **Qt Multimedia's HDR peak.** `hdrInfo()` is a constant on Linux, and
   there is the suspected `10000 *`. *Answer:* H2 checks SDR tone mapping
   against ffmpeg. In the stage, Hyprland tone maps whatever is above the
   display. Both are worth reporting upstream.
7. **The CPU cost of conversion at decode.** *Answer:* measured in H1
   against a 15 % budget on the decode rate, with lcms2 as the named
   replacement.
8. **libheif cannot decode the hidden `tmap` image.** *Answer:* Apple's
   legacy map is in every such file here, and H4a covers it.
9. **Hyprland's colour management changes under us** (protocol version 2
   and 3 renamed transfer functions; `experimental:wp_cm_1_2` exists).
   *Answer:* `display.cpp` binds the version it was generated for, and
   accepts both `srgb` and `compound_power_2_4`, and both `gamma22` and
   `ext_srgb`.
10. **Hardware decode of 10-bit HEVC.** It is not part of HDR, but HDR
    video is 10-bit. *Answer:* H2 writes down what the VA-API shim does.
    Without it, FFmpeg decodes in software, as today.

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
