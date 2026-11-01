# Sources of your own: providers

Pinhole shows photos from a *source*. iCloud and folders are built in
(their Rust is linked into pinholed). Any other source (Immich, a NAS, a
phone, a web gallery) is a plugin of your own with a *provider*: a program
in the plugin's folder, written in whatever you like, that pinholed starts
and talks to in JSON lines on its stdin and stdout.

The provider only says what there is. pinholed checks every line it sends,
writes the library itself (`library.sqlite` and the thumbnails), fetches
the files from where the provider says they are, makes the thumbnails a
provider has none of, and keeps everything in step. A provider never
writes the library, and no photo's bytes pass through it.

This page is all you need to write one. The test source,
`core/daemon/tests/provider/pinhole-test-source` (Python), is a complete
example: it signs in, follows changes, pushes them, serves thumbnails by
URL, takes files added and keeps favourites.

## The plugin

A provider is part of a plugin (`docs/plugins.md`): a folder in
`~/.config/pinhole/plugins/` with a `manifest.json`, a `Main.qml` (which
may be empty) and the program.

```json
{
  "schemaVersion": 1,
  "api": 3,
  "id": "me.immich",
  "name": "Immich",
  "entryPoints": { "main": "Main.qml" },
  "sources": [
    {
      "kind": "me.immich",
      "provider": "pinhole-immich",
      "needs_sign_in": true,
      "accepts": ["jpg", "jpeg", "heic", "mov"],
      "rounds_every_secs": 120
    }
  ]
}
```

```qml
import Pinhole.Plugin

PinholePlugin {
}
```

`sources` lists the kinds of source the plugin brings, each with:

| Field | | |
|---|---|---|
| `kind` | required | Its name in the settings. The plugin's `id`, or the id, a dot and more (`me.immich.shared`): a plugin can never take another's kind, nor `icloud` or `folder`. A kind is the plugin's with the longest id that names it: with plugins `me` and `me.jpegs` installed, `me.jpegs` and `me.jpegs.raw` are only `me.jpegs`'s. Letters, digits, `.`, `_` and `-`. |
| `provider` | required | The program, a path inside the plugin's folder (no `..`, not absolute), executable. |
| `needs_sign_in` | `false` | Its sources can need signing in: the window offers to. |
| `accepts` | `[]` | The extensions of files it takes when they are dropped on the window (`add`), in lower case. None: nothing can be added. |
| `rounds_every_secs` | 60 | How often a source of it is looked at for changes, 5 to 86400. |

The manifest needs `"api": 3`. A source that breaks a rule is left out,
and pinholed's log (`~/.local/state/pinhole/pinholed.log`) says why:

```
pinholed: a source refused: other.thief: source "me.immich": its kind must be the plugin's id, other.thief, or start with other.thief.
```

A plugin of your own is off until you turn it on in the side panel
(`"enabled": {"me.immich": true}` in `~/.config/pinhole/plugins.json`).
While it is off its provider never runs, its sources are not listed or
shown, and nothing of theirs is touched: their libraries and the
provider's own folder stay as they are, and come back at once, with no
listing, when it is on again. A plugin taken away altogether (its folder
moved) is the same: its sources stay in the settings for when it is back.

Turning it on or off in the side panel tells pinholed at once
(`plugins_changed`): its sources are listed and shown, or put away, as the
switch turns.

What the window shows of your sources is your plugin's QML, as iCloud's
and Folders' is theirs: a `PluginSource` in its root's `sources`
(docs/plugins.md) gives a source's row its words and buttons, offers what
your `discover` found and the ways in (a button and key of Add a source),
and signs in. Its `pinhole.sources` adds a source (`add(kind, entry)`,
which reaches your `add_entry`), signs in (`signIn`, `submitCode`) and out,
each only of the kinds your manifest names. With none, your sources are
still listed, by their names, with Remove.

Turning a plugin on is trusting it. It runs as you, as every plugin does;
these rules keep plugins from stepping on each other, not from you.

What it gives pinholed stays yours, not other users' of the computer. A
video's `url` that ffmpeg reads is served to it by pinholed on 127.0.0.1,
with your headers added there: at a path no one could guess, which is on
ffmpeg's command line, where other users can see it, so pinholed answers
only programs of yours (it asks the kernel whose the other end of each
connection is), a request head of at most 8 KB and eight at once. ffmpeg
reads it directly, never through a proxy `http_proxy` names.

## How it runs

pinholed starts it as

```
<plugin folder>/<provider> --state-dir ~/.local/state/pinhole/sources/<plugin id>
```

in that folder (mode 0700), which is the provider's own: keep sessions,
tokens and caches there. Nothing else of Pinhole's is its to write.

- One process serves each source in use (the one shown). It is started by
  the first request and stopped when the source is no longer shown, is
  removed, or its plugin is turned off.
- Signing in, `add_entry`, `discover`, `sign_out` and `removed` each get a
  process of their own, stopped once it has answered (a sign-in that waits
  for a code keeps its process for `submit_code`). So keep nothing only in
  memory that the next process needs: write it in the state folder.
- To stop one, pinholed closes its stdin; three seconds later it kills it.
  End when stdin closes.
- Its stderr goes to pinholed's log, each line after the plugin's id: at
  most 200 lines in ten seconds, 2,000 bytes a line.
- Its stdout is the protocol and nothing else. A print left in by mistake
  stops it (below).

## The protocol

One JSON object per line, UTF-8, ending in `\n`.

pinholed asks:

```json
{"id": 7, "method": "list", "params": {"entry": {"kind": "me.immich", "key": "me@nas", "url": "https://nas:2283"}}}
```

and the provider answers once, with the same `id`:

```json
{"id": 7, "ok": true, "result": {"token": "2026-11-01T10:00:00Z", "summary": "4,211 photos"}}
{"id": 7, "ok": false, "error": {"code": "needs_sign_in", "message": "the session expired"}}
```

Before its answer it may send *notes* for the request, with its `id`:

```json
{"id": 7, "note": "put", "assets": [ ... ]}
{"id": 7, "note": "progress", "done": 1000, "total": 4211}
```

A line with a `note` and no `id` is *news*, the provider's own:

```json
{"note": "changed"}
{"note": "listening", "on": true}
```

- Requests may overlap: pinholed can ask `prepare` while a `list` is under
  way. Answering one at a time, in order, is correct, only slower; a
  provider that can should answer each as it is ready, by its `id`.
- Unknown fields are ignored, in both directions, so either side can grow.
  An unknown note is ignored. A method you do not have: answer
  `unsupported`.
- Error codes are lower case, digits and `_`; messages are for people (the
  first 2,000 bytes are kept).

### Entries

A source is an *entry* in `~/.config/pinhole/settings.json`:

```json
{"kind": "me.immich", "key": "me@nas", "name": "Immich", "url": "https://nas:2283"}
```

- `kind` is yours, `key` tells your sources apart (1 to 256 bytes, no
  control characters), `name` is what the window calls it (the key when
  there is none). Add any fields of your own. The whole entry is at most
  64 KB.
- Its source id is `<kind>:<key>` (`me.immich:me@nas`) and its library
  `library/<kind>/<key>` in Pinhole's folder (the key made a safe folder
  name).
- The entry is sent with every request about the source as
  `params.entry`.
- It is not a secret: it is in a settings file. Keep passwords and tokens
  in your state folder, by key.

### Methods

| Method | Params | Answer | Notes before it |
|---|---|---|---|
| `add_entry` | what the window gives (a plugin's own form, `pinhole.sources.add`) | `{"entry"}` | |
| `sign_in` | what the window gives (`kind`, and say `password`) | `{"state": "signed_in", "entry"}` or `{"state": "needs_code"}` | |
| `submit_code` | `code`, and whatever else the window gives | as `sign_in` | |
| `sign_out` | `entry` | `{}` | |
| `removed` | `entry` | `{}`: forget what you kept for it | |
| `discover` | `added`: the entries of your kind added already | anything: the window gets it as `discover`'s `kinds[<kind>]` | |
| `start` | `entry` | `{}`, or an error: before each round | |
| `list` | `entry` | `{"token", "summary"}` | `put`, `progress` |
| `changes` | `entry`, `token` | `{"token", "summary"}`, or `token_expired` | `put`, `removed` |
| `prepare` | `entry`, `wanted` | `{"tickets"}` | |
| `watch` | `entry` | `{}`, then news | |
| `add` | `entry`, `file`, `motion`, `files` | `{"id", "duplicate", "note"}` | `sent` |
| `set_favorite` | `entry`, `ids`, `favorite` | `{}` | |

The least a provider needs is `add_entry` (or `sign_in`), `list` and
`prepare`. Answer the rest `unsupported`: `start` so is ready, `changes`
so lists again, `watch` so is looked at every round, `set_favorite` so
keeps no favourites, `add` so takes nothing.

**`add_entry`, `sign_in`, `submit_code`.** The window sends what its form
asked for. Give back the entry to add (its `kind` may be left out) and it
is added and shown. pinholed never logs these params. Codes: anything of
your own (`wrong_password`, `wrong_code`, `try_again`, `rate_limited`,
`needs_sign_in`) reaches the window as you say it. A `wrong_code` keeps
the sign-in's process for another try.

**`list`.** Every asset there is, in `put` notes. What the library had and
was not put is removed afterwards. `progress` says how far you are
(`done`, `total`), for the window's bar. The answer's `token` is where
`changes` picks up next time; `null`, and every round lists again (fine for
a few thousand photos). `summary` is a line for the log.

**`changes`.** What changed since `token`: `put` for what is new or
changed, `{"id": n, "note": "removed", "ids": [...]}` for what is gone.
Answer the next token. `token_expired` when you cannot tell from that
token: pinholed lists everything.

**`prepare`.** Where to read files: `wanted` is a list of
`{"asset", "kind", "name", "key"}` (a rendition as you put it), and
`tickets` is a list as long, in the same order, each one of

```json
{"path": "/mnt/nas/photos/2024/IMG_0001.HEIC"}
{"url": "https://nas:2283/api/assets/abc/original", "headers": {"x-api-key": "..."}}
{"error": {"code": "not_found", "message": "deleted on the NAS"}}
```

- A `path` is an absolute path to a file pinholed opens itself (byte
  ranges by seek, for a video that streams). Only a file: a FIFO or a
  device is `not_found`. A file that does not answer within 30 s (to be
  opened, or between two reads: a network mount gone) fails, and while
  four of your source's are still not back, its files are not tried.
- A `url` is `http` or `https`, fetched by pinholed with your `headers`
  (at most 32), byte ranges by `Range`. An answer of 401, 403, 404 or 410
  means the ticket is stale: pinholed asks `prepare` again, once. A server
  that sends nothing for 60 s (the answer, or the next bytes) fails the
  download; a slow one that keeps sending never does.
- `not_found` means the file is not there; any other code is the source's
  trouble, as for errors below.
- Up to 1,000 at once. Thumbnails are asked for in batches as the library
  fills; the window's photos one by one as they are opened.

**`watch`.** Answer at once, then send `{"note": "changed"}` whenever
something changes: a round follows within a second or two (several at once
make one). `{"note": "listening", "on": true}` tells the window changes come
as they happen; `false` when that stops. Rounds every
`rounds_every_secs` go on regardless. A provider started again after a
crash is asked to watch again; one asked twice watches once.

**`add`.** A file dropped on the window: `file`, an absolute path, and a
Live Photo's `motion` beside it (or `null`); `files` is both. Copy or
upload it; the files stay as they are. Say `{"id": n, "note": "sent",
"sent": bytes, "bytes": total}` as it goes. Answer the asset's `id` if you
know it, `duplicate: true` if you had it, `note` for what you left out.
Codes: `unsupported`, `too_large`, `over_quota`, `protected`, `failed`. A
round follows each one added.

**`set_favorite`.** Make `ids` favourites (or not). pinholed changes the
library once you have answered.

### Assets

A `put` note's `assets`, at most 1,000 in one note:

```json
{
  "id": "2024/IMG_0001.HEIC",
  "kind": "live",
  "filename": "IMG_0001.HEIC",
  "item_type": "public.heic",
  "taken": 1704103200000,
  "added": 1704103300000,
  "favorite": false,
  "duration": null,
  "orientation": 1,
  "has_location": true,
  "size": 2412331,
  "effect": "loop",
  "renditions": [
    {"kind": "thumb", "name": "2024/IMG_0001.HEIC", "key": "a1b2c3", "make": true},
    {"kind": "medium", "name": "2024/IMG_0001.HEIC", "key": "a1b2c3", "make": true},
    {"kind": "full", "name": "2024/IMG_0001.HEIC", "key": "a1b2c3", "width": 4032, "height": 3024, "size": 2412331, "file_type": "public.heic"},
    {"kind": "original", "name": "2024/IMG_0001.HEIC", "key": "a1b2c3", "size": 2412331, "file_type": "public.heic"},
    {"kind": "live", "name": "2024/IMG_0001.MOV", "key": "d4e5f6", "file_type": "com.apple.quicktime-movie"}
  ]
}
```

| Field | | |
|---|---|---|
| `id` | required | Unique in the source and stable while the asset is there. 1 to 512 bytes, no control characters. |
| `kind` | required | `photo`, `video` or `live` (a photo with motion). |
| `filename`, `item_type` | | The original's name and UTI (`public.jpeg`, `public.heic`, `com.apple.quicktime-movie`…). |
| `taken`, `added` | | Milliseconds since 1970. The grid is ordered by `taken`. |
| `favorite`, `has_location` | `false` | |
| `duration` | | A video's, in seconds. |
| `orientation` | | EXIF's, 1 to 8. |
| `size` | | The original's, in bytes. |
| `effect` | | A Live Photo's, as set on the iPhone: `loop`, `bounce` or `long_exposure`. |
| `renditions` | | At most 16, one of each kind. |

A rendition is one file of the asset, for a kind the window asks for:

| Kind | |
|---|---|
| `thumb` | About 480 px on the long edge. Kept in the library: the grid draws it. A JPEG. |
| `medium` | About 2048 px, for the viewer and big cells. |
| `full` | The photo as it looks now, edits and all. |
| `original` | As taken. |
| `video` | A video to play. |
| `live` | A Live Photo's motion. |
| `loop`, `bounce`, `long_exposure` | A Live Photo's effect, if you have it rendered; pinholed makes them from `live` otherwise. |

- `name` (required, up to 1,024 bytes) is yours: it comes back in
  `prepare`. `key` (required) names the file in the cache and must change
  when the file does: a checksum, or a hash of its path, size and time.
  1 to 256 bytes of `A-Z a-z 0-9 . _ ~ + / = -`. Keys need only be unique
  in your source: pinholed keeps them apart from other sources'.
- `width`, `height`, `size` and `file_type` (a UTI) are said if known.
- `"make": true` on a `thumb` or `medium`: you have none, and pinholed
  makes it from the `original` (which the asset must have), with
  ImageMagick, or ffmpeg from a video's most telling frame, as it does a
  folder's. It asks `prepare` for the original's ticket then. A `path`
  ticket is quickest. A photo's `url` is fetched whole first, into the
  cache, and only up to 512 MB; a video's is read by ffmpeg, by ranges,
  only what it needs (through pinholed, so your headers are never on a
  command line). An answer of 401, 403, 404 or 410 to either is a stale
  ticket, as for any `url`: `prepare` is asked again, once. ImageMagick
  or ffmpeg that has done nothing for two minutes (a mount that no longer
  answers; not one waiting for a processor on a busy computer) is stopped,
  and at most two videos are read from URLs at once, beside, not instead
  of, a folder's thumbnails.
- A photo with no `thumb` shows as its average colour, a blank.

pinholed checks every asset before it writes it. One that is not right
(no id, a kind it does not know, a bad key, two renditions of a kind, a
field of the wrong type) is left out and logged, and the rest of the note
is written. What the library had of an asset you now say wrong stays as it
was: a listing does not take it away.

### Errors, and what the window says

The code of an error answer says how the source is doing:

| Code | Health | The window |
|---|---|---|
| `needs_sign_in` | `needs_sign_in` | "Not signed in", and a way to sign in again |
| `offline` | `offline` | its disk or network is not there |
| `token_expired` (from `changes`) | | none: pinholed lists everything |
| `unsupported` | | as above, by method |
| anything else | `failing` | something is wrong |

Fetches that fail say `unavailable` to the window (`needs_sign_in` for
that), a ticket's `not_found` says `not_found`. A thumbnail or medium
pinholed makes (`"make": true`) from a file that ImageMagick or ffmpeg
turns down (damaged, cut short) says `unreadable`, and leaves the health
as it is: that is the file's, not your source's, and it is not made again
until its key changes.

## Limits

What pinholed does when a provider misbehaves, so that it never harms the
library or pinholed:

| | |
|---|---|
| Silent for two minutes while asked something (since it was asked, or since its last note, answer or news; lines for no request under way do not count) | killed; what it was asked fails `unavailable` |
| A line on stdout that is not a JSON object with an `id` or a `note`, or an answer with neither `"ok": true` nor `false` | stopped, the same |
| A line of more than 4 MB | stopped, the same |
| It ends, or crashes | the same |
| More than 1,000 assets in one `put`, or ids in one `removed` | that request fails |
| An asset that is not right | left out and logged |
| More than 200 lines of stderr in ten seconds, or a line of more than 2,000 bytes | the rest left out of the log (the rest of a long line passed over as it comes, never kept) |
| Lines for a request that is over, or that was never asked | ignored |

After it stops, the next request starts it again, at most once in ten
seconds: a provider that crashes as it starts runs once in ten seconds,
not in a loop, and the window shows the source failing meanwhile. (One
pinholed stopped itself, its plugin turned off or its source not shown,
starts again at once.) A
request that fails in a listing writes nothing more: what was put before
stays, and nothing is removed.

## How fast

50,000 assets of three renditions each, put by the test source
(`fake`), checked and written by pinholed: **0.6 s**, about 85,000 a
second, of which the Python itself is 0.33 s (27 MB of JSON). 200,000
took 1.8 s. Listing iCloud's 49,079 photos from Apple takes about five
minutes, so the protocol costs nothing that shows.
(`fifty_thousand_assets_through_the_protocol` in
`core/daemon/tests/provider.rs`; `PINHOLE_PROVIDER_ASSETS` for another
number.)

## Examples

### Python: a folder of JPEGs

The smallest useful provider. No token: each round lists the folder again.

```python
#!/usr/bin/env -S python3 -I
"""A folder of JPEGs as a source of Pinhole's."""
import hashlib, json, os, sys


def say(line):
    sys.stdout.write(json.dumps(line) + "\n")
    sys.stdout.flush()


def assets(folder):
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith((".jpg", ".jpeg")):
            continue
        st = os.stat(os.path.join(folder, name))
        key = hashlib.sha1(f"{name}\0{st.st_size}\0{st.st_mtime_ns}".encode()).hexdigest()
        yield {"id": name, "kind": "photo", "filename": name, "taken": st.st_mtime_ns // 1_000_000,
               "renditions": [{"kind": k, "name": name, "key": key, "make": True} for k in ("thumb", "medium")]
               + [{"kind": k, "name": name, "key": key, "file_type": "public.jpeg"} for k in ("full", "original")]}


for line in sys.stdin:
    request = json.loads(line)
    rid, method, params = request["id"], request["method"], request.get("params") or {}
    entry = params.get("entry") or {}
    try:
        if method == "add_entry":
            folder = os.path.abspath(params.get("folder", ""))
            if not os.path.isdir(folder):
                raise LookupError("bad_request", f"{folder} is not a folder")
            result = {"entry": {"key": folder, "name": os.path.basename(folder), "folder": folder}}
        elif method == "start":
            if not os.path.isdir(entry["folder"]):
                raise LookupError("offline", f"{entry['folder']} is not there")
            result = {}
        elif method == "list":
            found = list(assets(entry["folder"]))
            for i in range(0, len(found), 500):
                say({"id": rid, "note": "put", "assets": found[i:i + 500]})
            result = {"token": None, "summary": f"{len(found)} photos"}
        elif method == "prepare":
            result = {"tickets": [{"path": os.path.join(entry["folder"], w["name"])} for w in params["wanted"]]}
        else:
            raise LookupError("unsupported", f"no {method}")
        say({"id": rid, "ok": True, "result": result})
    except LookupError as e:
        say({"id": rid, "ok": False, "error": {"code": e.args[0], "message": e.args[1]}})
```

A window adds one with `pinhole.sources.add("me.jpegs", {folder: "/mnt/photos"})`,
or `set_source` with `{"kind": "me.jpegs", "folder": "/mnt/photos"}` on
pinholed's socket.

### Shell: the same with jq

```sh
#!/bin/sh
# A folder of JPEGs as a source of Pinhole's; jq reads and writes the JSON.
while IFS= read -r line; do
  id=$(printf '%s' "$line" | jq '.id')
  case $(printf '%s' "$line" | jq -r '.method') in
    add_entry)
      printf '%s' "$line" | jq -c '{id, ok: true, result: {entry: {key: .params.folder, folder: .params.folder}}}' ;;
    list)
      folder=$(printf '%s' "$line" | jq -r '.params.entry.folder')
      find "$folder" -maxdepth 1 -type f -iname '*.jpg' -printf '%f\t%s\t%T@\n' | jq -Rsc --argjson id "$id" '
        [split("\n")[] | select(length > 0) | split("\t") as [$name, $size, $time]
         | {id: $name, kind: "photo", taken: ($time | tonumber * 1000 | floor),
            renditions: [({kind: "thumb", make: true}, {kind: "medium", make: true}, {kind: "full"}, {kind: "original"})
                         + {name: $name, key: "\($size)-\($time)"}]}]
        | range(0; length; 1000) as $i | {id: $id, note: "put", assets: .[$i:$i + 1000]}'
      printf '{"id":%s,"ok":true,"result":{"token":null,"summary":"listed"}}\n' "$id" ;;
    prepare)
      printf '%s' "$line" | jq -c '.params.entry.folder as $f | {id, ok: true, result: {tickets: [.params.wanted[] | {path: "\($f)/\(.name)"}]}}' ;;
    *)
      printf '{"id":%s,"ok":false,"error":{"code":"unsupported","message":"no such method"}}\n' "$id" ;;
  esac
done
```

### Rust

A loop over `std::io::stdin().lines()`, `serde_json::from_str::<Value>` of
each, and `println!` of `json!({"id": id, "ok": true, "result": …})` (the
standard output is flushed at each newline only when it is a terminal: lock
it and `flush()` after each line). Notes the same way, before the answer.
A thread for each request that may take long (a `list`) lets `prepare`
be answered meanwhile.

## Trying one

By hand, a request on its stdin:

```sh
echo '{"id":1,"method":"list","params":{"entry":{"kind":"me.jpegs","key":"/mnt/photos","folder":"/mnt/photos"}}}' \
  | ./pinhole-jpegs --state-dir /tmp/state
```

In Pinhole: put the plugin in `~/.config/pinhole/plugins/`, turn it on in
the side panel, and watch pinholed's log
(`~/.local/state/pinhole/pinholed.log`): it says when the provider starts,
what it refused and why, and everything the provider writes on stderr.
`pinhole status` shows the source and its health.

The tests in `core/daemon/tests/provider.rs` run a pinholed of their own
(its socket, its folders) with the test source, and go through all of the
above: listing 300 photos with thumbnails by `url` and `path` tickets,
mediums made, originals fetched, changes pushed and followed, files added,
favourites, sign-in and its codes, a second source added and removed, the
provider hanging, crashing, flooding and lying (a 200 MB line of stderr
among it), a kind outside its plugin's name refused, the plugin turned off
and on, a video's thumbnail made from its URL by ranges, a medium made from
a URL in the cache, and FIFOs for originals.
