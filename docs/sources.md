# Sources

Pinhole began as an iCloud Photos browser, but almost nothing in it needs to
know that. The window reads `library.sqlite` and the thumbnails from disk and
asks the service for a photo's `medium`, `full`, `original`, `video` or
`live` file by asset id, or a Live Photo's effect (`loop`, `bounce`,
`long_exposure`). What it needs is a list of assets with dates and
kinds, a thumbnail for each, and a way to get the larger files. Anything that
can give those is a source. iCloud is one; a folder of photos on disk is the
second.

This is how the core is split so the rest of Pinhole talks to a source
without knowing which one it is.

## The trait

`core/src/source.rs` has the neutral types and the `Source` trait. A source
has two parts.

**The catalog** says what is in the library:

- `list(index)`: every asset there is, put into the index as it comes, and
  the change token to follow from. Assets the index had but the listing did
  not put are removed afterwards, by the core.
- `changes(index, token)`: what changed since the token: assets put and
  removed, and the next token. A token the source no longer accepts is
  `TokenExpired`, and the core lists everything again.

What the catalog puts is a neutral `Asset`: its id, file name, the time it
was taken and added, whether it is a photo, a video or a Live Photo, its
size, whether it is a favourite, a video's duration, its orientation and
whether it has a place, a Live Photo's effect as set on the iPhone (loop,
bounce, long exposure; iCloud's only), and the renditions it has. A `Rendition` is one of
the neutral kinds (`thumb`, `medium`, `full`, `original`, `video`, `live`)
with its size and type (a UTI, as the service has always answered), the
source's own name for it, and a key that names the file and changes when the
file does. The catalog decides which of its files serves each kind: iCloud's
`medium` is JPEGMed when the photo has one and JPEGFull otherwise.

A source may keep more about each asset than the neutral row holds, in
tables of its own in the same index (below). It writes them itself, beside
`index.put`, and the index gives it the connection to do so.

**The fetch** gets a rendition's bytes:

- `local(rendition)`: the file itself, when it is already on this computer
  and should be served as is (a folder's originals). Nothing is copied.
- `prepare(index, wanted)`: get many renditions ready to read at once, one
  ticket each. iCloud looks up fresh download links, a hundred assets to a
  lookup, using the links in its table while they are under 20 minutes old.
  A folder needs nothing.
- `open(ticket, range)`: start reading one, whole or a byte range. A ticket
  that has gone stale (an expired link) is `Expired`, and the caller
  prepares it again, once.

The service puts what `open` reads into its cache, as before; streaming a
video while it downloads reads byte ranges through `open` too. The
thumbnail pass reads `thumb` renditions the same way, so iCloud's are
downloaded and a folder's are made from the photo.

**Adding** puts photos and videos from this computer into the library:

- `accepts()`: the extensions it takes; none for a source that cannot
  be added to.
- `add(addition, sent)`: one asset, a photo or a video with a Live
  Photo's motion beside it (`add::gather` makes them of the files and
  folders given), with how many bytes have gone. A folder copies it in
  (`copy_into` in pinhole-folder: never over a file, the two halves of a
  Live Photo under one name), iCloud uploads it (pinhole-icloud's
  `upload.rs`). An `AddError` says why one cannot be added; the library
  hears of what was added at the next round, as of any other change.

**Favourites** are the one thing besides adding that a window changes:

- `set_favorite(index, ids, favorite)`: make these assets favourites, or
  not, where the source keeps that. iCloud sets `isFavorite` on each
  CPLAsset (pinhole-icloud's `favorite.rs`, with the change tags in
  `icloud_assets`, which it updates); a folder keeps them in its library
  (`folder_favorites`) and puts them back on its assets each time it
  describes them. The core sets `assets.favorite` once it has worked. A
  source that keeps none says `Unsupported`.

A source also says its name ("iCloud", or the folder's), whether it needs
signing in, how often to look for changes (a minute; a folder ten seconds),
and what an error means for the connection: `needs_sign_in`, `offline` or
`failing`, the health the window has always been told.

A source's `id` ("icloud", or "folder:" and the path) is written into the
library it fills, and the core never lets another source write there
(`sync::claim`): a folder pointed at iCloud's library by mistake must not
list 50,000 photos away. A library of another folder is emptied and made
again; one from another source, or one that says nothing of where it came
from (pinhole-fake's), is left alone.

## The index

`assets` keeps only what every source has, the columns the window reads
among them. What only iCloud means is in iCloud's own tables:

```text
assets          id, filename, item_type, asset_date, added_date, favorite,
                duration, orientation, has_location, is_video, is_live,
                live_effect, size, thumb_path, thumb_w, thumb_h, avg_color
renditions      asset_id, kind (thumb|medium|full|original|video|live|
                loop|bounce|long_exposure),
                name, key, width, height, size, file_type
meta            key, value: change_token, library_version, source,
                average_colour

icloud_assets   asset_id, master_id, change_tag, rank, subtype
icloud_resources  asset_id, kind (Original, JPEGMed, ...), width, height,
                size, file_type, checksum, url, url_fetched_at
icloud_records  asset_id, asset_json, master_json (pinhole-probe's)

folder_favorites  asset_id (a folder's favourites, kept nowhere else)
```

`item_type` (the original's UTI) and `size` are neutral; `subtype` (Apple's
asset subtype) is not and moves. Removing an asset from `assets` removes its
renditions and the source's rows too, by triggers the source sets up with
its tables, so the core never names them.

A rendition's `key` is what its cached file and thumbnail are named by.
For iCloud it is the file's checksum.

`avg_color` is the thumbnail's average colour in sRGB: the average of its
codes, through its ICC profile (`core/src/colour.rs`), as the window shows
the thumbnail itself ([hdr.md](hdr.md)). A library from before that has
its averages made sRGB once, by the first round of a service that knows
(meta `average_colour` = `srgb`).

## The service

`service.rs` holds one `Arc<dyn Source>`, and names no kind of source: what
it knows of each apart from the trait is the kind's (`core/src/kinds.rs`,
the `Kind` trait): what its settings entries look like and their source
id, making its source, its sources implied without settings (the Apple IDs
signed in to), describing one for `sources`, the entry `set_source` adds,
`discover`, and signing in and out. `IcloudKind` and `FolderKind` are
compiled in, from their plugins' crates (below; `core/daemon` puts them
together as pinholed); each follows the switch of its plugin, `pinhole.icloud` or `pinhole.folder`, in `plugins.json`
(`core/src/plugins.rs`), and a kind that is off is not listed or shown,
with nothing of it deleted (docs/service.md, Kinds). `fetch` finds the rendition in
`renditions`, serves `local` files as they are, else the cached file by
key, else gathers requests for 25 ms and hands them to `prepare`, and the
download threads to `open`. Its protocol (docs/service.md) is unchanged,
with two additions: `status` and a new `source` event say the source's
`name` and whether it `needs_sign_in`, and `set_source` changes it.

The sync thread asks the catalog for changes every minute (every ten
seconds for a folder): `changes` from the saved token, or `list` when there
is none or the token has expired, and makes thumbnails for whatever lacks
one. With no library yet it builds one from scratch. It is the only
writer of a library: `pinhole <command>` from a terminal asks it, as the
window does (`pinhole sync` for a round now, `pinhole sign-in` to add an
Apple ID and show it).

## Where the built-in sources' Rust is

Each built-in source is the Rust of its plugin, a crate in the plugin's
folder, in the workspace whose root is `core/` (one `core/Cargo.lock`, and
`core/target`, where pinholed is built): `plugins/icloud/source` is
`pinhole-icloud` (with `pinhole-probe`), `plugins/folder/source` is
`pinhole-folder`. Each depends on `pinhole-core` for the `Source` and
`Kind` traits, the index and the sync; `core/daemon` (`pinholed`) depends
on all three and builds the `Kinds` of the two. The core names neither.

## Sources of your own

Any other source is a plugin's, out of process: its manifest names a
*provider*, a program in its folder that pinholed starts and talks to in
JSON lines ([source-providers.md](source-providers.md)). `ProcessKind` and
`ProcessSource` (`core/src/process.rs`) are its `Kind` and `Source`; the
plugins' folders are looked through for them as their switches are read
(`plugins::provided`, `Kinds::refresh`), and a kind must be its plugin's id
or start with it, so none takes another's, nor `icloud` or `folder`. The
provider says what there is, in assets as the trait has them; pinholed
checks each and writes the library itself, and reads the files from the
`path` or `url` tickets it gives (making thumbnails and mediums it has none
of with the folder's renderer, `render.rs`). It never writes the library,
and no bytes pass through it. One that hangs, sends what is not the
protocol or crashes is stopped and started again at most once in ten
seconds; its sources' libraries never pay for it.

## The iCloud source

`plugins/icloud/source` holds what is Apple's: the sign-in provider
(`auth.rs`, and `plugins/icloud/auth/PROTOCOL.md`, whose provider
`pinhole-auth` is in `plugins/icloud/auth/pyicloud` and installed beside
pinholed), the session, the CloudKit client, the pairing of
asset and master records, the parallel walk and the change feed (moved out
of `sync.rs`), and the link lookups and their refreshing (moved out of
`service.rs`). Its walk and change feed are as they were; they now put
neutral assets and write `icloud_assets` and `icloud_resources` beside
them.

## The folder source

`plugins/folder/source` makes a library of a folder (and the folders in it).
An asset's id is its path inside the folder. Photos (JPEG, HEIC, PNG, and
anything else ImageMagick reads) and videos (MOV, MP4, M4V) are assets; a
video with the same name as a photo beside it (`IMG_1234.HEIC` and
`IMG_1234.MOV` or `IMG_1234_HEVC.MOV`, as iPhones and icloudpd write them)
is that photo's Live Photo motion, not an asset of its own.

The date taken is the photo's EXIF date (`kamadak-exif`, which reads JPEG,
HEIF, PNG and TIFF), a video's creation time and duration from `ffprobe`,
and the file's modification time when neither says. A rendition's key is a
hash of the path, size and modification time, so a changed file gets a new
thumbnail. `original`, `full`, `video` and `live` are the files themselves;
`thumb` (480 px, kept in the library like iCloud's) and `medium` (2048 px,
made into the cache when first asked for, as iCloud's are downloaded) are
made with ImageMagick, and a video's by ffmpeg from the most telling of
seven frames spread across it (`cover.rs`), not a fade from black; an HDR
video's (an iPhone's HLG, PQ) tone mapped to SDR, not flat. A video's key
also has how its cover is made (`cover::VERSION`), so a change to that
makes every cover again once.
ImageMagick reads the file on stdin, so no name is taken for its own
syntax (`50%off.jpg` would be `500ff.jpg` to it).

Following changes is a fresh look at the folder each round, compared with
the index by path, size and modification time. A folder is not on a
network, so this is quick (a stat of every file) and needs no change token
beyond the time of the last look. A folder that is not there (a disk not
mounted) is an error, `offline`, never an empty library. In the same way,
what a look cannot read (a folder without permission, a failing disk) is
unseen, not gone: the photos the library has there stay, favourites and
all, until a look gets through to them again. The folder itself unread is
an error too, `failing`, as is a photo's file that cannot be read for
anything but not being there.

Making thumbnails is the slow part: a JPEG takes about 40 ms, but a 12 MP
HEIC about a CPU-second to decode, in ImageMagick or vips alike. ~/Pictures
(625 assets, mostly HEIC) took 41 s on 12 cores; a folder of 50,000 HEICs
would take an hour or so in the background. Windows see the photos as their
thumbnails arrive, every four seconds.

That must never slow the computer down (`background.rs`). ImageMagick,
ffmpeg and ffprobe run at idle priority (SCHED_IDLE, nice 19), so whatever
the person does goes first, and only on four in five of the physical cores
with their second threads, so the rest stay free however many threads a
decoder starts itself (libheif does). On an 8-core Ryzen, Pictures/iCloud
(654 assets) took 37 s at 80% of the CPU, and a single-threaded task beside
it ran 7% slower instead of twice as slow. As many run at once as those
cores have threads; a provider's video read from a URL by ffmpeg, which
mostly waits on the network, takes one of two places of its own instead.
One is never stopped for taking long: on a computer busy with a build it
may wait minutes for a processor, and then goes on. It is stopped, and its
place is free, when it has done nothing for two minutes (no processor time,
nothing read or written, not waiting for a processor: a mount that no
longer answers), or has used five minutes of processor time (a file that
sends a decoder round in circles).

A file the program turns down (it makes no image of it, or spins on it: a
HEIC or a video cut short, an empty JPEG) is that file's alone, never the
folder's health: the rest of the folder is fine, and the window says
nothing of it but that the photo could not be loaded (`unreadable`). It is
not made again for six hours unless it changes, and so its key does (a
program updated meanwhile may read it); pinholed keeps that in mind while
it runs. One that hangs, a folder gone or unreadable, is the folder's:
`offline` or `failing`, and tried again each round. The same holds for a
provider's photos made from a file, and for an iCloud video whose cover
(`cover.rs`) ffmpeg cannot make: it is not fetched again for one.

## Choosing a source

`~/.config/pinhole/settings.json` keeps every source added, in order, and
the one shown:

```json
{"sources": [{"kind": "icloud", "account": "me@example.com"},
             {"kind": "folder", "path": "/home/me/Pictures/iCloud"}],
 "source": {"kind": "folder", "path": "/home/me/Pictures/iCloud"}}
```

Without `sources` they are the Apple IDs signed in to, then the folder
shown if it is one; without `source`, the first of them; `"source": null`
is none, as on a first run. An iCloud entry without an `account` is the
only Apple ID signed in to, if there is just one. With no settings.json at
all, then, one Apple ID signed in to is the one source, and shown.
`storage::resolve` says this for every kind through its `Kind` (`normal`,
`implicit`), and a test holds it to what it said before there were kinds,
input for input. The source shown is the one named while its kind's
plugin is on, else the first added whose kind is on (`Kinds::shown`); the
settings are not written for that.

Every source has a library of its own in the one folder Pinhole keeps
everything in, under its kind: `library/icloud/<apple-id>/` for each Apple
ID, `library/folder/home-me-Pictures/` for each folder (its path, with
dashes; one with a dash of its own also gets a hash of it after a tilde,
so `/a-b/c` and `/a/b-c` are not both `a-b-c`). Switching between them rebuilds nothing, and a library is only
ever written by its own source (`sync::claim`). All of them share `cache/`
(files named by their keys, which do not clash) and the one limit, and a
move of the folder takes `library/` as a whole. Removing a source deletes
its library, and an Apple ID's session.

Each Apple ID's session is kept apart too, in
`~/.local/state/pinhole/auth/<apple-id>/` (the provider's `--state-dir`),
with the owner's name as Apple gives it, which the window calls the source
by. The service signs in for the window and for `pinhole sign-in` alike
(`sign_in`, `submit_code`): either way the Apple ID is added and shown, and
its library listed if it has none yet.

## The window

It never sends anyone to a terminal. With no source it offers what
`discover` finds (icloudpd's sign-in, folders of photos) and signs in to
iCloud itself. What it shows of each kind is that kind's plugin's
(`plugins/icloud`, `plugins/folder`: a `PluginSource` each, docs/plugins.md):
a source's row in the side panel and its buttons, what the first run and
Add a source offer, the sign-in, and the source's place in a sentence. The
window keeps what is the same for every kind: the list, 1–9, Remove,
progress, rounds, dropping files, and the card saying a source needs
signing in, whose button is its plugin's. Turning a plugin on or off in the
side panel writes `plugins.json` and sends `plugins_changed`, so pinholed
lists and shows what is on at once; with sources it shows one at a time and switches between
them (`sources`, `set_source`, `remove_source`), and a library being built
fills the grid as `progress` says how far it is. It says the source's name
where it means the source (the health line, the viewer's messages), and
asks to sign in only for a source that needs it.
