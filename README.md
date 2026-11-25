# Pinhole

A fast photo browser for Omarchy: iCloud Photos on demand, or a folder of
photos on this computer. Working name.

Pinhole keeps an index of the whole library on disk (metadata and small
thumbnails) and fetches everything else from iCloud only when it is looked at,
dragged or exported, the way Photos on a Mac does with "Optimize Mac Storage".
It adds to the library: photos and videos dropped on the window go into it
(uploaded to iCloud, copied into a folder). It never deletes from it. It
edits a photo only when you ask: Turn and crop (R, K) on an iCloud photo
changes that photo in iCloud, so your iPhone shows it turned or cropped
too; Ctrl Z turns it back.

A folder works too: in the side panel (Tab), Add a source → a folder.
Pinhole indexes it and makes the thumbnails in the background, shows its
photos and videos as they are (an iPhone's `IMG_1234.HEIC` with
`IMG_1234.MOV` beside it is a Live Photo), and follows changes to it. How
sources fit in is in [docs/sources.md](docs/sources.md).

Everything that is set is in that side panel: the sources, where Pinhole
keeps its files and how much room they take, and the plugins. iCloud,
Folders, Favourites, Turn and crop, Live Photos, Info, Send to phone and
Screenshots are on from the start; Series, People, the calendar, On this
day, Mosaic, Rainbow wall, Contact sheet, Photo frame, Background and
Theme are there too, off until turned on.

Small marks in a photo's corner say what it is: a video, a Live Photo, a
favourite, several photos as one card, and whatever a plugin marks; each
can be turned off in the side panel.

Series, the first plugin, finds photos of one subject taken close together
and shows each series as one card in the grid; under a photo of one, the
strip underlines the series. What it finds can be put right: on a photo,
X takes it out of its series, A adds it to the series beside it, U
ungroups the series; in select mode, G groups what is ticked. Ctrl Z
undoes any of it, as it does a favourite, and Ctrl ⇧ Z does it again. It
also (G in the grid, or on a photo of one) makes a series a short GIF or
video to send: Flip, Bounce, Fade, Drift or Pile, at a pace, in a look and a
shape, held steady; ⏎ copies it and keeps it in Pictures/Pinhole. It is
off until turned on in the side panel.

On this day shows the photos taken on today's date in earlier years, a year
at a time, the latest first: D in the grid, where its button says how many
years there are. In the side panel, the day alone or a few days around it,
and whether videos and screenshots count.

Contact sheet makes the photos ticked in select mode one picture to send
(K): an even grid, or a collage that keeps each photo whole, with a gap, a
background, a size and the dates if you like; ⏎ copies it and keeps it
in Pictures/Pinhole.

Send to phone (O, on a photo or what is ticked) sends photos to a phone or
computer nearby with LocalSend open: the devices show as they answer, and
once its owner accepts, the originals go, a Live Photo with its video, or
HEIC as JPEG (J).

Info, on I on a photo, shows its details in a small panel over its corner:
when it was taken, the camera, lens and exposure, its size and format (a
video's codec, frame rate and length), and where. It stays open from photo
to photo. A video's original over 50 MB is read only when asked (⇧ I).

Map (M in the grid or in a located photo's Info panel) shows the current
library's photos and videos on a map Pinhole draws itself from
OpenFreeMap's vector tiles of OpenStreetMap: in the theme's colours, sharp
at any scale, with names in your language where the map has them and in
Latin letters elsewhere. Drag or slide
two fingers to move, and pinch or turn the wheel to zoom. Click or tap a
marker to open a photo or see its group, with the years it was taken in;
a photo opened there steps ← → through that place alone. Use **Zoom to
group** to look closer, or double click the map to zoom at the pointer.
**Photos in view** follows the visible area as you move.

With the keyboard, arrows choose a place or navigate its photo grid, Shift
with arrows moves the map, and Enter opens. Home fits all places, + and −
zoom, Z looks closer at the selected group, and G toggles **Photos in view**.
Escape goes back one view, and M closes the map.
GPS comes from iCloud or local metadata without fetching originals. Existing
libraries fill in their GPS data on the next catalog refresh. Pinhole sends
only the requested map tile coordinates to OpenFreeMap; the photo index
and thumbnails stay local. Cached tiles and photo markers work offline.

Mosaic makes any photo out of your other photos (H): each tile is a photo
of the colour the picture has there. A slider, + and −, or the wheel make
the tiles bigger and smaller, Space flips to the original and back, a
click on a tile opens its photo, and S saves the mosaic to Pictures.

Rainbow wall lays as many of your most colourful photos as fill the window
out by hue (W), light at the top and dark at the foot. Space makes a wall
of other photos, + and − or a pinch make them bigger and smaller, and a
click on one opens it. It goes by each photo's average colour, so it is of
photos that are mostly one colour (a sunset, the sea, a forest) and not a
red coat on a grey street.

Screenshots keeps screenshots from crowding out the photos: each day's one
card, or (⇧ S, or the side panel) not in the grid at all, with a button
saying how many are away. A screenshot is what iCloud marks as one (an
iPhone's or an iPad's), a file named as one (screenshot-…, Screenshot …,
Zrzut ekranu …, hyprshot's, grim's), or a PNG with no place shaped like a
phone's screen.

A plugin of your own is a folder in
`~/.config/pinhole/plugins` with a manifest.json and a QML file, loaded
again whenever you save it: [docs/plugins.md](docs/plugins.md). It can
bring a source of photos of its own (Immich, a NAS, a phone) through a
provider, a program in any language that pinholed talks to in JSON lines:
[docs/source-providers.md](docs/source-providers.md).

Settings (Tab) → Plugins → Build a plugin opens the offline plugin workshop:
a quick start, searchable API/reference sections, an Examples tab with
complete view/tool/source plugins, and instructions to copy for an agent. Tab/Shift Tab move through settings,
Escape closes them, and left/right change a focused section tab. In the
workshop, Ctrl Tab / Ctrl Shift Tab change documents and Ctrl F searches.
`pinhole plugin docs --json` gives the
installed API version and the same bundled documents to tools. Start with
[the quick start](docs/plugin-quickstart.md) or [the agent contract](docs/plugin-agents.md).

The [local photo-content search plugin](docs/research/photo-search-plugin.md)
indexes thumbnails automatically in pinholed, even after the window closes. Ctrl F opens
search over the whole window: describe a photo in English or Polish and the
best matches come first. ⏎ opens the best one, and ← → go through the
answers alone; Esc comes back to them. A line under the question says how
much of the library can be found yet. The local
profiles use WeMM 4B on NVIDIA CUDA, CLIP on Apple Silicon with ANE,
or WeMM 4B on Apple GPUs with Asahi Vulkan when explicitly enabled.

[People](plugins/people/README.md) groups similar faces in thumbnails
automatically in pinholed, even after the window closes. Shift+F shows the
people named so far and the faces not named yet. A group opens with every
name a key away (1–9), the likeliest first; once a few photos have a name,
groups that look like that person say so, and C on the person checks them
one after another. On a photo shown full window, a button says who is in it
(Anna, or Anna? for who a face looks like); W rings the faces and names each
one in turn: ⏎ for the likely one, a name typed, or Not Anna when it is wrong.
The names you give, and which faces are or are not someone, go to a Pinhole
folder in the iCloud Drive of the library's Apple ID, with the name of the
computer they were given on, so your other computers know them too. The
photos and faces themselves stay on the computer.

Background: B on a photo shown full window makes it the desktop background;
a video becomes a moving one, and on a Live Photo V its motion, as taken or
as its Loop, and ⇧ V its Bounce (both also under ▾, ⇧ B). In Series' studio,
B makes the video just made the background. Each is kept in
Pictures/Pinhole/.backgrounds, hidden from a folder source: the newest 12,
and any set now or a theme will bring back; a file put there otherwise is
never removed. Ctrl Z puts the one before back.

Year calendar (Y in the grid) shows every day of every year as a square in
the theme's accent, brighter the more photos it has, one row of weeks a
year; a click, or ↑ ↓ ← → and ⏎, shows that day in the grid.

Theme makes an Omarchy theme from the photo shown full window: T (or
Theme) sets it, the photo as the background and its colours on the whole
desktop, Pinhole too, the text always readable; Ctrl Z goes back to the
theme before. The last 12 made or used stay in Omarchy's theme picker; a
copy of one under another name is yours, and stays.

Photo frame (Frame, P, in the grid; P on a photo to start from it, or in
select mode for what is ticked) shows photos one after another, full
window, each slowly panned and zoomed and faded into the next: favourites,
all, or those taken on this day in other years, shuffled or in order,
videos and Live Photos moving. The screen stays on while it shows. Esc or
a click stops it, ← → step, and a two-finger tap or Space holds it still.

Zooming the grid sharpens visible tiles with the medium rendition, or the full
photo when the preview is missing or too small. Decode size follows the square
crop in device pixels, including portrait screenshots. Full-photo decodes run
two at a time once motion settles.

C copies the photo under the grid cursor, or the selected photos in select
mode. A small confirmation appears once the clipboard is ready; the Copy
button confirms selected photos in place. Ctrl C remains available too.

The main grid defaults to Automatic order: photos added in the last 30 days stay
at the newest end, ordered by addition date. Older additions sit where their
capture dates belong. The date chip's menu (or Ctrl Shift O) and the side
panel offer Automatic, Date taken and Date added; the last choice is kept
as the default. Capture dates, calendar and series data stay unchanged. The
viewer follows the grid's order too. A newly extended series shows its latest
addition on its card; archived series keep their original cover.

The main grid ends in a complete row; spare cells before its oldest photo
form a history card with the library's years, colours and a jump to the newest.

## Install

Pinhole is for Arch Linux, and made for and tried on Omarchy. These parts
need Omarchy itself: the bar widget, Background and Theme (they set
Omarchy's background and theme), and opening Pinhole where the bar's icon
is dropped (Hyprland). Elsewhere the window, its sources and the other
plugins are there, in Pinhole's own colours rather than the theme's.

There is no package yet: Pinhole installs from this repository, in one
command:

```bash
git clone https://github.com/ncr/pinhole && pinhole/bin/install
```

`bin/install` installs what Pinhole needs and builds with through pacman
first (sudo asks for your password once), then builds it, which takes
several minutes the first time, and links it into `~/.local` for your
account. On Omarchy everything else is there already; a bare Arch install
needs `git` and `sudo` before it. Run it as yourself, not with
`sudo`. `gum`, if installed, gives it a small menu and a spinner.

```bash
bin/install                  # terminal UI; build and run directly from this checkout
bin/install --non-interactive # the same source install without the UI
bin/install --uninstall      # remove this account's install; keep photos/settings/repo
```

Then open Pinhole from the launcher and add a source in the side panel
(Tab): an Apple ID (password and two-factor code, or the session icloudpd
already has) or a folder. The library builds in the background, the index
in about 5 minutes and every thumbnail in about 6 more for 50,000 photos,
and the grid fills as it does. `?` lists every shortcut.

The same from a terminal, without the window:

```bash
pinhole sign-in              # Apple ID, then the password and code, or icloudpd's session (iCloud's plugin on)
pinhole status               # the source shown, its library, how far it is built
pinhole help                 # every command: sources, show, remove, sync, add, storage…
```

Each asks the Pinhole service, `pinholed`, and starts it if it is not
running; only the service writes the library.

On Omarchy, `bin/install` also gives the bar a Pinhole widget: its camera
obscura lights up with the number of photos that came since you last
looked, and a click shows the newest as a small grid to drag them from
into a chat, a browser or a folder. Click photos to drag several at once.
Photos dropped on the icon are added to the source shown, as on the window;
a right click opens Pinhole. Dragged off the bar with the right button, the
icon opens Pinhole where you let it go, in the tile under the pointer as
Hyprland places a new window; an open Pinhole moves there.
It goes on the bar with `omarchy bar put io.github.ncr.pinhole`. It reads
what `pinhole recent --follow` says, drags what `pinhole export` makes (a
HEIC as a JPEG, as a drag out of the window does) and adds with
`pinhole add --json`, whatever the source.

`bin/install` builds the checkout and links the launcher, service and plugins to
it in `~/.local`, without a package or root password. QML plugins stay live in
the repository. After a successful install, running windows and their service
are stopped, then those windows reopen with their original arguments and
session environment. If Pinhole was closed, it stays closed. A failed build
leaves running processes alone. `PREFIX` changes the installation folder;
`--non-interactive` bypasses the menu for scripts. `--dev` remains an alias
for existing developer update helpers.

`pkgbuild/` holds a recipe for an Arch package, `pinhole-git`, not
published anywhere yet, and its `.SRCINFO`; after changing the
recipe, write that again with `makepkg --printsrcinfo > .SRCINFO`.
`bin/install` does not build or install packages. Both install
`share/licenses/pinhole/`: Pinhole's license and those of the Rust crates
its programs link, which `bin/build` writes (`bin/third-party-notices`).

Developer installations quietly check `main` hourly. A small Update button, also in the
side panel under Pinhole updates, builds the new version while you browse,
apart from your checkout, which moves to it only once all of it is built;
choose Restart when it is ready. Package updates and removal use the OS
package manager. Developer updates require a clean `main`
checkout and only fast-forward it, preserving local work. A checkout
without Git or an `origin` (a downloaded source archive) installs too, with
no Update button.

`--uninstall` stops this installation's Pinhole and its service (and with
it the plugins' work in the background, People's and the search's), then
removes only files recorded by the local/developer installer;
changed or replaced files are preserved, as are your photos, preferences,
custom plugins and source checkout. It never removes a system package. A
source's saved sign-in (an Apple ID's session) stays too: to forget it,
run `pinhole sign-out <source>` first, a source as `pinhole sources` lists
it.

## Development

`bin/build` builds the Rust helpers with Cargo and the window with Qt.
Source installations and package builds use this same entry point.
Cargo builds core/pinholed, Series and Send with their checked-in lockfiles.

Status: two spikes grown into an app. The iCloud API probe is in
[docs/api-findings.md](docs/api-findings.md); the photo grid, at 120 Hz on a
synthetic 100k library, is in [docs/grid-spike.md](docs/grid-spike.md).

The [2026-11-03 project review](docs/reviews/2026-11-03.md) records the
architecture, UI/UX and compiler correctness findings; see also the
[repeatable startup measurements](docs/startup-performance.md). The current
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
plugins/mosaic/           the Mosaic plugin: a photo made of the library's photos (Mosaic.js picks them by colour, mosaic.frag draws them)
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
[docs/service.md](docs/service.md).

Double-click a photo, or press Space or ⏎ over it, to open it full
window: the thumbnail at once, the medium rendition in about a second, then
the original (HEIC through libheif). Every photo is in its right colours,
in the grid and the plugins too: an iPhone's Display P3, and any other
profile a photo carries, made the screen's sRGB as it is decoded
([docs/hdr.md](docs/hdr.md)). HDR shows right on an SDR screen: an
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

The [project review](docs/reviews/2026-11-03.md) records the architecture and
measured findings. [ci/README.md](ci/README.md) documents reproducible checks
for the compiler, Rust, Qt/plugin API, installer, accessibility, rendering and
native Wayland startup. The Rust checks also run the independent JPEG
fixtures with SIMD and scalar decoding.
