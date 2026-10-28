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
copying a built-in there is how to change it. Saving any of a plugin's
files loads it again at once.

## A whole plugin

`~/.config/pinhole/plugins/me.favourites/manifest.json`:

```json
{
  "schemaVersion": 1,
  "id": "me.favourites",
  "name": "Favourites count",
  "version": "1.0.0",
  "description": "How many favourites, on F2.",
  "entryPoints": { "main": "Main.qml" }
}
```

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
        PluginAction { place: "grid"; text: "♥ " + plugin.count; key: "F2"; onTriggered: plugin.recount() }
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
  before Copy) or `"selection"` (select mode's bar, after Export); then `text`, `key`, `shown`, `usable`, `active`, `busy`, and
  `onTriggered`.
- `covering`: true while it is over the photos: the grid's controls fade
  and its keys rest, as for Pinhole's panels. Its own `Shortcut`s should be
  enabled only then.
- `settings`: a `Component` shown under its switch in the side panel.
- `help`: its keys for the help (?), as `[["key", "what it does"], ...]`.
- `stacks`: photos shown in the grid as one card, each a list of library
  positions, as Series does with each series. The card stands where the
  first one is, shows it and carries the mark for several photos; opening
  it opens that photo, selecting or dragging it takes them all, and the
  strip under a photo shown full window underlines a stack's photos. A
  photo in another plugin's stack already stays there.

`Pinhole.Plugin` also has Pinhole's own `ShellButton`, `Chip`, `UiText`,
`Spinner` and `Toggle`, and `theme` has the Omarchy theme's colours and
type, so a plugin can look like the rest.

`pinhole` is the plugin's way into Pinhole:

| | |
|---|---|
| `library` | `count`, `item(i)` (id, date in ms, filename, thumbnail URL, width, height, video, live, favorite), `indexOf(id)`, `dir` (the source's library: `library.sqlite`, to read, and its thumbnails), and `changed()` when it is read again |
| `viewer` | the photo shown full window: `open`, `index` |
| `currentIndex()` | the photo under the pointer in the grid, else the middle one |
| `selection()`, `selectionCount` | the photos ticked in select mode, a card's all of them |
| `reveal(i)`, `openPhoto(i)` | the grid to a photo; a photo full window |
| `fetch(i, kind, done)` | a photo's file from the service (`medium`, `full`, `original`, `video`, `live`): `done(path, error)` |
| `run(program, args, done)` | a program, its own (in its folder) or any on PATH: `done(exitCode, stdout, stderr)` |
| `record(item, options, progress, done)` | an item as it looks over a number of frames, as a GIF or an MP4: `options` are `path`, `format`, `width`, `height`, `fps`, `frames` and `seek(frame)` |
| `copyFile(path)`, `showFile(path)` | a file on the clipboard; its folder in the file manager |
| `picturesDir()`, `exists(path)`, `fileSize(path)`, `sizeText(bytes)` | |
| `dir`, `cacheDir` | its own folder; a folder for what it can make again, one to each source's library |
| `setting(key, fallback)`, `setSetting(key, value)` | its settings, kept in `~/.config/pinhole/plugins.json` |

Strings in `qsTr()` are translated from `i18n/<language>.json` in the
plugin's folder, as Pinhole's own are (`app/src/i18n`).

A plugin runs inside Pinhole with all of Pinhole's rights, as Omarchy's do:
only turn on one whose code you would run yourself.
