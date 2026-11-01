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
wrong, for an editor or a script to run on save. It warns of anything the
plugin uses of an API newer than its manifest asks for, and of a file named
as one of another plugin's or of Pinhole's own (see Translations).

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
it. `pinhole.api` says which one it is running in. API 2 brought promises
(`copyFile`'s too), `pinhole.settings`, `notify`, `warn`, photos by id,
`hidden`, `avgColor`, `library.field`, `isHidden`, `run`'s `output`,
`pinhole.coveredBy`, `dismissed` and `keepAwake`. API 3 brought `marks`,
and `sources` in the manifest: a source of photos of the plugin's own,
whose provider pinholed runs ([source-providers.md](source-providers.md)).

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
own. It loads once the library has been read, before the window's first
frame: what it sets in `Component.onCompleted` from the library, or from
its settings, is there as the window first shows. It sets:

- `actions`: `PluginAction`s, drawn as Pinhole's buttons. `place` is
  `"grid"` (top right, before Select), `"viewer"` (a photo full window,
  before Copy) or `"selection"` (select mode's bar, after Copy); then
  `text`, `key`, `shown`, `usable`, `active`, `busy`, and `onTriggered`.
  `key` is a sequence as Qt writes one ("F2", "Shift+S", "Ctrl+Return"),
  and the button and the help show it as Pinhole names keys: F2, ⇧ S,
  Ctrl ⏎. In
  the viewer and select mode, actions with the same `group` are one button,
  the first of them shown, with the rest in a menu under ▾ beside it, opened
  with ⇧ and that button's key. Pinhole sets its `hovered` while the pointer
  is on it, for the plugin to show what pressing it would do, as Series
  shows the series a photo would join. Where the window is too narrow for
  them all, their keys go from the buttons, then the plugins' buttons fold
  into one, More ▾, whose menu lists them with their keys; the keys work
  either way, and a group's ⇧ key opens that menu on its row. More ▾ is
  lit while one of them is `active`, and turns while one is `busy`.
- `covering`: true while it is over the photos: the grid's controls fade
  and its keys rest, as for Pinhole's panels. Its own `Shortcut`s and
  `Command`s should be enabled only then. One plugin covers at a time, the
  last to start: it is drawn over the other plugins, which are hidden
  meanwhile (an overlay as Info's too), and the `"plugin"` key layer is its
  alone. `pinhole.coveredBy` says which one it is (its id, `""` for none):
  a plugin that comes back by itself, as On this day does when a photo
  opened from it closes, stays away while another's covers, and forgets
  coming back once another has covered meanwhile. When the one over the
  photos hands one over (`openPhoto`, `reveal`), those covering beneath
  it are told with `pinhole.dismissed()`, and close for good: Colours
  opened over On this day, ⏎ opens a photo, and Esc then comes back to
  Colours alone, then to the grid.
- `settings`: a `Component` shown under its row in the side panel while it
  is on, unfolded with a tap on the row (or ⏎ there), as its tour is.
- `help`: its keys for the help (?), as `[["key", "what it does"], ...]`
  (a key as a sequence, "Shift+S", is shown as Pinhole names it: ⇧ S),
  listed under its name with its `Command`s that give a `section` (any
  but Pinhole's own parts: `"photo"`, `"share"`…). A few words each, as
  Pinhole's are: the heading says whose they are.
- `stacks`: photos shown in the grid as one card, each a list of library
  positions, as Series does with each series. The card stands where the
  first one is, shows it and carries the mark for several photos; opening
  it opens that photo, selecting or dragging it takes them all, and the
  strip under a photo shown full window underlines a stack's photos. A
  photo in another plugin's stack already stays there. When the library is
  read again, each stack's photos stay together where they are now until
  it gives `stacks` again (on `library.changed()`, from the ids it keeps,
  as Series does).
- `hidden`: photos not in the grid at all, a list of library positions,
  as Screenshots hides screenshots. They leave any stack they are in, the
  viewer's ← and →, its strip and Home/End pass over them, and Ctrl A and a
  range leave them out; a plugin can still `openPhoto` one. Say somewhere
  how many are away and how to have them back; with every photo hidden,
  the window says so, and names the plugin. Other plugins see what is
  hidden with `pinhole.isHidden`. When the library is read again, the
  photos it hid stay hidden where they are now until it gives `hidden`
  again (on `library.changed()`, from the ids it keeps).
- `marks`: marks in the bottom left corner of grid cells, beside
  Pinhole's own (a video's, a Live Photo's, a favourite's, a stack's),
  each a `PluginMark`, four at most (32 in all, Pinhole's among them; one
  past that is left out, and said under the plugin, as is one that is not
  right). Each has a switch of its own under the side panel's "In the
  grid", kept for this computer by its id, `<plugin id>/<name>`:

  ```qml
  marks: [
      PluginMark {
          name: "picked"            // unique in the plugin
          text: qsTr("Picked")      // its switch's words in the side panel
          icon: ""            // video, live, heart, stack, or one character (a Nerd Font glyph)
          colour: "green"           // red, green, yellow, blue, magenta, cyan, accent, or "#rrggbb"
          order: 25                 // left to right: favourite 10, video 20, Live 30, stack 90; 50 if left out
          photos: plugin.picked     // library positions (or ids)
          // or field: "live"       // the photos whose library field is true: live, favorite, video
          defaultShown: true        // its switch until it is first turned
      }
  ]
  ```

  A mark of a `field` costs nothing as the library changes. A mark of
  `photos` is made into a bit of each photo's once, when they are given;
  when the library is read again, its photos stay marked where they are
  now until it gives `photos` again (on `library.changed()`, from the ids
  it keeps), as with `hidden`. Nothing of a mark is QML's while the grid
  draws: it reads a word for each cell. Its glyph is drawn in the theme's
  font, white in the side panel, in its colour on the photos, and as a
  neon tube in it far out.
- `tour`: a few steps that show what it does, on the photos and buttons as
  they are: `[{ title, text, photo, open, action, settings }, ...]`.
  `photo` is a library position (or a function giving one) the grid goes to
  and lights, `open` one shown full window, `action` one of its
  `PluginAction`s whose button is lit, and `settings: true` lights its part
  of the side panel. Pinhole shows it by itself the first time the plugin is
  on, once `tourReady` is true and the grid is free, and again from the
  side panel. Series' shows a series of your own. Of the plugins on at
  launch, one tour at most shows by itself: with more than one not seen
  yet, a card lists those plugins instead, each once its tour is ready,
  with Show for its tour (that one then counts as seen), Skip all (they
  all do) and Later (none does, and the card comes back next launch). A
  plugin turned on in the side panel shows its own once the panel
  closes.

`Pinhole.Plugin` also has Pinhole's own `ShellButton`, `Chip`, `UiText`,
`Spinner`, `Toggle` and `Command` (a key, with what it does in the help),
and `theme` has the Omarchy theme's colours and type, so a plugin can look
like the rest.

Pinhole's keys are application shortcuts, and a field with the keys takes
letters, Space and the arrows first: a plugin's `TextInput` takes focus only
while it shows, and lets go (`focus = false`, or `enabled: false`) as its
panel closes. One hidden or turned off with the keys anyway gives them back
to the photos at the next key, never typed into.

### `pinhole`

`pinhole` is the plugin's way into Pinhole. A photo is named by its
position in the library (oldest first, as the grid has them) or by its id;
positions move when the library is read again, ids do not, so keep ids.

| | |
|---|---|
| `library` | `count`; `item(photo)`: `index`, `id`, `date` (ms), `filename`, `thumbnail` (a URL), `width`, `height` (the thumbnail's), `video`, `live`, `favorite`, `avgColor` (the thumbnail's average colour, `"#rrggbb"`, or `""` before it has one), or `{}` if there is no such photo; `field(name)`: one of those for every photo at once, in the library's order (`field("date")`: all the dates), far quicker than `item` for each, or `field("day")`: the day each was taken in local time, as a number `yyyymmdd` (20250108), before 1970 too, 0 for one with no date (its `date` 0); `[]` for `index`, `thumbnail` or a name it has not; `indexOf(id)`, -1 if none; `dir` (the source's library: `library.sqlite`, to read, and its thumbnails); `key` (which source's library it is, wherever it is kept: `icloud:me@example.com`, `folder:/home/me/Pictures`, `""` for one that does not say; for what a plugin keeps for each source apart); `keys()`, the key of every source added, as the settings say now (what a plugin keeps for one no longer there can go); and `changed()` when it is read again |
| `viewer` | the photo shown full window: `open`, `index` |
| `currentIndex()` | the photo under the pointer in the grid, else the middle one |
| `selection()`, `selectionCount` | the photos ticked in select mode, a card's all of them |
| `reveal(photo)`, `openPhoto(photo)` | the grid to a photo, marked there with a ring that fades (a hidden one: the next shown, where it would be); a photo full window |
| `isHidden(photo)`, `hiddenCount`, `hiddenChanged()` | whether a photo is hidden from the grid by any plugin's `hidden`, how many are, and when that changes (after `library.changed()` when the library is read again): what a plugin counting or showing photos leaves out, as the grid does |
| `fetch(photo, kind)` | a photo's file from the service (`medium`, `full`, `original`, `video`, `live`): a promise of its path. `full` and `original` are mostly HEIC, which an `Image` shows as `"image://photo/" + encodeURIComponent(path)`, upright, decoded at its `sourceSize` |
| `run(program, args, options)` | a program, its own (in its folder) or any on PATH: a promise of `{ stdout, stderr, code }`. `options`: `input` (to its stdin), `timeout` (ms), `env` (added to Pinhole's), `output(line)` (each line it writes, as it comes). `cancel()` asks it to end (SIGTERM) and kills it a second later |
| `record(item, options)` | an item as it looks over a number of frames, as a GIF or an MP4: a promise of the file's path. `options` are `path`, `format`, `width`, `height`, `fps`, `frames`, `seek(frame)` and `progress(fraction)` |
| `notify(text)` | a few words at the foot of the window for a while, as undo's: "Saved to Pictures" |
| `keepAwake(on)` | the screen kept on, no screensaver and no lock, while `on`, as the Photo frame does while it shows; `false` lets it go, and so does the plugin turned off or loaded again, and Pinhole quitting or crashing. Whether the screen is kept on now: not where it cannot be (not on Wayland, the window hidden, or on a workspace not shown), which it then is once it can |
| `warn(error)` | something gone wrong, under the plugin in the side panel and on stderr |
| `settings` | its settings, kept in `~/.config/pinhole/plugins.json`: read one (`pinhole.settings.size`), bind to one, assign one and it is kept at once |
| `setting(key, fallback)`, `setSetting(key, value)` | the same, by name; `setSetting` says whether it could be kept, and `null` takes the setting out. Series keeps its edits as `edits:` and the library's `key`, so each source's are its own, and Screenshots what it found as `found:` and the `key`, read as it loads (none for a library whose `key` is `""`, nor past 5,000, and those of sources no longer added gone) |
| `did(name, undo, redo)` | a change just made, to undo with Ctrl Z and do again with Ctrl ⇧ Z: `name` says what in a word or two ("ungrouping"), `undo()` and `redo()` do it. Forgotten when the plugin is turned off or loaded again, and when another source's library is shown, however it came to be |
| `copyFile(path)` | a file on the clipboard, an image as one a chat takes pasted too (a JPEG as it is, a GIF as it is, and a PNG of at most 2560 px, a GIF's first frame), as Pinhole's own copy has it: a promise of the path once the clipboard has it, which for a big picture is a moment later (its PNG is made first, away from the window), so say "Copied" then, not before; `not_found` if there is no such file. The last copy asked for wins: if another (a plugin's or Pinhole's own) is asked for while its picture is made, this one is never set, and is `cancelled` |
| `showFile(path)` | its folder in the file manager |
| `picturesDir()`, `exists(path)`, `fileSize(path)`, `sizeText(bytes)` | |
| `dir`, `cacheDir` | its own folder; a folder for what it can make again, one to each source's library (`cacheDirChanged()` when another source's is shown) |
| `coveredBy` | the plugin over the photos now, its id, `""` for none (see `covering`) |
| `dismissed()` | the plugin over this one, covering, handed a photo over (`openPhoto`, `reveal`): close, and do not come back when that photo closes |
| `id`, `api` | its id; the version of all this |

### Promises

What takes a while (`fetch`, `run`, `record`, `copyFile`) gives a
promise, which also has `cancel()` (`copyFile`'s does nothing):

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
| `copyFile` | `not_found` (no such file), `cancelled` (another copy asked for after it has the clipboard) |
| `run` | `exit` (it ran and did not exit 0: `exitCode`, `stdout`, `stderr` too, and the message is what it wrote to stderr), `start` (it could not be started), `timeout`, `cancelled` |
| `record` | `failed` (ffmpeg, or the item gone), `cancelled` |

End a chain with `.catch(pinhole.warn)`: what a `then` throws is otherwise
said nowhere. There is no `finally` in Qt's JavaScript; a `.then()` after
the `.catch()` does the same. Nor is there `async`/`await` in QML.

Everything a plugin started ends with it: turned off or loaded again, its
programs are stopped (asked to end with SIGTERM, killed a second later; so
too when the window closes), its recordings dropped, and no promise or
callback of its old self is answered.

Before API 2 these took a callback last in place of a promise
(`fetch(i, kind, done(path, error))`, `run(program, args, done(code,
stdout, stderr))`, `record(item, options, progress, done(path, error))`);
they still do.

### Translations

Strings in `qsTr()` are translated from `i18n/<language>.json` in the
plugin's folder, as Pinhole's own are (`app/src/i18n`): the English as
the key, the translation its value, or an object of plural forms for a
counted one. Its catalog is its own: a string in its files is looked up
there first, then in Pinhole's, and never changes what Pinhole or another
plugin says. The manifest's `name` and `description` are looked up there
too, so the side panel and the help name it in the language. Qt names a
string's file only by its name, though: a file named as one of another
plugin's, other than `Main.qml`, may get that one's words for the same
English, and one named as one of Pinhole's own (`Toggle.qml`,
`Spinner.qml`…) Pinhole's first. Give each a name of its own, as Contact
sheet's `SheetStudio.qml` beside Series' `Studio.qml`; `pinhole plugin
check` warns of one that is not.

### Trust

A plugin runs inside Pinhole with all of Pinhole's rights, as Omarchy's do:
only turn on one whose code you would run yourself.
