# Developing Pinhole

`bin/build` builds the Rust helpers with Cargo and the window with Qt.
Source installations and package builds use this same entry point.
Cargo builds core/pinholed, Series and Send with their checked-in lockfiles.

Status: two spikes grown into an app. The iCloud API probe is in
[docs/api-findings.md](api-findings.md); the photo grid, at 120 Hz on a
synthetic 100k library, is in [docs/grid-spike.md](grid-spike.md).

The [2026-11-03 project review](reviews/2026-11-03.md) records the
architecture, UI/UX and compiler correctness findings; see also the
[repeatable startup measurements](startup-performance.md). The current
native warm first frame is about 391 ms median on the review machine;
older frame-rate results describe their original hardware and revision.

## Layout

```text
core/                     the Rust core: the service, the index, the cache, source kinds;
                          the workspace root (Cargo.lock, target/) for all the Rust below
core/daemon/              pinholed, the background service, with the built-in sources linked in
core/src/process.rs       sources of plugins' own, run out of process by their providers
core/daemon/tests/provider/  the test source, a provider in Python (docs/source-providers.md)
core/src/bin/fake.rs      pinhole-fake, a synthetic library for the grid
plugins/icloud/           the iCloud plugin: its sources' rows, what Add a source offers, the sign-in (ICloudSignIn.qml)
plugins/icloud/source/    iCloud as a source (crate pinhole-icloud): session, CloudKit, walk, push, upload
plugins/icloud/source/src/bin/probe.rs  pinhole-probe, which measures the API
plugins/icloud/auth/PROTOCOL.md  the sign-in provider protocol: JSON lines over stdio
plugins/icloud/auth/pyicloud/pinhole-auth  the first provider, on the pyicloud module from icloudpd
plugins/folder/           the Folders plugin: its sources' rows, the folders offered, Choose a folder…
plugins/folder/source/    a folder as a source (crate pinhole-folder)
app/                      the window: Qt Quick, with the grid in C++; and `pinhole <command>` (commands.cpp)
plugins/live/             the Live Photos plugin: the mark, L and ⇧ L, the motion and effects over the photo
plugins/edit/             Turn and crop: R turns the photo shown, K crops it, kept in Pinhole (adjustment.h), undo
plugins/favourites/       the Favourites plugin: the mark, F and its heart in the viewer and select mode, undo
plugins/series/           the Series plugin; finder/ is pinhole-series, which finds them
plugins/background/       the Background plugin: a photo or video as the desktop background
plugins/onthisday/        the On this day plugin
plugins/calendar/         the Year calendar plugin: photos per day, a year a row
plugins/sheet/            the Contact sheet plugin: a grid or collage of what is ticked, made by magick
plugins/send/             Send to phone; sender/ is pinhole-send, LocalSend's protocol
plugins/info/             photo details and the library map; pinhole-info reads an original's EXIF (ImageMagick) or a video's (ffprobe); check/ tries it on real files
plugins/info/module/      Info's native QML module, Pinhole.Internal.Info: the map drawn from vector tiles, built with the window
plugins/mosaic/           the Mosaic plugin: a photo made of the library's photos (Mosaic.js picks them by colour, mosaic.frag draws them, pinhole-mosaic finds the photos that suit one)
plugins/screens/          the Photos of screens plugin; pinhole-screens finds them (MobileNetV2 and a head of its own, head.json)
plugins/wall/             the Rainbow wall plugin: the library's most colourful photos laid out by hue
plugins/theme/            the Theme plugin: an Omarchy theme from a photo (Palette.js, pinhole-theme)
plugins/screenshots/      the Screenshots plugin
plugins/frame/            the Photo frame plugin
```

## Trying the grid

On your own library (after `pinhole sign-in`, once `pinhole status` says
it is built):

```bash
app/bin/build
app/build/pinhole            # ? lists keys and gestures; Ctrl Shift F toggles frame stats
app/build/pinhole --bench    # scripted run, prints frame timing per phase
app/bin/test                 # unit tests: scroll physics, viewer motion, translations, theme, library
bin/test-all                 # every test, at once, on Xvfb (about a minute and a half)
```

`PINHOLE_BENCH_ONLY=pinch` runs only the phases with that in their name;
`PINHOLE_BENCH_GRAB=<folder>` saves the window 20 times a second (slow on
screen; recording the window with `wf-recorder -g` shows real timing).
`PINHOLE_SELFTEST=mouse` drives clicks, swipes, pinches and select mode
with synthetic events and reports each step (in a floating window, on
photos this computer has: app/src/selftest.h); `PINHOLE_SELFTEST=through`
does the same over everything that lies over the photos (the side panel at
its ends, the date, the help and the other cards, menus, the viewer) and
fails if any of it reaches the grid.
Videos play without sound in a selftest and in the bench; `PINHOLE_SILENT=1`
does the same for any other run.

`pinholed` keeps the library in step with iCloud on its own: new, deleted
and edited photos appear within a minute (at once when Apple pushes the
change), without a restart; `pinhole sync` asks it to look now.

When cells grow past what the 480 px thumbnails can fill, the window asks
`pinholed` for the 2048 px rendition. The window starts the service if it is
not running; it keeps the photos it fetches beside the library, in
`~/.local/share/pinhole`, within one size limit for both (the folder and the
limit can be changed in the side panel, Tab), and logs to
`~/.local/state/pinhole/pinholed.log`. Its protocol is in
[docs/service.md](service.md).

Double-click a photo, or press Space or ⏎ over it, to open it full
window: the thumbnail at once, the medium rendition in about a second, then
the original (HEIC through libheif). Every photo is in its right colours,
in the grid and the plugins too: an iPhone's Display P3, and any other
profile a photo carries, made the screen's sRGB as it is decoded
([docs/hdr.md](hdr.md)). HDR shows right on an SDR screen: an
iPhone's HDR and Dolby Vision videos and their covers, and photos in PQ
or HLG, are tone mapped rather than flat or broken, and Info (I) says how
many stops an HDR photo's highlights reach; `pinhole display` prints what
the compositor says each screen can show. On a screen that shows more than
sRGB, a photo full window shows its whole gamut, and HDR photos (an
iPhone's gain map, Android's Ultra HDR) and HDR video go above white, once
the photo is still (Full HDR in the side panel, on such a
screen only); on an sRGB screen nothing of that is made. ← and → step through, Esc or
Space closes, as does dragging the photo up or down (two fingers on a
touchpad, the left button with a mouse), Space plays and pauses a video, L
plays a Live Photo's motion (the Live Photos plugin, on unless turned off
in the side panel; off, a Live Photo shows as its still).
A Live Photo shows with the effect set on the iPhone (a loop or a bounce
plays round and round, a long exposure is its frames as one); its button
at the top right is named for the effect shown (Live, Loop, Bounce, Long
exposure), and the ▾ beside it, or ⇧ L, shows it with another, here
only: the library keeps its own. F, or the heart, the first of the
buttons at the top right, makes the photo a favourite or not,
in iCloud too, and in select mode all that is ticked (the Favourites
plugin, on unless turned off in the side panel; off, there is no heart
and no mark, and the Photo frame still shows the favourites the source
has).
`PINHOLE_SELFTEST=viewer app/build/pinhole` opens one of each kind and
reports how long every stage took.

Drop photos and videos (or folders of them) from a file manager on the
window to add them to the source shown, as `pinhole add <files>` does
from a terminal; `PINHOLE_SELFTEST=drop:<dir>` with
`PINHOLE_SELFTEST_DROP=<file>:<file>` drops files on the window and
reports, for a scratch source only, since they really are added.

Click selects (Ctrl click adds, ⇧ click a range, Ctrl A all). Drag
photos out to any app; Ctrl C copies them; Ctrl E exports the originals and
Ctrl ⇧ E a compatible form (HEIC as JPEG, keeping the date, place and
camera details, and an iPhone's HDR as an Ultra HDR JPEG) to a folder
chosen in the desktop's own picker. Dragging
and Ctrl C give the compatible form too. The JPEGs are pinholed's own
(`jpeg`), made once into the cache, as Send's are. Originals come from
iCloud as they are needed.

Without a real library the window opens the synthetic one:

```bash
core/target/release/pinhole-fake --count 100000   # ~1 min, 6 GB in ~/.local/share/pinhole/fake
app/build/pinhole --library ~/.local/share/pinhole/fake
```

A library given so, other than your own, has a service of its own (on a
socket named after its path), so Pinhole opened from the launcher meanwhile
still shows your photos. It is shown even where no source is set up yet (a
new computer, a scratch `HOME`): the first-run picker is for a window that
was given none.

The two boundaries that are meant to be swapped:

- **Sign-in.** iCloud's source starts a provider process and asks it for a
  session (cookies, service URLs, query params). How the provider signs in is
  its own business. A native provider only has to speak
  `plugins/icloud/auth/PROTOCOL.md`.
- **Photo source.** `core/src/source.rs` defines `Source`, and
  `core/src/kinds.rs` the kinds of source. iCloud and folders are the
  built-in ones, each the Rust of its plugin (`plugins/icloud/source`,
  `plugins/folder/source`), linked into pinholed (`core/daemon`).

## Trying the probe

The provider runs with [uv](https://docs.astral.sh/uv/), which fetches
icloudpd into its cache on first use. To adopt a session icloudpd already
signed in (it is copied, not shared):

```bash
echo '{"id":1,"method":"import_icloudpd","params":{"apple_id":"you@example.com"}}' | plugins/icloud/auth/pyicloud/pinhole-auth
```

Then, from the repository root:

```bash
cargo build --release --locked --workspace --manifest-path core/Cargo.toml
core/target/release/pinhole-probe count
core/target/release/pinhole-probe walk --parallel 4
core/target/release/pinhole-probe stats
core/target/release/pinhole-probe changes
core/target/release/pinhole-probe sample --kind JPEGThumb --n 200
core/target/release/pinhole-probe stream
```

The index lands in `~/.local/share/pinhole/probe.sqlite`; the provider keeps
its session in `~/.local/state/pinhole/auth`, readable only by you.

## Project checks

The [project review](reviews/2026-11-03.md) records the architecture and
measured findings. [ci/README.md](../ci/README.md) documents reproducible checks
for the compiler, Rust, Qt/plugin API, installer, accessibility, rendering and
native Wayland startup. The Rust checks also run the independent JPEG
fixtures with SIMD and scalar decoding.
