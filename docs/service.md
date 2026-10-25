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
| `status` | | `library`, `cache`, `cache_bytes`, `in_flight`, `health`, `source` (as the `source` event) |
| `sync` | | `{}`: look for changes in the source now rather than at the next round |
| `set_source` | `kind` (`icloud` or `folder`), and a folder's `path` | the source, as the `source` event; see Sources |
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
changes: `name` is what to call it ("iCloud", or the folder's name), and
`needs_sign_in` whether it is one that can need signing in.

```json
{"event": "health", "state": "offline"}
```

How the last exchange with the source went, sent when it changes and to a
window as it connects (unless all is fine): `fine`, `needs_sign_in` (no
session, or Apple rejected it; `pinhole-sync sign-in` fixes it), `offline`
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
command line.

## Sources

`set_source` changes where the photos come from, kept in the settings as
`source` (`{"kind": "icloud"}` or `{"kind": "folder", "path": ...}`; none is
iCloud). Each source has its own library in the one folder: iCloud's in
`library/`, a folder's in `library-folder/`, made again from scratch when
the folder changes. Every window hears the new `source`, then
`library_moved` with the library to read, then `library_changed` as it
fills.

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
the window chose another: the library in `library/` (`library.sqlite` and
`thumbs/`, built by `pinhole-sync` or the service and kept by the service;
another source's in `library-<kind>/`) and the photos fetched from the
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
{"event": "library_moved", "library": "/new/folder/library"}
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
