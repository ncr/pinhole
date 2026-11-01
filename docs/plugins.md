# Plugins

What Pinhole does beyond showing photos comes as plugins, and anyone can
write one for themselves: a one-off that does exactly what you want with
your photos, and nobody else needs to see. Series (`plugins/series`) is
Pinhole's own and the fullest example.

A plugin is a folder with a `manifest.json` and a QML file, as Omarchy's
are. Pinhole's own are in its plugins folder (`/usr/lib/pinhole/plugins`
installed, `plugins/` in a checkout) and are on unless turned off; your own
go in `~/.config/pinhole/plugins/<id>/` and start off, to be turned on in
the side panel (Tab). One of yours with a built-in's id takes its place, so
copying a built-in there is how to change it.

## Writing one

```sh
pinhole plugin new me.favourites
```

makes `~/.config/pinhole/plugins/me.favourites` with a manifest and a
`Main.qml` that already does something. Turn it on in the side panel, and
change it: saving any of its files loads it again at once, and stops
whatever its old self had started. What goes wrong shows in red under it in
the side panel: QML that does not load, an error thrown, a warning from one
of its files, each with the file and the line.

```sh
pinhole plugin check me.favourites
```

looks it over as Pinhole would load it, without a window: the manifest,
every QML file it uses and the JavaScript they import, its translations.
It takes a folder too (`pinhole plugin check .`), and exits 1 if anything is
wrong, for an editor or a script to run on save.

## A whole plugin

`manifest.json`:

```json
{
  "schemaVersion": 1,
  "api": 2,
  "id": "me.favourites",
  "name": "Favourites count",
  "version": "1.0.0",
  "description": "How many favourites, on F2.",
  "entryPoints": { "main": "Main.qml" }
}
```

`api` is the oldest version of what follows the plugin needs (1 if left
out); a Pinhole older than that says it needs a newer one and does not load
it. `pinhole.api` says which one it is running in.

`Main.qml` beside it:

```qml
import QtQuick
import Pinhole.Plugin

PinholePlugin {
    id: plugin
    property int count: 0
    function recount() {
        let n = 0
        for (let i = 0; i < pinhole.library.count; ++i)
            if (pinhole.library.item(i).favorite)
                ++n
        count = n
    }
    Component.onCompleted: recount()
    Connections { target: pinhole.library; function onChanged() { plugin.recount() } }

    actions: [
        PluginAction {
            place: "grid"
            text: "♥ " + plugin.count
            key: "F2"
            onTriggered: pinhole.notify(qsTr("%1 favourites").arg(plugin.count))
        }
    ]
    help: [["F2", "Count favourites"]]
}
```

## What a plugin is given

The root is a `PinholePlugin`. It covers the window, above the photos and
the viewer and below Pinhole's own panels, and what it shows there is its
own. It sets:

- `actions`: `PluginAction`s, drawn as Pinhole's buttons. `place` is
  `"grid"` (top right, before Select), `"viewer"` (a photo full window,
  before Copy) or `"selection"` (select mode's bar, after Copy); then
  `text`, `key`, `shown`, `usable`, `active`, `busy`, and `onTriggered`. In
  the viewer and select mode, actions with the same `group` are one button,
  the first of them shown, with the rest in a menu under ▾ beside it, opened
  with ⇧ and that button's key. Pinhole sets its `hovered` while the pointer
  is on it, for the plugin to show what pressing it would do, as Series
  shows the series a photo would join.
- `covering`: true while it is over the photos: the grid's controls fade
  and its keys rest, as for Pinhole's panels. Its own `Shortcut`s and
  `Command`s should be enabled only then.
- `settings`: a `Component` shown under its row in the side panel while it
  is on, unfolded with a tap on the row (or ⏎ there), as its tour is.
- `help`: its keys for the help (?), as `[["key", "what it does"], ...]`.
- `stacks`: photos shown in the grid as one card, each a list of library
  positions, as Series does with each series. The card stands where the
  first one is, shows it and carries the mark for several photos; opening
  it opens that photo, selecting or dragging it takes them all, and the
  strip under a photo shown full window underlines a stack's photos. A
  photo in another plugin's stack already stays there.
- `hidden`: photos not in the grid at all, a list of library positions,
  as Screenshots hides screenshots. They leave any stack they are in, the
  viewer's ← and →, its strip and Home/End pass over them, and Ctrl+A and a
  range leave them out; a plugin can still `openPhoto` one. Keep it short
  of the whole library, and say somewhere how many are away and how to
  have them back.
- `tour`: a few steps that show what it does, on the photos and buttons as
  they are: `[{ title, text, photo, open, action, settings }, ...]`.
  `photo` is a library position (or a function giving one) the grid goes to
  and lights, `open` one shown full window, `action` one of its
  `PluginAction`s whose button is lit, and `settings: true` lights its part
  of the side panel. Pinhole shows it by itself the first time the plugin is
  on, once `tourReady` is true and the grid is free, and again from the
  side panel. Series' shows a series of your own.

`Pinhole.Plugin` also has Pinhole's own `ShellButton`, `Chip`, `UiText`,
`Spinner`, `Toggle` and `Command` (a key, with what it does in the help),
and `theme` has the Omarchy theme's colours and type, so a plugin can look
like the rest.

### `pinhole`

`pinhole` is the plugin's way into Pinhole. A photo is named by its
position in the library (oldest first, as the grid has them) or by its id;
positions move when the library is read again, ids do not, so keep ids.

| | |
|---|---|
| `library` | `count`; `item(photo)`: `index`, `id`, `date` (ms), `filename`, `thumbnail` (a URL), `width`, `height` (the thumbnail's), `video`, `live`, `favorite`, `avgColor` (the thumbnail's average colour, `"#rrggbb"`, or `""` before it has one), or `{}` if there is no such photo; `field(name)`: one of those for every photo at once, in the library's order (`field("date")`: all the dates), far quicker than `item` for each, or `field("day")`: the day each was taken in local time, as a number `yyyymmdd` (20250108), 0 for one with no date; `[]` for `index`, `thumbnail` or a name it has not; `indexOf(id)`, -1 if none; `dir` (the source's library: `library.sqlite`, to read, and its thumbnails); `key` (which source's library it is, wherever it is kept: `icloud:me@example.com`, `folder:/home/me/Pictures`; for what a plugin keeps for each source apart); and `changed()` when it is read again |
| `viewer` | the photo shown full window: `open`, `index` |
| `currentIndex()` | the photo under the pointer in the grid, else the middle one |
| `selection()`, `selectionCount` | the photos ticked in select mode, a card's all of them |
| `reveal(photo)`, `openPhoto(photo)` | the grid to a photo; a photo full window |
| `fetch(photo, kind)` | a photo's file from the service (`medium`, `full`, `original`, `video`, `live`): a promise of its path |
| `run(program, args, options)` | a program, its own (in its folder) or any on PATH: a promise of `{ stdout, stderr, code }`. `options`: `input` (to its stdin), `timeout` (ms), `env` (added to Pinhole's), `output(line)` (each line it writes, as it comes). `cancel()` asks it to end (SIGTERM) and kills it a second later |
| `record(item, options)` | an item as it looks over a number of frames, as a GIF or an MP4: a promise of the file's path. `options` are `path`, `format`, `width`, `height`, `fps`, `frames`, `seek(frame)` and `progress(fraction)` |
| `notify(text)` | a few words at the foot of the window for a while, as undo's: "Saved to Pictures" |
| `warn(error)` | something gone wrong, under the plugin in the side panel and on stderr |
| `settings` | its settings, kept in `~/.config/pinhole/plugins.json`: read one (`pinhole.settings.size`), bind to one, assign one and it is kept at once |
| `setting(key, fallback)`, `setSetting(key, value)` | the same, by name; `setSetting` says whether it could be kept. Series keeps its edits as `edits:` and the library's `key`, so each source's are its own |
| `did(name, undo, redo)` | a change just made, to undo with Ctrl+Z and do again with Ctrl+⇧Z: `name` says what in a word or two ("ungrouping"), `undo()` and `redo()` do it. Forgotten when the plugin is turned off or loaded again, and when another source's library is shown, however it came to be |
| `copyFile(path)`, `showFile(path)` | a file on the clipboard; its folder in the file manager |
| `picturesDir()`, `exists(path)`, `fileSize(path)`, `sizeText(bytes)` | |
| `dir`, `cacheDir` | its own folder; a folder for what it can make again, one to each source's library |
| `id`, `api` | its id; the version of all this |

### Promises

What takes a while (`fetch`, `run`, `record`) gives a promise, which also
has `cancel()`:

```js
const steadying = pinhole.run("pinhole-series", ["steady", dir].concat(ids), { timeout: 30000 })
steadying.then(result => use(result.stdout), error => {
    if (error.code !== "cancelled")
        throw error
}).catch(pinhole.warn)
// …another series shown in the meantime:
steadying.cancel()
```

A promise is rejected with an `Error` whose `code` says why:

| | |
|---|---|
| `fetch` | the service's: `not_found` (no such photo in this library), `offline`, … |
| `run` | `exit` (it ran and did not exit 0: `exitCode`, `stdout`, `stderr` too, and the message is what it wrote to stderr), `start` (it could not be started), `timeout`, `cancelled` |
| `record` | `failed` (ffmpeg, or the item gone), `cancelled` |

End a chain with `.catch(pinhole.warn)`: what a `then` throws is otherwise
said nowhere. There is no `finally` in Qt's JavaScript; a `.then()` after
the `.catch()` does the same. Nor is there `async`/`await` in QML.

Everything a plugin started ends with it: turned off or loaded again, its
programs are stopped, its recordings dropped, and no promise or callback of
its old self is answered.

Before API 2 these took a callback last in place of a promise
(`fetch(i, kind, done(path, error))`, `run(program, args, done(code,
stdout, stderr))`, `record(item, options, progress, done(path, error))`);
they still do.

### Translations

Strings in `qsTr()` are translated from `i18n/<language>.json` in the
plugin's folder, as Pinhole's own are (`app/src/i18n`).

### Trust

A plugin runs inside Pinhole with all of Pinhole's rights, as Omarchy's do:
only turn on one whose code you would run yourself.
