# pinholed: the background service

The window never talks to iCloud. It reads the library index and the
thumbnails straight from disk, and asks `pinholed` for everything else: the
larger renditions of a photo, originals, videos. The service owns all
network access and all writes, so the window stays a fast reader.

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
| `fetch` | `asset` (the asset record name, the index's `id`), `kind` | the local file: `path`, `type` (a UTI), `rendition`, `cached` |
| `status` | | `library`, `cache`, `cache_bytes`, `in_flight` |
| `ping` | | `{}` |

`kind` picks the rendition, the first one the asset has:

| kind | renditions tried |
|---|---|
| `medium` | JPEGMed, JPEGFull, JPEGLarge |
| `original` | Original |
| `video` | VidMed, VidFull, Original |
| `live` | OriginalVidCompl (a Live Photo's motion) |

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
