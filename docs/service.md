# pinholed: the background service

The window never talks to the source of the photos (iCloud, or a folder;
see [sources.md](sources.md)). It reads the library index and the
thumbnails straight from disk, and asks `pinholed` for everything else: the
larger renditions of a photo, originals, videos. The service owns all
network access and all writes (to the library too: photos dropped on the
window are added through it), so the window stays a fast reader. It also
follows the source's changes and tells the window when the library changed.

The window starts the service if none answers, passing the socket
(`$PINHOLE_SOCKET`, else `$XDG_RUNTIME_DIR/pinhole.sock`) and a library
given to the window (`--library`) other than the one the settings name.
Such a library (pinhole-fake's, a copy for a test) has a service of its
own, on `$XDG_RUNTIME_DIR/pinhole-<hash of its path>.sock` unless
`$PINHOLE_SOCKET` names one: the shared service is never asked for it, nor
started with it, which would show it to every window after.

That service is started with `--given` too: the library is only read and
fetched for. It is opened read-only (no journal mode set, no table made,
iCloud's fresh download links kept in memory, not written into it), so
its file is left as it was. Nothing is synced into it (no rounds, the source neither
followed nor watched: `rounds` names no source), nothing of the cache is
evicted (the shared service's to do), and a window asking it to change
anything (`set_source`, `remove_source`, `sign_in`, `submit_code`,
`sign_out`, `add`, `set_favorite`, `set_storage`) is answered `read_only`.
A library of another source than the settings' (pinhole-fake's) has
nothing fetched either (`other_source`); a copy of that source's own has
its photos fetched as the shared service would, into the same cache. It
logs to `$XDG_STATE_HOME/pinhole/<its socket's name>.log`, begun afresh,
and ends once no window has been connected to it for 10 seconds, after
the downloads under way (half a minute at most: any cut short leave no
part file in the cache). A window is gone however its connection ended,
a bug in answering it too (see Transport), and a file not got for a bug
(a panic in a source) is answered with an error rather than left under
way. Ending, it stops the programs still making something (ffmpeg,
ImageMagick: none goes on writing into the cache after it), removes
what it was making thumbnails from (`cache/making/<pid>/`), and removes
its socket only if that is still its own: a service started on the same
path after its socket was removed keeps it.

## Transport

A Unix socket at `$XDG_RUNTIME_DIR/pinhole.sock`, one JSON object per line
each way. Requests carry an `id`; replies echo it and may arrive in any
order, since a cached file answers at once while a download takes a while.

The first line a window hears is which protocol the service speaks, and its
version:

```json
{"event": "hello", "protocol": 1, "version": "1.0.0"}
```

`protocol` goes up whenever either side would misread what the other says.
A window sends nothing before it has heard it, and asks nothing of a
service that speaks another one: it says so (health `incompatible`,
below), and whatever it asks is answered `incompatible` until another
service answers. A service whose first line is not a `hello` speaks
protocol 0, from before there was one.

Lines to each window wait in a queue of its own, so a window slow to read
holds up no other, nor anything the service is doing. One that does not
read what it is sent for five seconds, or falls 4096 lines behind, is hung
up on; it connects again and starts afresh. So is one whose request the
service tripped over (a panic, a bug): the rest go on.

```json
{"id": 7, "method": "fetch", "params": {"asset": "A5379A3C-...", "kind": "medium"}}
{"id": 7, "ok": true, "result": {"path": "/home/me/.local/share/pinhole/cache/medium/3f/AWx...jpg", "type": "public.jpeg", "rendition": "JPEGMed", "cached": false}}
{"id": 8, "ok": false, "error": {"code": "unavailable", "message": "..."}}
```

## Methods

| Method | Params | Result |
|---|---|---|
| `fetch` | `asset` (the asset record name, the index's `id`), `kind`, optionally `stream`, and `source` (the id of the source whose photo it is: else `other_source`, as for `""`, no one's). A library read that is not the source shown's (one given with `--library`) has nothing fetched: `other_source` | the local file: `path`, `type` (a UTI), `rendition`, `cached`; with `stream`, maybe a `stream` URL instead of waiting (see Streaming) |
| `status` | | `library`, `cache`, `cache_bytes`, `in_flight`, `windows` (how many are connected), `health`, `source` (as the `source` event), `progress` (as the `progress` event, or null), `rounds` (as the `rounds` event) |
| `sync` | | `{}`: look for changes in the source now rather than at the next round |
| `add` | `files`: absolute paths of photos, videos or folders of them | how each went, once all are done; meanwhile `adding` events to this window (see Adding) |
| `set_favorite` | `assets` (ids from the index), `favorite` (true or false), and `source` (the id of the source they are of, optional) | `changed`: how many were not so already; see Favourites |
| `sources` | | every source added whose kind is on, as the `sources` event |
| `kinds` | | every kind of source, as the `kinds` event |
| `plugins_changed` | | `{}`: plugins.json was written (a plugin turned on or off), read again now; see Kinds |
| `set_source` | an `id` from `sources`, or `kind` (`icloud` or `folder`, one that is on) and an iCloud `account` (Apple ID) or a folder's `path` | the source, as the `source` event: added to the sources if new, and shown; see Sources |
| `remove_source` | `id` | the `sources` left; see Sources |
| `sign_out` | an Apple ID's `id` | the `sources`, that one still among them; see Sources |
| `discover` | | what could be added: `icloudpd` and `folders`, and each kind's under `kinds`; see Sources |
| `sign_in` | `kind` (`icloud` unless said), `apple_id`, and `password` unless icloudpd is signed in to it | `state`: `signed_in` (added and shown) or `needs_code` |
| `submit_code` | `kind` (`icloud` unless said), `apple_id`, `code` (six digits) | `state`: `signed_in` (added and shown) |
| `storage` | | where and how much: `location` (the one folder), `library`, `cache`, `limit_bytes`, `min_limit_bytes`, `default_limit_bytes`, `limit_set`, `used_bytes` (all of it), `kinds` (bytes of `library` and of each kind of photo), `freeing`, `moving` (`done_bytes`, `total_bytes`, or null) |
| `set_storage` | `limit_bytes` and/or `location` (an absolute folder) | as `storage`, after the change (see Storage) |
| `ping` | | `{}` |

`kind` picks the rendition: `medium` (about 2048 px), `full` (the photo as
it looks now: the edit, if it has one), `original` (as taken, unedited),
`video`, `live` (a Live Photo's motion), a Live Photo's effects: `loop`,
`bounce` (the motion forward and back) and `long_exposure` (its frames as
one photo), and `jpeg` (`full` as a JPEG, for whatever reads no HEIC; see
below). Which of the source's files serves each was chosen as the
library was indexed (the index's `renditions`); `rendition` is the source's
name for it. For iCloud:

| kind | renditions tried |
|---|---|
| `medium` | JPEGMed, JPEGFull, JPEGLarge |
| `full` | JPEGFull, VidFull, Original |
| `original` | Original |
| `video` | VidMed, VidFull, Original (videos only) |
| `live` | OriginalVidCompl |
| `loop`, `bounce` | VidCompl, when that is the effect set on the iPhone |
| `long_exposure` | JPEGFull, when that is the effect set on the iPhone |

An effect the source has no file for is made from the motion
(`effects.rs`): a `loop` is the `live` file itself, played round and round;
a `bounce` is ffmpeg's MP4 of it forward then reversed (about a second); a
`long_exposure` is every frame held to the first one's framing (vidstab's
tripod mode) and averaged into a JPEG (about five seconds for a 1920x1440
motion). They are answered with `rendition: "made"` and kept in the cache
beside the motion, named after its key, so each is made once.

A `jpeg` of a photo whose `full` is a JPEG is that `full`, answered as a
`full` would be (its `rendition`, `cached`). Any other photo's is made
from its `full` with ImageMagick at quality 92, which keeps the EXIF (the
date, the place, the camera), the colour profile, and turns the pixels
upright with the orientation set to match; only the first image of a file
that holds several, and anything see-through on white. It is answered with
`type: "public.jpeg"`, `rendition: "made"`, and kept in the cache beside
`full`, as `cache/full/…/<full's key>-jpeg.jpg`: counted as `full` in
`storage`, under the limit like any photo, and made once (two at a time at
most; those asking for one being made wait for it). A video has none:
`not_found`.

A folder's `full`, `original`, `video` and `live` are the files themselves,
answered with `cached: true` and their own path; its `medium` is made into
the cache when first asked for.

## Events

Lines without an `id` are the service's own news, sent to every connected
window:

```json
{"event": "library_changed", "version": 2, "added": 5, "removed": 0}
```

The library index changed in a way a window draws (new, deleted, hidden,
favourited, edited or re-dated photos, or a video given a cover of its own
in place of a black or blank thumbnail: `cover.rs`); the thumbnails of new
photos are already on disk. A window reads the index again.

```json
{"event": "source", "kind": "folder", "plugin": "pinhole.folder", "id": "folder:/home/me/Pictures", "name": "Pictures", "needs_sign_in": false,
 "path": "/home/me/Pictures", "accepts": ["jpg", "jpeg", "heic", "...", "mov", "mp4"]}
```

Where the photos come from, sent to a window as it connects and when it
changes: `id` is the one its library says it holds the photos of (its
`meta.source`), `plugin` the plugin its kind is of (see Kinds), `name`
what to call it ("iCloud", or the folder's name as in `sources`),
`needs_sign_in` whether it is one that can need signing in, and an iCloud
source's `account` (its Apple ID) or a folder's `path`. Right
after it, as a window connects, comes `library_moved` with the library to
read: the one this service chose (never another source's, `library_for`),
whatever the window guessed. Both are sent before any switch or move made
after, which is then told in turn, never before them. A window given a
library of its own (`--library`) keeps to it, following it only where it
moves.
`accepts` is the extensions of the files `add` takes for it, lower case
(empty: nothing can be added). With no
source at all (none added yet, every one removed, or the plugins of those
added all off) `kind` and `plugin` are null and `name` empty, and the
library is an empty one in `library/none`.

```json
{"event": "sources", "current": "icloud:me@example.com", "sources": [
  {"id": "icloud:me@example.com", "kind": "icloud", "plugin": "pinhole.icloud", "name": "Jacek Becela", "account": "me@example.com",
   "count": 49024, "thumbs": 49022},
  {"id": "folder:/home/me/Pictures/iCloud", "kind": "folder", "plugin": "pinhole.folder", "name": "Pictures/iCloud",
   "path": "/home/me/Pictures/iCloud", "count": 654, "thumbs": 320}
]}
```

Every source added whose kind is on (see Kinds), in the order they were
added (the window numbers them 1 to 9 in it), and the one shown
(`current`, null when there is none):
sent to a window as it connects, and whenever the list, the one shown or a
library's counts change. `name` is what to call it: an Apple ID's owner as
Apple gives it (the Apple ID itself until known), a folder's name with the
folder it is in (`Pictures/iCloud`, `T7/DCIM`; alone when that is the home
folder or the root). `count` is the photos and videos in its library and
`thumbs` how many of them have their thumbnail.

```json
{"event": "progress", "source": "icloud:me@example.com", "phase": "index", "done": 32200, "total": 49024}
```

A library being built: `phase` is `index` while the source is listed
(`total` is the source's own count, 0 when it gives none or has not
counted yet: the first is always `0` of `0`) and `thumbs`
while thumbnails are made, at most four times a second, then `done` once.
Only for a whole listing or more than 100 thumbnails to make; a sync
round's handful says nothing. Meanwhile `library_changed` comes every four
seconds, and once the listing is done, so the grid fills as the index does.

Showing another source never waits for a library being built: the round
building it gives up within a second or so (between pages of the listing
and between thumbnails), says `done` for its source, and what it made
stays. When that source is shown again its first round lists again if the
listing was cut short and makes the thumbnails still missing. A window
takes a `progress` only for the source it shows. `set_source` and
`remove_source` are done one after another in the order they came, so of
several asked at once the last is the one shown.

```json
{"event": "rounds", "source": "icloud:me@example.com", "last": 1793026800000, "next": 1793026860000, "push": true}
```

When the source shown was last looked at for changes and when it will be
next (see Staying in sync), in milliseconds since 1970: sent to a window
as it connects, as each round starts (`next` null: one is under way) and
as it ends (`next` when the one after is due). `last` is when the last
round that worked ended, null before one has since the service started
(a failed round leaves it, and says why in `health`). With no source,
`source` is null. A round also starts early when a window sends `sync`,
another source is shown or the source says something changed, so `next`
is when it will be at the latest. `push` is whether the source tells of
its changes as they happen (also sent when that changes).
A round that panics (a bug in a source, or data it trips over) is a round
that failed: it is logged, the health is `failing`, the source is made
afresh from its settings entry, and the next round goes on as usual.

```json
{"event": "kinds", "kinds": [
  {"kind": "icloud", "plugin": "pinhole.icloud", "on": true, "needs_sign_in": true, "accepts": ["heic", "jpg", "..."]},
  {"kind": "folder", "plugin": "pinhole.folder", "on": false, "needs_sign_in": false, "accepts": ["jpg", "jpeg", "..."]}
]}
```

Every kind of source the service knows, the plugin whose switch it
follows, and whether it is on: sent to a window as it connects (after
`sources`) and whenever a plugin is turned on or off. See Kinds.

```json
{"event": "health", "state": "offline"}
```

How the last exchange with the source went, sent when it changes and to a
window as it connects (unless all is fine): `fine`, `needs_sign_in` (no
session, or Apple rejected it; `sign_in` again fixes it), `offline`
(the network, Apple's sign-in service or the folder's disk is out of reach)
or `failing` (the source answers with errors). A sync round, a lookup or a download that
works sets it back. The window shows anything but `fine` in a corner, and
two of its own: `no_service` when no service answers at all (whatever it
had asked is answered `no_service` then, and so is anything it asks until
one does), and `incompatible` when the service speaks another protocol
(whatever it asks is answered so too).

## Staying in sync

Every minute for iCloud and every ten seconds for a folder
(`PINHOLE_SYNC_SECS` sets another), and when a window asks, the service
asks the source what changed since the token the last round saved
(`sync::follow`). For iCloud that is CloudKit's change feed: changed assets
are written, an asset that changed alone gets its master looked up, and
assets deleted, hidden or erased are removed. Photos' own background work
changes many records in ways Pinhole does not draw; those are written but
announce nothing. For a folder it is a fresh look at the files. With no
token yet, or one the source no longer accepts, the whole library is
listed again, which is also how a new library is first made. A library
holds one source's photos and is never written by another
(`sync::claim`). Nothing but the service writes a library: the window,
and `pinhole <command>` from a terminal, ask it (`pinhole sync` sends
`sync` and follows the round to its end). Windows hear when each round
ran and the next will (`rounds`).

iCloud also tells the service of a change as it happens, as icloud.com
is told (`icloud::push`): a WebSocket to `websocket.push.apple.com` with
a P-256 key gives a push token and a JWT; the token is registered for an
hour at a time with PDS (`gatewayws.icloud.com/pds`, with the JWT and the
session's cookies) for `com.apple.icloud-container.com.apple.photos.cloud`,
and with ckdevice's `tokens/register`, with no subscription of its own
(the web cannot save one in the photos container, and CloudKit's
`tokens/create` is gone). A change to `PrimarySync` then comes down the
socket within seconds, saying only that the zone changed: a round starts
1.5 s after it (the few pushes of one change make one round). A lost
connection is tried again after 5 s, doubling to 5 minutes; the rounds
every minute go on regardless, in case a push is missed.

## Sources

The settings keep every source added, in order, as `sources`, and the one
shown as `source` (each `{"kind": "icloud", "account": apple-id}` or
`{"kind": "folder", "path": ...}`). Settings without `sources` have the
Apple IDs signed in to, then the folder shown if it is one. Each source has
its own library in the one folder: `library/icloud/<apple-id>/`,
`library/folder/home-me-Pictures/`.

### Kinds

Each kind of source is a plugin's: `icloud` is `pinhole.icloud`'s and
`folder` is `pinhole.folder`'s (`core/src/kinds.rs`), compiled into the
service from the plugins' crates (`plugins/icloud/source`,
`plugins/folder/source`; `core/daemon` puts them together), tied there and
never claimed by a manifest. A kind is on unless its plugin is turned
off in `~/.config/pinhole/plugins.json` (`"enabled": {"pinhole.icloud":
false}`), which the window writes and the service only reads: the window
sends `plugins_changed` after writing it, and each sync round looks at the
file again in case it was edited by hand. A file there that cannot be read
as a JSON object (caught half written, or broken by hand) changes no
switch: they stay as last read, and the log says so once. A kind that is off keeps
everything: its entries stay in the settings, its libraries and sessions
on disk. It is only not listed in `sources`, not shown, not synced and not
watched, and `set_source`, `remove_source`, `sign_out`, `sign_in` and
`submit_code` for it are answered `turned_off`. Which source is shown is
the one the settings name while its kind is on, else the first added whose
kind is on, else none; the settings are never written for this, so turning
a plugin on again shows what it showed before, at once and with no listing.
Windows hear `kinds`, then the switch (`source`, `library_moved`), then
`sources`.

A plugin may bring a kind of its own, run out of process by its provider
([source-providers.md](source-providers.md)): `kinds` lists it after the
built-in ones, with what its manifest says (`needs_sign_in`, `accepts`). It
follows its plugin's switch as the window does (a plugin of your own is on
only when turned on), and the plugins' folders are looked through again
each time the switches are read (`plugins_changed`, or the file changed).
Its sources are entries `{"kind", "key", …}`, with the id `<kind>:<key>`
and the fields `key` and `name` in `sources` and `source`; `set_source`
with its `kind` adds one through the provider's `add_entry`, and `sign_in`
and `submit_code` with its `kind` go to the provider, whose error codes the
window gets as they are. A plugin taken away leaves its entries in the
settings, as one turned off does.

`set_source` shows another source, adding it first if it is new. The
service also switches when the settings change under it, edited by
hand. Every window hears the new `source`, then
`library_moved` with the library to read, then `library_changed` as it
fills, and `sources`.

`remove_source` takes a source out of the list and deletes its library
(the index and thumbnails: never the photos themselves). An Apple ID is
signed out of too, its session deleted. Removing the one shown shows the
first left, or none.

`sign_out` deletes an Apple ID's session and nothing else: it stays among
the sources with its library, which shows what is on this computer (health
`needs_sign_in`) until it is signed in to again. Without a session the
provider says so without asking Apple.

`sign_in` signs in to an Apple ID through the sign-in provider (one
provider process to an Apple ID while it waits for the code, so
`submit_code` reaches the same sign-in). Without a `password` it takes
over the session icloudpd has for that Apple ID, as `pinhole sign-in`
does when icloudpd has one (`discover`). Signed in, the Apple ID is added and shown. Errors
carry the provider's codes: `wrong_password`, `try_again`, `wrong_code`,
`rate_limited`, `unavailable`, `needs_sign_in` (no icloudpd session for it).

The provider is `pinhole-auth` (`plugins/icloud/auth`, protocol in its
`PROTOCOL.md`): pinholed's `--auth`, else `$PINHOLE_AUTH`, else the
checkout's `plugins/icloud/auth/pyicloud/pinhole-auth` when pinholed runs
from `core/target/release`, else the one installed beside pinholed
(`bin/install-files` puts it in `<prefix>/bin`), else `pinhole-auth` on
`PATH`.

`discover` says what could be added, for the first window and the Add
button, from each kind that is on: under `kinds` by kind
(`{"icloud": {"icloudpd": […]}, "folder": {"folders": […]}}`, and a
provider's whatever it says), and the built-in kinds' fields at the top,
as windows before kinds read them: `icloudpd`, the Apple IDs icloudpd is signed in to as the names of
its session files (the Apple ID with everything but letters, digits and
`_` removed: `megmailcom`; the window matches a typed Apple ID against
them), and `folders`, each with `path`, `name` (as in `sources`),
`assets` (photos and videos, a Live Photo's two files counted once),
`live`, `bytes`, and `partial` when counting stopped before the end: every
`DCIM` folder on a disk under `/run/media/$USER`, the folders in Pictures,
then Pictures itself, those with any photos, not already added. Their
files are only looked at by name and size, and counting stops after two
seconds for all of them together. An Apple ID signed in to here already is
not in `icloudpd`.

## Adding

`add` puts photos and videos from this computer into the library of the
source shown, and is the one thing that changes a library: a folder gets
copies, iCloud gets uploads; the files given stay as they are. Folders
given are looked into (hidden files skipped), and a Live Photo's two
files (`IMG_1234.HEIC` with `IMG_1234.MOV` or `IMG_1234_HEVC.MOV`) make
one asset. Assets go one at a time, whichever window asked: a second
`add` waits for the first.

A folder takes what a look at it finds (JPEG, HEIC, PNG, GIF, TIFF,
WebP, AVIF, MOV, MP4, M4V, MKV, WebM, AVI) and copies it into the folder
itself, never over a file there: `IMG_1234 (2).HEIC` when the name is
taken, a Live Photo's two files under the same new name, each written
under a hidden name first so a round never sees half a file. iCloud takes
what icloud.com does (JPEG, PNG, HEIC, HEIF, TIFF, GIF, WebP, MOV, MP4,
M4V, up to 500 MB a file), uploaded as icloud.com uploads them
(api-findings.md, Uploading); a Live Photo goes as its still photo, since
the web cannot pair the motion with it. Each asset added starts a round,
so it is in the library (`library_changed`) as soon as the source has it:
for a folder at once, for iCloud some seconds later.

While it works, the window that asked hears, at most four times a second
and as each asset starts:

```json
{"event": "adding", "request": 7, "source": "icloud:me@example.com", "done": 1, "total": 3,
 "file": "/home/me/Desktop/IMG_1234.HEIC", "sent": 4200000, "bytes": 9800000}
```

`done` assets of `total`, `sent` bytes of all of them (`bytes`), and the
file under way; the last has `done` equal to `total` and `file` null.
The answer says how each went:

```json
{"id": 7, "ok": true, "result": {"source": "icloud:me@example.com", "added": 2, "duplicates": 0, "failed": 1, "files": [
  {"file": "/home/me/notes.txt", "files": ["/home/me/notes.txt"], "state": "failed",
   "error": {"code": "unsupported", "message": "not a photo or a video"}},
  {"file": "/home/me/IMG_1234.HEIC", "files": ["/home/me/IMG_1234.HEIC", "/home/me/IMG_1234.MOV"], "state": "added",
   "asset": "B7C1BB6F-...", "note": "IMG_1234.HEIC was added as a still photo: ..."},
  {"file": "/home/me/clip.mp4", "files": ["/home/me/clip.mp4"], "state": "added", "asset": "..."}]}}
```

`state` is `added`, `duplicate` (iCloud has it already: nothing was
added) or `failed`; `asset` is its id in the library when the source
knows it already. An error's `code`: `unsupported` (a kind of file the
source does not take), `too_large`, `over_quota` (iCloud storage is full),
`protected` (Advanced Data Protection keeps the web out of the library),
`needs_sign_in`, `unavailable` (the source or the network failed), or
`failed` (anything else, in the message). The request itself fails with
`unsupported` when nothing is shown, and `bad_request` without `files`.
`pinhole add <files>` does the same from a terminal.

## Favourites

`set_favorite` makes photos favourites, or not, in the source shown: for
iCloud `isFavorite` on each asset's record, by a `records/modify` with the
change tag the index has for it, as icloud.com does it (one changed since,
on the iPhone say, is looked up for its tag now and sent again); a folder
has no favourites of its own and keeps them in its library
(`folder_favorites`), where they stay as its files change. The index
follows at once, and every window hears `library_changed`, so the grid and
the viewer show it before the next round, which then agrees: iCloud's
change feed brings the same `isFavorite` back. Requests are done one after
another in the order they came, so a photo made a favourite and back again
quickly ends as it began. Those already as asked are left alone (`changed`
counts the rest). With `source`, they are done only if the library shown is
that source's (`""`, for a library that does not say whose it is, is
taken for no one's): else `other_source`, and nothing changes (a window's undo,
made after another source was shown from elsewhere, never reaches another
source's photos of the same ids).

An error leaves the index as it was: `not_found` for an asset the library
does not have, `unsupported` for a source that keeps no favourites,
`needs_sign_in` or `unavailable` as for any request to the source.
`bad_request` without `assets` or `favorite`. pinhole-fake's library, which
no source fills, keeps them itself.

## Streaming

A video or a Live Photo's motion asked for with `"stream": true` is answered
as soon as its download starts, with `stream`: an HTTP URL on the loopback
that plays it while it arrives (`path` is where it will be once complete).
The URL carries a random token and serves byte ranges, so a player can seek:
a range at most 4 MB past what has arrived waits for the download; one
further on (such as a QuickTime index at the end of the file) is fetched
from the source for that range alone. The download carries on into the cache
either way, and the URL keeps working for an hour after it was last read.
A cached file is answered with `path` alone.

## Errors

| Code | Meaning |
|---|---|
| `not_found` | no such asset, or it has no rendition of that kind |
| `needs_sign_in` | the sign-in provider has no valid session |
| `unavailable` | the source or the network failed; trying again later may work |
| `unreadable` | the file is there, but what makes a `medium` (or thumbnail) of it turned it down: damaged or cut short. The file's alone, the health stays as it is, and asking again answers the same at once until the file changes or six hours have passed |
| `bad_request` | unknown method or missing params |
| `read_only` | a change asked of the service of a library given to a window (`--given`) |
| `turned_off` | a source kind whose plugin is off (see Kinds) |

## Storage

Everything Pinhole keeps is in one folder, `~/.local/share/pinhole` unless
the window chose another: each source's library in `library/<kind>/<name>/`
(`library.sqlite` and `thumbs/`, built and kept by the service) and the photos fetched from the
source in `cache/<kind>/`, named by the rendition's key (iCloud's checksum
of the file), so a re-edited photo gets a new name and duplicates share
one.

One limit covers the whole folder. The library always stays, so the limit
is never under the library and 1 GB for photos; unless set it is the
library and 10 GB, and at least 20 GB. A hit refreshes a photo's
modification time; past what the limit leaves for photos, the least
recently used go, every two minutes, down to nine tenths of it. A file
still being written (`<name>.part`, or `<name>.part.<ext>` for one being
made: a JPEG, an effect) is never counted, moved or evicted. A
provider's original fetched only to make a thumbnail of
(docs/source-providers.md) waits in `cache/making/<pid>/` meanwhile,
outside the photos counted. A window's service of a library given
shares the cache, so each has a folder of its own there; any service
starting removes those of services no longer running.

The window's settings panel changes the folder and the limit with
`set_storage`; the service keeps them in `~/.config/pinhole/settings.json`
(`location`, `limit_bytes`) and reads them as it starts, as the window
does to guess the library before the service names it. `--library` and `--cache` override them. A lower
limit than is kept frees space at once, down to the limit itself. A new
folder has everything move over in the background: first the libraries,
with nothing writing them meanwhile (no round, favourite or lookup), renamed on one disk or copied across two, after which
every window is told

```json
{"event": "library_moved", "library": "/new/folder/library/icloud/me@example.com",
 "libraries": {"from": "/old/folder/library", "to": "/new/folder/library"}}
```

and reads it from there (a window keeping to another source's library, given
`--library`, finds it under `to` as it was under `from`) (the old copy goes 30 seconds later); then the
photos, new ones into the new folder at once and those kept each served
from the old folder until it has moved, keeping when it was last used. The
old folders go once empty.

The settings say the new folder only once the libraries are over, in one
write with the move's second half, made while still nothing writes them
(the commit); until then a move is kept in them as `moving` (from, to,
which half), and a commit that cannot be written undoes it. A move cut
short (the service stopped, the computer off) is put right as the service
starts again: committed, the photos go on moving; not, it is undone and
everything stays in the old folder (a copy already in the new one was
never written, and goes), unless the libraries were renamed over whole,
which is committed then. A folder inside the current
one, or the other way round, is refused, as is one that cannot be written
or already has a library. Photos in `~/.cache/pinhole`, where the cache
was before it moved in with the library, move in as the service starts.
`~/.cache/pinhole/compatible`, where the window made its own JPEGs
before it asked for `jpeg`, is removed then.

## Links

Download links expire within about half an hour. The service uses the one
in the index when it is under 20 minutes old and otherwise looks the asset
up again, gathering everything requested within 25 ms into one lookup of up
to 100 assets.
