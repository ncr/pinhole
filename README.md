# Pinhole

A fast photo browser for Omarchy: iCloud Photos on demand, or a folder of
photos on this computer. Working name.

Pinhole keeps an index of the whole library on disk (metadata and small
thumbnails) and fetches everything else from iCloud only when it is looked at,
dragged or exported, the way Photos on a Mac does with "Optimize Mac Storage".
It changes the library only by adding to it: photos and videos dropped on
the window go into it (uploaded to iCloud, copied into a folder). Nothing
in it is ever edited or deleted.

A folder works too: in the side panel (Tab), Add a source → a folder.
Pinhole indexes it and makes the thumbnails in the background, shows its
photos and videos as they are (an iPhone's `IMG_1234.HEIC` with
`IMG_1234.MOV` beside it is a Live Photo), and follows changes to it. How
sources fit in is in [docs/sources.md](docs/sources.md).

Everything that is set is in that side panel: the sources, where Pinhole
keeps its files and how much room they take, and the plugins.

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
on unless turned off in the side panel.

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
video's codec, frame rate and length), and where, with W to open the place
on OpenStreetMap. It stays open from photo to photo. A video's original over
50 MB is read only when asked (⇧ I).

Colours finds photos by colour (H): pick one on a wheel, or on a photo the
ones closest to its colour, or lay the library out by hue as a rainbow wall
(W). It goes by each photo's average colour, so it finds what is mostly one
colour (a sunset, the sea, a forest) and not a red coat on a grey street.

Screenshots keeps screenshots from crowding out the photos: each day's one
card, or (⇧ S, or the side panel) not in the grid at all, with a button
saying how many are away. A screenshot is what iCloud marks as one (an
iPhone's or an iPad's), a file named as one (screenshot-…, Screenshot …,
Zrzut ekranu …, hyprshot's, grim's), or a PNG with no place shaped like a
phone's screen.

A plugin of your own is a folder in
`~/.config/pinhole/plugins` with a manifest.json and a QML file, loaded
again whenever you save it: [docs/plugins.md](docs/plugins.md).

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

## Install

```bash
bin/install                  # builds the package and installs it (asks for your password)
```

Then open Pinhole from the launcher and add a source in the side panel
(Tab): an Apple ID (password and two-factor code, or the session icloudpd
already has) or a folder. The library builds in the background, the index
in about 5 minutes and every thumbnail in about 6 more for 50,000 photos,
and the grid fills as it does. `?` lists every shortcut.

The same from a terminal, without the window:

```bash
pinhole sign-in              # Apple ID, then the password and code, or icloudpd's session
pinhole status               # the source shown, its library, how far it is built
pinhole help                 # every command: sources, show, remove, sync, add, storage…
```

Each asks the Pinhole service, `pinholed`, and starts it if it is not
running; only the service writes the library.

Working on Pinhole, `bin/install-local` builds the checkout into `~/.local`
instead, with no password and no package, and stops a running window so the
launcher opens the new one.

## Development

Status: two spikes grown into an app. The iCloud API probe is in
[docs/api-findings.md](docs/api-findings.md); the photo grid, at 120 Hz on a
synthetic 100k library, is in [docs/grid-spike.md](docs/grid-spike.md).

## Layout

```text
core/                     the Rust core: the service, the index, the cache, source kinds;
                          the workspace root (Cargo.lock, target/) for all the Rust below
core/daemon/              pinholed, the background service, with the built-in sources linked in
core/src/bin/fake.rs      pinhole-fake, a synthetic library for the grid
plugins/icloud/source/    iCloud as a source (crate pinhole-icloud): session, CloudKit, walk, push, upload
plugins/icloud/source/src/bin/probe.rs  pinhole-probe, which measures the API
plugins/icloud/auth/PROTOCOL.md  the sign-in provider protocol: JSON lines over stdio
plugins/icloud/auth/pyicloud/pinhole-auth  the first provider, on the pyicloud module from icloudpd
plugins/folder/source/    a folder as a source (crate pinhole-folder)
app/                      the window: Qt Quick, with the grid in C++; and `pinhole <command>` (commands.cpp)
plugins/series/           the Series plugin; finder/ is pinhole-series, which finds them
plugins/background/       the Background plugin: a photo or video as the desktop background
plugins/onthisday/        the On this day plugin
plugins/calendar/         the Year calendar plugin: photos per day, a year a row
plugins/sheet/            the Contact sheet plugin: a grid or collage of what is ticked, made by magick
plugins/send/             Send to phone; sender/ is pinhole-send, LocalSend's protocol
plugins/info/             the Info plugin; pinhole-info reads an original's EXIF (ImageMagick) or a video's (ffprobe); check/ tries it on real files
plugins/colour/           the Colours plugin: photos by their colour, and the rainbow wall
plugins/theme/            the Theme plugin: an Omarchy theme from a photo (Palette.js, pinhole-theme)
plugins/screenshots/      the Screenshots plugin
plugins/frame/            the Photo frame plugin
```

## Trying the grid

On your own library (after `pinhole sign-in`, once `pinhole status` says
it is built):

```bash
app/bin/build
app/build/pinhole            # ? lists every key and gesture; S toggles the frame stats
app/build/pinhole --bench    # scripted run, prints frame timing per phase
app/bin/test                 # unit tests: scroll physics, viewer motion, translations, theme, library
```

`PINHOLE_BENCH_ONLY=pinch` runs only the phases with that in their name;
`PINHOLE_BENCH_GRAB=<folder>` saves the window 20 times a second (slow on
screen; recording the window with `wf-recorder -g` shows real timing).
`PINHOLE_SELFTEST=mouse` drives clicks, swipes, pinches and select mode
with synthetic events and reports each step.
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
the original (HEIC through libheif). ← and → step through, Esc or
Space closes, as does dragging the photo up or down (two fingers on a
touchpad, the left button with a mouse), Space plays and pauses a video, L
plays a Live Photo's motion.
A Live Photo shows with the effect set on the iPhone (a loop or a bounce
plays round and round, a long exposure is its frames as one); the ▾ beside
Live, or ⇧ L, shows it with another, here only: the library keeps its
own.
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
camera details) to a folder chosen in the desktop's own picker. Originals
come from iCloud as they are needed.

Without a real library the window opens the synthetic one:

```bash
core/target/release/pinhole-fake --count 100000   # ~1 min, 6 GB in ~/.local/share/pinhole/fake
app/build/pinhole --library ~/.local/share/pinhole/fake
```

A library given so, other than your own, has a service of its own (on a
socket named after its path), so Pinhole opened from the launcher meanwhile
still shows your photos.

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
cargo build --release --workspace --manifest-path core/Cargo.toml
core/target/release/pinhole-probe count
core/target/release/pinhole-probe walk --parallel 4
core/target/release/pinhole-probe stats
core/target/release/pinhole-probe changes
core/target/release/pinhole-probe sample --kind JPEGThumb --n 200
core/target/release/pinhole-probe stream
```

The index lands in `~/.local/share/pinhole/probe.sqlite`; the provider keeps
its session in `~/.local/state/pinhole/auth`, readable only by you.
