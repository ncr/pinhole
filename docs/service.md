# pinholed: the background service

The window never talks to iCloud. It reads the library index and the
thumbnails straight from disk, and asks `pinholed` for everything else: the
larger renditions of a photo, originals, videos. The service owns all
network access and all writes, so the window stays a fast reader. It also
follows iCloud's changes and tells the window when the library changed.

The window starts the service if none answers, passing the library it
shows (`--library`) and the socket (`$PINHOLE_SOCKET`, else
`$XDG_RUNTIME_DIR/pinhole.sock`).

## Transport

A Unix socket at `$XDG_RUNTIME_DIR/pinhole.sock`, one JSON object per line
each way. Requests carry an `id`; replies echo it and may arrive in any
order, since a cached file answers at once while a download takes a while.

```json
{"id": 7, "method": "fetch", "params": {"asset": "A5379A3C-...", "kind": "medium"}}
{"id": 7, "ok": true, "result": {"path": "/home/me/.cache/pinhole/medium/3f/AWx...jpg", "type": "public.jpeg", "rendition": "JPEGMed", "cached": false}}
{"id": 8, "ok": false, "error": {"code": "unavailable", "message": "..."}}
```

## Methods

| Method | Params | Result |
|---|---|---|
| `fetch` | `asset` (the asset record name, the index's `id`), `kind`, optionally `stream` | the local file: `path`, `type` (a UTI), `rendition`, `cached`; with `stream`, maybe a `stream` URL instead of waiting (see Streaming) |
| `status` | | `library`, `cache`, `cache_bytes`, `in_flight` |
| `sync` | | `{}`: look for changes in iCloud now rather than within the minute |
| `ping` | | `{}` |

`kind` picks the rendition, the first one the asset has:

| kind | renditions tried |
|---|---|
| `medium` | JPEGMed, JPEGFull, JPEGLarge |
| `original` | Original |
| `video` | VidMed, VidFull, Original |
| `live` | OriginalVidCompl (a Live Photo's motion) |

## Events

Lines without an `id` are the service's own news, sent to every connected
window:

```json
{"event": "library_changed", "version": 2, "added": 5, "removed": 0}
```

The library index changed in a way a window draws (new, deleted, hidden,
favourited, edited or re-dated photos); the thumbnails of new photos are
already on disk. A window reads the index again.

## Staying in sync

Every minute (`PINHOLE_SYNC_SECS`), and when a window asks, the service reads
CloudKit's change feed from the token the last walk or sync saved
(`sync::apply_changes`): changed assets are written, an asset that changed
alone gets its master looked up, and assets deleted, hidden or erased are
removed. Photos' own background work changes many records in ways Pinhole
does not draw; those are written but announce nothing. If iCloud no longer
accepts the token, the service walks the whole library again.
`pinhole-sync changes [--dry-run]` does the same from the command line.

## Streaming

A video or a Live Photo's motion asked for with `"stream": true` is answered
as soon as its download starts, with `stream`: an HTTP URL on the loopback
that plays it while it arrives (`path` is where it will be once complete).
The URL carries a random token and serves byte ranges, so a player can seek:
a range at most 4 MB past what has arrived waits for the download; one
further on (such as a QuickTime index at the end of the file) is fetched
from iCloud for that range alone. The download carries on into the cache
either way, and the URL keeps working for an hour after it was last read.
A cached file is answered with `path` alone.

## Errors

| Code | Meaning |
|---|---|
| `not_found` | no such asset, or it has no rendition of that kind |
| `needs_sign_in` | the sign-in provider has no valid session |
| `unavailable` | iCloud or the network failed; trying again later may work |
| `bad_request` | unknown method or missing params |

## Cache

Files land in `~/.cache/pinhole/<kind>/`, named by iCloud's checksum of the
file, so a re-edited photo gets a new name and duplicates share one. A hit
refreshes the file's modification time; past the size limit
(`PINHOLE_CACHE_GB`, 10 by default) the least recently used files go.

## Links

Download links expire within about half an hour. The service uses the one
in the index when it is under 20 minutes old and otherwise looks the asset
up again, gathering everything requested within 25 ms into one lookup of up
to 100 assets.
