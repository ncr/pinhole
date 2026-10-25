# Sources

Pinhole began as an iCloud Photos browser, but almost nothing in it needs to
know that. The window reads `library.sqlite` and the thumbnails from disk and
asks the service for a photo's `medium`, `full`, `original`, `video` or
`live` file by asset id. What it needs is a list of assets with dates and
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
whether it has a place, and the renditions it has. A `Rendition` is one of
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

The window's columns in `assets` stay as they are, so the window reads an
old and a new library alike. What only iCloud means moves out:

```text
assets          id, filename, item_type, asset_date, added_date, favorite,
                duration, orientation, has_location, is_video, is_live,
                size, thumb_path, thumb_w, thumb_h, avg_color
renditions      asset_id, kind (thumb|medium|full|original|video|live),
                name, key, width, height, size, file_type
meta            key, value: change_token, library_version, source, schema

icloud_assets   asset_id, master_id, change_tag, rank, subtype
icloud_resources  asset_id, kind (Original, JPEGMed, ...), width, height,
                size, file_type, checksum, url, url_fetched_at
icloud_records  asset_id, asset_json, master_json (pinhole-probe's)
```

`item_type` (the original's UTI) and `size` are neutral; `subtype` (Apple's
asset subtype) is not and moves. Removing an asset from `assets` removes its
renditions and the source's rows too, by triggers the source sets up with
its tables, so the core never names them.

A rendition's `key` is what its cached file and thumbnail are named by.
For iCloud it is the file's checksum, as the cache has always used, so the
photos already in the cache stay where they are and are found again.

### Migrating

An index from before (no `schema` in `meta`) is migrated in place as it is
opened, in one transaction, after a copy of it is saved beside it as
`library.v1.sqlite` (with `VACUUM INTO`, so it is whole even while the
service has it open):

1. `resources` becomes `icloud_resources` and `raw_records` becomes
   `icloud_records`, as they are.
2. `icloud_assets` is filled from `assets` (`master_id`, `change_tag`,
   `rank`, `subtype`).
3. `renditions` is filled from `icloud_resources`, choosing for each kind
   what the service chose so far (`medium`: JPEGMed, else JPEGFull, else
   JPEGLarge; `full`: JPEGFull, VidFull, Original; `video`: VidMed, VidFull,
   Original; `original`: Original; `live`: OriginalVidCompl; `thumb`:
   JPEGThumb), keyed by the checksum, else by `<asset>-<name>` as before.
4. Those columns are dropped from `assets`, and `size` is added (the
   original's).
5. `sync_token` (else `walk_sync_token`) becomes `change_token`; `source`
   becomes `icloud`; `schema` becomes 2.

Nothing the window shows changes, and no thumbnail moves. The copy stays
until it is removed by hand; it is how to go back to an older Pinhole.

## The service and pinhole-sync

`service.rs` holds one `Arc<dyn Source>`. `fetch` finds the rendition in
`renditions`, serves `local` files as they are, else the cached file by
key, else gathers requests for 25 ms and hands them to `prepare`, and the
download threads to `open`. Its protocol (docs/service.md) is unchanged,
with two additions: `status` and a new `source` event say the source's
`name` and whether it `needs_sign_in`, and `set_source` changes it.

The sync thread asks the catalog for changes every minute (every ten
seconds for a folder): `changes` from the saved token, or `list` when there
is none or the token has expired, and makes thumbnails for whatever lacks
one. With no library yet it builds one from scratch; for iCloud that used to
be `pinhole-sync`'s job only.

`pinhole-sync` does the same from a terminal for whichever source the
settings name: `index`, `thumbs`, `all`, `changes`. `sign-in` is iCloud's,
and only offered for it.

## The iCloud source

`core/src/icloud/` holds what is Apple's: the sign-in provider (`auth.rs`,
and `auth/PROTOCOL.md`), the session, the CloudKit client, the pairing of
asset and master records, the parallel walk and the change feed (moved out
of `sync.rs`), and the link lookups and their refreshing (moved out of
`service.rs`). Its walk and change feed are as they were; they now put
neutral assets and write `icloud_assets` and `icloud_resources` beside
them.

## The folder source

`core/src/folder.rs` makes a library of a folder (and the folders in it).
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
made with ImageMagick, and a video's from a frame by ffmpeg.

Following changes is a fresh look at the folder each round, compared with
the index by path, size and modification time. A folder is not on a
network, so this is quick (a stat of every file) and needs no change token
beyond the time of the last look. A folder that is not there (a disk not
mounted) is an error, `offline`, never an empty library.

Making thumbnails is the slow part: a JPEG takes about 40 ms, but a 12 MP
HEIC about a CPU-second to decode, in ImageMagick or vips alike. ~/Pictures
(625 assets, mostly HEIC) took 41 s on 12 cores; a folder of 50,000 HEICs
would take an hour or so in the background. Windows see the photos as their
thumbnails arrive, every ten seconds.

## Choosing a source

`~/.config/pinhole/settings.json` gains `source`:

```json
{"source": {"kind": "folder", "path": "/home/me/Pictures"}}
{"source": {"kind": "icloud"}}
```

No `source` means iCloud, as before. Each source has its own library in
the one folder Pinhole keeps everything in: iCloud's stays `library/`,
a folder's is `library-folder/` (made again from scratch when the folder
changes). Both share `cache/` and the one limit; moving the folder moves
both. The window's settings panel picks the source, and the service
switches to it as it runs: windows are told the library moved, and read
the new one.

## The window

It stops saying iCloud where it means the source: the health line, the
viewer's messages, the empty library's advice and the settings panel use
the name the service gives, and only a source that needs signing in is
said to.
