# pinholed: the background service

The window never talks to the source of the photos (iCloud, or a folder;
see [sources.md](sources.md)). It reads the library index and the
thumbnails straight from disk, and asks `pinholed` for everything else: the
larger renditions of a photo, originals, videos. The service owns all
network access and all writes, so the window stays a fast reader. It also
follows the source's changes and tells the window when the library changed.

The window starts the service if none answers, passing the library it
shows (`--library`) and the socket (`$PINHOLE_SOCKET`, else
`$XDG_RUNTIME_DIR/pinhole.sock`).

## Transport

A Unix socket at `$XDG_RUNTIME_DIR/pinhole.sock`, one JSON object per line
each way. Requests carry an `id`; replies echo it and may arrive in any
order, since a cached file answers at once while a download takes a while.

```json
{"id": 7, "method": "fetch", "params": {"asset": "A5379A3C-...", "kind": "medium"}}
{"id": 7, "ok": true, "result": {"path": "/home/me/.local/share/pinhole/cache/medium/3f/AWx...jpg", "type": "public.jpeg", "rendition": "JPEGMed", "cached": false}}
{"id": 8, "ok": false, "error": {"code": "unavailable", "message": "..."}}
```

## Methods

| Method | Params | Result |
|---|---|---|
| `fetch` | `asset` (the asset record name, the index's `id`), `kind`, optionally `stream` | the local file: `path`, `type` (a UTI), `rendition`, `cached`; with `stream`, maybe a `stream` URL instead of waiting (see Streaming) |
| `status` | | `library`, `cache`, `cache_bytes`, `in_flight`, `health`, `source` (as the `source` event), `progress` (as the `progress` event, or null), `rounds` (as the `rounds` event) |
| `sync` | | `{}`: look for changes in the source now rather than at the next round |
| `sources` | | every source added, as the `sources` event |
| `set_source` | an `id` from `sources`, or `kind` (`icloud` or `folder`) and an iCloud `account` (Apple ID) or a folder's `path` | the source, as the `source` event: added to the sources if new, and shown; see Sources |
| `remove_source` | `id` | the `sources` left; see Sources |
| `sign_out` | an Apple ID's `id` | the `sources`, that one still among them; see Sources |
| `discover` | | what could be added: `icloudpd` and `folders`; see Sources |
| `sign_in` | `apple_id`, and `password` unless icloudpd is signed in to it | `state`: `signed_in` (added and shown) or `needs_code` |
| `submit_code` | `apple_id`, `code` (six digits) | `state`: `signed_in` (added and shown) |
| `storage` | | where and how much: `location` (the one folder), `library`, `cache`, `limit_bytes`, `min_limit_bytes`, `default_limit_bytes`, `limit_set`, `used_bytes` (all of it), `kinds` (bytes of `library` and of each kind of photo), `freeing`, `moving` (`done_bytes`, `total_bytes`, or null) |
| `set_storage` | `limit_bytes` and/or `location` (an absolute folder) | as `storage`, after the change (see Storage) |
| `ping` | | `{}` |

`kind` picks the rendition: `medium` (about 2048 px), `full` (the photo as
it looks now: the edit, if it has one), `original` (as taken, unedited),
`video`, `live` (a Live Photo's motion). Which of the source's files serves
each was chosen as the library was indexed (the index's `renditions`);
`rendition` is the source's name for it. For iCloud:

| kind | renditions tried |
|---|---|
| `medium` | JPEGMed, JPEGFull, JPEGLarge |
| `full` | JPEGFull, VidFull, Original |
| `original` | Original |
| `video` | VidMed, VidFull, Original (videos only) |
| `live` | OriginalVidCompl |

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
favourited, edited or re-dated photos); the thumbnails of new photos are
already on disk. A window reads the index again.

```json
{"event": "source", "kind": "folder", "name": "Pictures", "needs_sign_in": false, "path": "/home/me/Pictures"}
```

Where the photos come from, sent to a window as it connects and when it
changes: `name` is what to call it ("iCloud", or the folder's name as in
`sources`), `needs_sign_in` whether it is one that can need signing in, and
an iCloud source's `account` (its Apple ID) or a folder's `path`. With no
source at all (none added yet, or every one removed) `kind` is null and
`name` empty, and the library is an empty one in `library/none`.

```json
{"event": "sources", "current": "icloud:me@example.com", "sources": [
  {"id": "icloud:me@example.com", "kind": "icloud", "name": "Jacek Becela", "account": "me@example.com", "count": 49024, "thumbs": 49022},
  {"id": "folder:/home/me/Pictures/iCloud", "kind": "folder", "name": "Pictures/iCloud", "path": "/home/me/Pictures/iCloud", "count": 654, "thumbs": 320}
]}
```

Every source added, in the order they were added (the window numbers them
1 to 9 in it), and the one shown (`current`, null when there is none):
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

```json
{"event": "health", "state": "offline"}
```

How the last exchange with the source went, sent when it changes and to a
window as it connects (unless all is fine): `fine`, `needs_sign_in` (no
session, or Apple rejected it; `sign_in` again fixes it), `offline`
(the network, Apple's sign-in service or the folder's disk is out of reach)
or `failing` (the source answers with errors). A sync round, a lookup or a download that
works sets it back. The window shows anything but `fine` in a corner, and
`no_service` of its own when no service answers at all.

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
(`sync::claim`). `pinhole-sync changes [--dry-run]` does the same from the
command line. Windows hear when each round ran and the next will
(`rounds`).

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

`set_source` shows another source, adding it first if it is new. The
service also switches when the settings change under it, as
`pinhole-sync sign-in` does. Every window hears the new `source`, then
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
over the session icloudpd has for that Apple ID, as `pinhole-sync sign-in
--from-icloudpd` does. Signed in, the Apple ID is added and shown. Errors
carry the provider's codes: `wrong_password`, `try_again`, `wrong_code`,
`rate_limited`, `unavailable`, `needs_sign_in` (no icloudpd session for it).

`discover` says what could be added, for the first window and the Add
button: `icloudpd`, the Apple IDs icloudpd is signed in to as the names of
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
| `bad_request` | unknown method or missing params |

## Storage

Everything Pinhole keeps is in one folder, `~/.local/share/pinhole` unless
the window chose another: each source's library in `library/<kind>/<name>/`
(`library.sqlite` and `thumbs/`, built by `pinhole-sync` or the service and
kept by the service) and the photos fetched from the
source in `cache/<kind>/`, named by the rendition's key (iCloud's checksum
of the file), so a re-edited photo gets a new name and duplicates share
one.

One limit covers the whole folder. The library always stays, so the limit
is never under the library and 1 GB for photos; unless set it is the
library and 10 GB, and at least 20 GB. A hit refreshes a photo's
modification time; past what the limit leaves for photos, the least
recently used go, every two minutes, down to nine tenths of it.

The window's settings panel changes the folder and the limit with
`set_storage`; the service keeps them in `~/.config/pinhole/settings.json`
(`location`, `limit_bytes`) and reads them as it starts, as `pinhole-sync`
does to find the library. `--library` and `--cache` override them. A lower
limit than is kept frees space at once, down to the limit itself. A new
folder takes new photos at once and everything moves over in the
background: first the library, with syncing paused, renamed on one disk or
copied across two, after which every window is told

```json
{"event": "library_moved", "library": "/new/folder/library/icloud/me@example.com"}
```

and reads it from there (the old copy goes 30 seconds later); then the
photos, each served from the old folder until it has moved, keeping when it
was last used. The old folders go once empty. A folder inside the current
one, or the other way round, is refused, as is one that cannot be written
or already has a library. Photos in `~/.cache/pinhole`, where the cache
was before it moved in with the library, move in as the service starts.

## Links

Download links expire within about half an hour. The service uses the one
in the index when it is under 20 minutes old and otherwise looks the asset
up again, gathering everything requested within 25 ms into one lookup of up
to 100 assets.
