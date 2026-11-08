# Plugins

Open the offline handbook in **Settings (Tab) → Plugins → Build a plugin**.
Start with [the quick start](plugin-quickstart.md), or copy the
[implementation contract for an agent](plugin-agents.md). The installed
version's complete references are also available with
`pinhole plugin docs --json`. The app and CLI bundle these exact source files.

What Pinhole does beyond showing photos comes as plugins, and anyone can
write one for themselves: a one-off that does exactly what you want with
your photos, and nobody else needs to see. Series (`plugins/series`) is
Pinhole's own and the fullest example.

A plugin is a folder with a `manifest.json` and a QML file, as Omarchy's
are. Pinhole's own are in its plugins folder (`/usr/lib/pinhole/plugins`
installed, `plugins/` in a checkout) and are on unless turned off, or off
until turned on where their manifest says `"startsOff": true`; your own
go in `~/.config/pinhole/plugins/<id>/` and start off, to be turned on in
the side panel (Tab). One of yours with a built-in's id takes its place, so
copying a built-in there is how to change it.

## Writing one

```sh
pinhole plugin new me.favourites
```

makes `~/.config/pinhole/plugins/me.favourites` with a manifest, a
`Main.qml` that already does something and its words in Polish: on F2,
the newest photos on a screen of their own, in a grid that scrolls and
pinches exactly as Pinhole's does, Esc to close. Turn it on in the side
panel, and change it: saving any of its files loads it again at once, and
stops whatever its old self had started. What goes wrong shows in red
under it in the side panel: QML that does not load, an error thrown, a
warning from one of its files, each with the file and the line.

Build it from the kit (below) and it is right without knowing why: its
lists scroll as the grid does, nothing it shows lets a click, a wheel or a
pinch through to the photos, and what it makes goes where Pinhole's goes.

```sh
pinhole plugin check me.favourites
```

looks it over as Pinhole would load it, without a window: the manifest,
every QML file in it (one a `Loader` loads by name too) and the
JavaScript they import, what they use of `pinhole`, `pinhole.library`
and `theme` (a misspelt name is said with the one meant), its translations,
and the footguns (What check looks for, below). It takes a folder too
(`pinhole plugin check .`), and exits 1 if anything is wrong, for an
editor or a script to run on save. A manifest's `sources`
are checked as pinholed takes them (a provider's kind namespaced by the
plugin's id, its program in the folder and runnable, `accepts` and
`roundsEverySecs` as [source-providers.md](source-providers.md) says),
and so are its `workers` (Workers, below),
so what pinholed would refuse is said here, and under the plugin in the
side panel. It refuses anything the plugin uses of an API newer than its
manifest asks for, saying the number to give, and warns of a file named as
one of another plugin's or of Pinhole's own (see Translations).

## The kit

What Pinhole's own plugins once wrote by hand, and got wrong, is in
`Pinhole.Plugin`, right by default. Every property, signal and function
of each is in [the kit, member by member](plugin-kit.md)
(`pinhole plugin docs plugin-kit`):

- `PluginView`: a view of the plugin's own over the photos, the
  calendar, a gallery, a studio, in one line:

  ```qml
  PluginView { id: calendar; source: "Calendar.qml"; toggleKey: "Y" }
  PluginAction { place: "grid"; text: qsTr("Calendar"); key: "Y"; onTriggered: calendar.open() }
  ```

  While open the plugin covers the photos (`covering` follows it, unless
  bound by hand), and it fades in and out. It is made the first time it
  opens and kept (`lazy: false` makes it at once). Esc and its `toggleKey`
  close it, as does a photo handed over from it (`dismissed()`); while a
  photo opened from it with `comeBack` shows, it is hidden and still open,
  and a view opened meanwhile (`openedAway`) shows over that photo and
  covers it, as People's W does.
  `open()`, `close()`, `toggle()`, `shown`, `opened()`, `closed()`, and
  `item`, what `source` (a file beside the main one) or `content` (a
  Component) made. In a `source` file `view` is its view from the first
  binding on (`view.shown`, `view.close()`, `view.plugin`, the plugin's
  root), as `pinhole` is the plugin's: declare neither. `open(…)` hands
  what it is given to the item's `start(…)` before it shows (an Apple ID,
  the photos to send) and `close()` calls its `stop()`, so a view that
  reads something as it opens needs no handler in the main file.
  `keep: false` lets go of the item as it closes, made afresh the next
  time. A `PanelHeader` in it closes it with ✕ by itself. Esc goes to what it shows first: a `function back()` there that
  returns true has taken it (a close-up left, a making stopped) and the
  view stays open. A plugin's views open at once (a studio over its
  editor) stack: only the last opened has its keys (`onTop`), and Esc
  closes it alone. Its `toggleKey` does as Esc does. `make()` makes what
  it shows without opening it, and gives it. `closeText` puts Esc in the
  help with those words (none by default: the action that opens it is
  listed); `closeCommand.mouse` and `.touchpad` say how else it closes.
- `Job`: one thing asked at a time, the latest winning. It
  cancels the one before, drops an answer that came too late, has `busy`,
  asks again by itself when the library was read meanwhile (`reloaded`),
  says nothing of a cancel, and stops with the plugin:

  ```qml
  Job { id: reading; onDone: rows => panel.rows = rows; onFailed: error => panel.problem = error.message }
  reading.start(() => pinhole.query(sql, [limit]))
  ```

  `coalesce: true` waits for the one running and then asks once more with
  the latest instead of cancelling it (a search typed into). `ask` may
  return a list of promises asked together (a page of previews): `done`
  has their answers in order, the first to fail fails them all, and a
  cancel cancels each. `start(ask, answered)` gives this one answer to
  `answered` instead of `done`, for an answer that needs what it was
  asked with: `motion.start(() => pinhole.fetch(id, kind), file =>
  play(kind, file))`.

- `ScrollList`, `ScrollGrid`, `ScrollArea`: a `ListView`, a `GridView` and
  a `Flickable` (anything larger than its room: give it a `contentWidth`
  and a `contentHeight`), each scrolled by the wheel and the touchpad
  exactly as the grid is: the same physics, its inertia, its springs at
  the ends and its wheel's glide, measured against the grid's own by a
  self-test. Up and down, and across where the
  content is wider (a strip: `orientation: ListView.Horizontal`, which the
  wheel then moves across too). They take every wheel turn, swipe and pinch
  over them, at their ends too, so the photos beneath never scroll; one in
  another (a list in the side panel's settings) moves first, to its end.
  A click or a touch while one coasts only stops it, as on the grid.
- `PinchZoom`: what it holds, grown and shrunk through a few sizes as the
  grid zooms through its columns: a pinch, Ctrl and the wheel, and the
  sideways wheel, under the fingers and about them, landing on the size
  nearest where the pinch was heading, eased; `zoomBy(1)`, `zoomBy(-1)` for
  + and -. `levels` are its sizes, smallest first, `level` the one laid
  out, `size` that size: lay what it holds out at `size`, and it is drawn
  larger or smaller while it zooms. A `ScrollGrid` in it passes its
  pinches on, so a grid of cells scrolls and pinches as Pinhole's.
- `PanZoom`: a surface moved and zoomed without levels (a map, a
  picture larger than its room) on the grid's physics. It says how far,
  the plugin moves its own camera: `panned(dx, dy)` (content pixels, right
  and down positive), `zoomed(x, y, steps)` (steps of 2 around a point,
  positive in), `tapped(x, y)`, `doubleTapped(x, y)`. A touchpad's swipe
  pans and coasts on as the grid scrolls (fingers coming to rest stop it);
  a mouse drag pans one to one and coasts on if let go moving; the wheel,
  and Ctrl with a swipe, zoom around the pointer, gliding notch by notch
  (`zoomPerNotch`, 0.5); a pinch zooms with the fingers. `moving`,
  `dragging`, `stop()` (call it when the camera is put elsewhere: Fit all,
  a key) and `zoomBy(x, y, steps)`. It takes every wheel turn, swipe and
  pinch over it; its children (markers) take their own clicks. Info's map
  is one.
- `theme`: the window's colours and type, as Omarchy's theme sets them:
  `background`, `darkBackground`, `darkerBackground`, `lighterBackground`,
  `foreground`, `darkForeground`, `brightForeground`, `muted`, `accent`,
  `accentForeground`, `selection`, `red`, `yellow`, `green`, `cyan`,
  `blue`, `magenta`, `light` (a light theme), `font`, `fontSize` and
  `radius`.
- `Panel`: a screen over the photos, its parent's whole room in the theme's
  background (or a `color`), taking every click, touch, wheel, swipe, pinch
  and hover, so nothing reaches the photos; `triggered()` where no control is,
  and with `steps` (`Qt.Horizontal`, `Qt.Vertical`) the wheel and a swipe
  step: `stepped(1)` the next, `stepped(-1)` the one before (a notch a
  step, a swipe one), as the Photo frame and the Series editor step.
- `Card`: a box in the theme, the same: nothing over it goes through. Over
  the photos for a moment, on a dimmed `Panel` whose click puts it away.
- `PhotoTiles`: photos as square tiles, as a view shows what it
  found: `library` (`pinhole.library`), `photos` (ids, in order), `active`
  (its keys while true: ← → ↑ ↓, Home, End, PgUp, PgDn, ⏎, Space),
  `cursor`, `minCell`, and `opened(photo, index)`; `moveTo(id)` puts the
  cursor on one (as `returned(id)` says), `pastTop()` is ↑ from the first
  row (to a field above), `caption` a Component under each tile
  (`captionHeight` tall, given `photo` and `index`). Each drawn at the
  display's own pixels, the ring on the cursor's and under the pointer as
  Pinhole's own.
- `PanelHeader`: the title bar of a view over the whole window,
  as the map's: `title`, a quieter `detail`, `tools` (buttons at the
  right), then ✕ pressing `closeCommand` and showing its key (or
  `closed()` and `closeHint`); its children go before the title (a ‹ back
  button), and `progress` (0 to 1, -1 for none) is a line along its foot.
- `SettingNote`, `SettingSwitch`: a settings component's few
  words (`dim` for a remark) and a setting on or off (`text`, `checked`,
  `usable`, `hint`, `toggled()`), as Pinhole's own plugins set theirs.
- `Choices`: one of a few on a row of buttons, each also its number's key:
  `options` as `[{ value, text }]`, `value`, `chosen(value)`; `numbered:
  false` for a settings row, without the number keys.
- `ShellButton`'s `complain(text)`: what pressing it could not do, under it
  for four seconds, the button shaking, as a `PluginAction`'s.
- `pinhole.picturePath(name, extension)` and `pinhole.copyFile(path)`: a
  file to make in Pictures/Pinhole, named as Pinhole names what it makes
  (`name 2.png` when `name.png` is there), and a file on the clipboard. In a
  self-test both are a scratch folder's and the window's own: never the
  person's Pictures or clipboard.
- `pinhole.query(sql, values)`: the library's index asked, read only, away
  from the window, waiting while the service writes it: a promise of its
  rows, with the bounds and cancellation described below. Use it instead
  of opening the database with an external `sqlite3` process. `pinhole.library.field(name)` has a field
  of every photo at once, `"day"` among them.
- `settings`, `state` and `cache` (below): the user's choices, what they
  made, what can be worked out again.

A plugin covers the photos only while its `covering` is true, under
Pinhole's own panels and over the other plugins, with the `"plugin"` keys
its own: the template's `covering: open` is all it takes.

The [workshop examples](plugin-examples.md) give three complete starting
points: a custom view, a file-making tool and a photo-source provider, with
all files, dependencies and run instructions. Executable copies live in
`app/tests/examples/` in the source checkout.

The following smaller examples show additional kit features.

A `PinchZoom` over tiles laid out at its `size`:

```qml
import QtQuick
import Pinhole.Plugin

// F6: the newest photos' colours as tiles, grown and shrunk by a pinch, Ctrl
// and the wheel, + and -, through three sizes, as the grid zooms.
PinholePlugin {
    id: plugin
    property bool open: false
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Tiles"); key: "F6"; onTriggered: plugin.open = true }]
    Command { name: "tiles.close"; keys: ["Escape"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: plugin.open = false }
    Command { name: "tiles.in"; keys: ["+", "="]; keyLayer: "plugin"; usable: plugin.open; onTriggered: zoom.zoomBy(1) }
    Command { name: "tiles.out"; keys: ["-"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: zoom.zoomBy(-1) }

    Panel {
        visible: plugin.open
        PinchZoom {
            id: zoom
            anchors.fill: parent
            levels: [24, 48, 96]
            level: 1
            Flow {
                anchors.fill: parent
                Repeater {
                    model: plugin.open ? Math.min(pinhole.library.count, Math.floor(parent.width / zoom.size) * Math.floor(parent.height / zoom.size)) : 0
                    delegate: Rectangle {
                        required property int index
                        width: zoom.size
                        height: zoom.size
                        color: pinhole.library.item(pinhole.library.count - 1 - index).avgColor || "transparent"
                    }
                }
            }
        }
    }
}
```

For `Card`, `Choices`, `picturePath` and `copyFile`, use the complete
[Swatch tool example](plugin-examples.md#tool-swatch).

The index asked with `pinhole.query`, the answer in a `ScrollArea`:

```qml
import QtQuick
import Pinhole.Plugin

// F8: the library's files, by how many are videos and how many know where
// they were taken, asked of its index with SQL, in a ScrollArea
// wider and taller than the window.
PinholePlugin {
    id: plugin
    property bool open: false
    property string report: ""
    property var pending: null
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Places"); key: "F8"; busy: plugin.pending !== null; onTriggered: plugin.ask() }]
    Command { name: "places.close"; keys: ["Escape"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: plugin.open = false }

    onOpenChanged: if (!open) cancelQuery()
    function cancelQuery() {
        const job = pending
        pending = null
        if (job) job.cancel()
    }
    function ask() {
        cancelQuery()
        open = true
        report = ""
        const job = pinhole.query("SELECT is_video, has_location, count(*) AS n FROM assets GROUP BY is_video, has_location")
        pending = job
        job.then(rows => {
            if (pending !== job || !open) return
            pending = null
            report = rows.map(r => (r.is_video ? "video" : "photo") + (r.has_location ? ", placed: " : ": ") + r.n).join("\n")
        }).catch(error => {
            if (pending !== job) return
            pending = null
            pinhole.warn(error)
        })
    }

    Panel {
        visible: plugin.open
        ScrollArea {
            anchors.fill: parent
            anchors.margins: 32
            contentWidth: text.implicitWidth
            contentHeight: text.implicitHeight
            UiText { id: text; text: plugin.report; font.pixelSize: 96 }
        }
    }
}
```

### What check looks for

`pinhole plugin check` refuses, with what to use instead (each one a
footgun Pinhole's own plugins hit):

- a `ListView`, `GridView`, `Flickable` or `ScrollView` (`ScrollList`,
  `ScrollGrid`, `ScrollArea`), and a `TableView`, `TreeView`, `PathView`,
  `Tumbler` or `SwipeView`: they scroll not as the grid does, and at
  their ends let the wheel through to the photos;
- a `PinchHandler`, `PinchArea` or `MultiPointTouchArea` (`PinchZoom`, or
  `PanZoom` for a surface without levels), a
  `WheelHandler`, and a `MouseArea`'s `onWheel` (a `Panel` or `Card` under
  the controls, a `Panel`'s `steps`): a pinch went through to the photos,
  and a `MouseArea` without `onWheel` lets the wheel through; so is one
  over the whole window, a `MouseArea` filling the plugin's root (or an
  `Item` filling it): what a plugin shows over the photos goes in a
  `Panel`;
- `Qt.createQmlObject`: QML made from a string, which check cannot look
  over (a file of the plugin's, with a `Loader` or `Qt.createComponent`);
- a key of `pinhole.state`, `synced`, `cache` or `settings` changed inside
  (`pinhole.state.edits[id] = x`, `.push()`, `delete`): a copy is changed,
  nothing is kept and nothing bound to it hears. Copy the key, change the
  copy, assign it whole;
- a property named `on` and a capital (`property bool onAir`): QML reads
  it as a signal's handler;
- a file imported from outside the plugin (`import "../screenshots/…"`):
  another plugin's file changes, or is replaced, without it;
- `import Pinhole.Internal` (or one of its modules, `Pinhole.Internal.Info`)
  in a plugin whose id is not `pinhole.…`: it is Pinhole's own plugins',
  with no promise it stays;
- a `VideoOutput` or QtMultimedia's `Video` (`VideoView`): they show HDR
  video wrong;
- a QtQuick.Controls `Menu`, `Popup`, `ComboBox`, `Dialog` or `Drawer`,
  and a `ContextMenu`: they open over the whole window, outside the
  plugin's `Panel`, and hide what they act on. Pinhole has no right-click
  menus (a touchpad's two-finger tap is a right click, which stops what
  moves): actions go on controls one can see, a `ShellButton`, `Choices`
  or a `PluginAction`;
- a `Command` without a `keyLayer`, or with `"any"` or `"chrome"`: its
  keys would be over the grid and the viewer whenever the plugin is on,
  Pinhole's own among them. `"plugin"` with a `usable` has them while its
  view is shown; `"grid"` or `"viewer"` there, where a key Pinhole or
  another plugin has is said under the plugin, as an action's is;
- a member `pinhole`, `pinhole.library` or `theme` does not have, and a
  `library.field()` it does not know: undefined, or an empty list, as it
  runs;
- a `PluginAction`'s `place`, a `PluginMark`'s `icon` or a `colour` /
  `textColour` Pinhole does not know: nothing is drawn;
- a file a `Loader`, `Image` or `Qt.createComponent` takes from outside
  the plugin (`"../series/Studio.qml"`, `file:///…`);
- `copy()` of a text field: it sets the session's clipboard
  (`pinhole.copyText`);
- in `manifest.json`, a key it never reads (`"verison"`, with the one
  meant), an `api` that is not a whole number, an id
  of anything but letters, digits, `.`, `_` and `-`, and a `pinhole.` id
  of one's own (they are Pinhole's plugins');
- `sqlite3` (`pinhole.query`), named anywhere in a string: by its path,
  in a shell's line (`sh -c "…"`), put together (`"sqlite" + "3"`);
- `wl-copy`, `xclip` and the like (`pinhole.copyText`, `pinhole.copyFile`), and Pictures found
  by hand, `~/Pictures`, a `/Pictures` to build a path with, or
  `xdg-user-dir` (`pinhole.picturePath`), in a string or a shell's line: a
  self-test of them wrote to the real Pictures and the session's
  clipboard. The words a plugin shows, in `qsTr()`, are not looked at
  ("Kept in ~/Pictures/Pinhole" is fine). `pinhole.run` refuses the same
  programs as it runs them, however their names were made: by name or
  path, after `env` and the like, or in a shell's line (`sh -c`), with an
  error `refused` saying what to use;

And warns of:

- a setting written in a handler that runs often (`onTriggered`,
  `on…Changed`) or from a list or the library: settings are the user's
  choices, and `plugins.json` is written again each time (`state`,
  `cache`);
- a key named as Qt writes it in what is shown, `Shift+S` or `Return`, not
  as the buttons name it (⇧ S, ⏎);
- words shown as they are, in `text:`, `notify(…)` or the `help` without
  `qsTr()`;
- words in `qsTr()` with no Polish in `i18n/pl.json` (nor Pinhole's own);
- words put together inside `qsTr()` (`qsTr("Copied " + n)`): none of
  them is found to translate. One literal, with `%1` and `.arg()`, or
  `%n` for a count.

## A whole plugin

`manifest.json`:

```json
{
  "api": 15,
  "id": "me.favourites",
  "name": "Favourites count",
  "version": "1.0.0",
  "description": "How many favourites, on F2."
}
```

| Key | |
|---|---|
| `id` | its folder's name if left out: letters, digits, `.`, `_` and `-`, beginning with a letter or a digit; yours (`me.…`), as `pinhole.…` are Pinhole's own plugins'. It names the plugin's switch, settings, state and cache |
| `name`, `description` | what the side panel says of it (translated in `i18n/<language>.json` too) |
| `version` | yours, shown with its name |
| `api` | the plugin API it is written for, this Pinhole's: 15, required (below) |
| `sources`, `workers`, `makes` | the kinds of source it brings, the programs pinholed runs for it, and what it makes (below) |
| `startsOff` | `true`: one of Pinhole's own that is off until turned on in the side panel, there for whoever wants it (Series, People, the calendar…); left out, it is on. Yours start off anyway |
| `requires` | the programs it needs, by their names on PATH: `["magick", "ffprobe"]`. One not installed is said under the plugin in the side panel; it is loaded all the same. `pinhole.has(name)` asks for one it can do without |
| `author`, `license` | yours |

What Pinhole loads is always the `Main.qml` beside it.

A key not among these is never read: `plugin check` says so, with the one
meant.

`api` is the plugin API it is written for, and it is required: this
Pinhole's is 15, and a plugin of any other is not loaded, older or newer
(`plugin check` and the side panel say which it is). There is no running
a plugin written for an older one: when the API changes, a plugin is made
of the new one. `pinhole.api` says which one it is running in.

`Main.qml` beside it:

```qml
import QtQuick
import Pinhole.Plugin

PinholePlugin {
    id: plugin
    property int count: 0
    property bool counted: false
    // No scan at startup, and no per-photo item() calls.
    function recount() {
        count = pinhole.library.field("favorite").filter(favorite => favorite).length
        counted = true
    }
    Connections { target: pinhole.library; function onRevisionChanged() { plugin.counted = false } }

    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("Count favourites")
            key: "F2"
            onTriggered: {
                if (!plugin.counted) plugin.recount()
                pinhole.notify(qsTr("%n favourite(s)", "", plugin.count))
            }
        }
    ]
    help: [["F2", qsTr("Count favourites")]]
}
```

`i18n/pl.json` beside it:

```json
{
  "Favourites count": "Licznik ulubionych",
  "How many favourites, on F2.": "Liczba ulubionych po F2.",
  "Count favourites": "Policz ulubione",
  "%n favourite(s)": {
    "one": "%n ulubione zdjęcie",
    "few": "%n ulubione zdjęcia",
    "many": "%n ulubionych zdjęć",
    "other": "%n ulubionych zdjęć"
  }
}
```

## Workers

A plugin that works through the whole library in the background (finding
faces, indexing what photos show) declares the program that does it in its
manifest, and the shared service, pinholed, keeps it running, with or
without a window:

```json
{
  "api": 15,
  "id": "pinhole.people",
  "workers": [
    { "name": "faces", "program": "pinhole-faces", "args": ["daemon"], "per": "source" }
  ]
}
```

| Field | |
|---|---|
| `name` | its name among the plugin's workers: letters, digits, `.`, `_` and `-`, at most 64 |
| `program` | a file in the plugin's folder that can be run, by a relative path without `..` |
| `args` | strings given to it first (none if left out) |
| `per` | `"source"`, the only one there is (also if left out): one runs for the source shown |
| `answers` | `true`: it answers what its plugin asks, `pinhole.ask` (below); `false` if left out |

pinholed finds them as it finds providers
(source-providers.md): in Pinhole's own plugins and yours, a plugin of
your own with a built-in's id in that one's place. It looks again every
two seconds, so a manifest changed, a plugin added or a switch in
`plugins.json` is followed without a restart. A worker runs while its
plugin is on, as the side panel has it (a built-in plugin unless turned
off, one of your own only once turned on), and a source with a library is
shown. It is given, after `args`:

```
--library <the library's folder, with library.sqlite>
--library-key <the source's id, the library's meta.source>
--cache-dir ~/.cache/pinhole/plugins/<id>/<kind>-<name>/
--managed-plugins ~/.config/pinhole/plugins.json
```

and, in its environment, `PINHOLE_SYNCED`: the plugin's `pinhole.synced`
file of that library, `{"values": {…}, "clock": {…}}`, whose
`values` are what the QML reads; it may not be there yet. Read it again
when it changes, as the settings below; never write it.

`--cache-dir` is the plugin's `pinhole.cacheDir` for that library, made
before it starts, so what the worker writes there the plugin's QML reads
(`pinhole.readText`, `readJson`). Its last part is the library's folder
and the one it is in, joined: `icloud-me@example.com`,
`folder-Pictures`. `--managed-plugins` is the plugins' switches and
settings, a JSON file with the plugin's settings at `settings` → its id:
`{"settings": {"me.dupes": {"videos": true}}}`; one never chosen is not
there, and its default is the manifest's `settings`. Nobody tells the worker
when they change: read the file again when its modification time does
(a look every few seconds is plenty). It starts ten points lower in CPU priority than
pinholed (`nice`); lower it more, or its I/O, as it needs. Its stdout and
stderr go into `~/.local/state/pinhole/pinholed.log`, each line after its
plugin's id and name (at most 200 lines in ten seconds, 2,000 bytes a
line, as a provider's). Read the library only: open `library.sqlite`
read-only, and give up on a library whose `meta.source` is not
`--library-key`.

It is stopped when another source is shown, its plugin is turned off or
its manifest changes it, by closing its stdin: watch stdin and end at EOF
(also how it ends with pinholed, however that ends). One still running
five seconds later is killed. One that ends by itself is started again ten
seconds later. A service started for a library given (`--given`) runs
none.

Its plugin's QML has `pinhole.workers`: each of its workers'
`{ name, running, since, lastExit, key, said }`, as pinholed runs them,
with `workersChanged()`; `said` is what it said last while it
runs, or null, so a panel opened halfway through a scan shows how far it
is at once. A worker that ends, or is killed, is `running: false` with
its `lastExit`: notice it with `onWorkersChanged`, not a timer. A line the worker writes on its stdout that is a JSON
object with an `"event"` is not logged but told to the plugin's windows:
`pinhole.workerSaid(name, said)`, `said` the object, the latest of each
quarter second (what it wrote meanwhile is left out). Say there what it got
further with, and read it again from `cacheDir` then, rather than ask on a
timer:

```qml
Connections {
    target: pinhole
    function onWorkerSaid(name, said) { if (name === "faces") panel.load() }
}
```

What it wrote in `cacheDir` the QML reads with `pinhole.readJson`:

```qml
function load() {
    pinhole.readJson(pinhole.cacheDir + "/groups.json")
        .then(groups => panel.groups = groups)
        .catch(error => { if (error.code !== "not_found") pinhole.warn(error) })
}
```

### Asking a worker

A worker that keeps something in memory (an index of what photos show, a
model loaded once) can answer its plugin's questions, instead of a
program started for each one loading it all again. Its manifest says
`"answers": true`, and the QML asks:

```qml
pinhole.ask("index", { text: field.text, limit: 50 }, { timeout: 10000 })
    .then(result => panel.found = result.ids)
    .catch(error => { if (error.code !== "cancelled") panel.problem = error.message })
```

pinholed writes each question to the worker's stdin as a line of JSON,
`{"ask": 7, "request": {"text": "cats", "limit": 50}}`, the request as
the QML gave it; the worker answers on its stdout with a line of its own,
`{"answer": 7, "result": {"ids": [...]}}`, or `{"answer": 7, "error":
"why"}`, in any order and while others wait (the `ask` number says which).
What it says otherwise is as before: its news, its log. The promise is of
the `result`; rejected with `code` `failed` (its `error` the message),
`timeout` (none in `timeout` ms, 30 s unless said), `not_running` (not
started yet, or it stopped before it answered: no source shown, the plugin
turned off), `unsupported` (its manifest does not say it `answers`),
`not_found` (no such worker of the plugin's), `cancelled` once `cancel()`
or the plugin stopped (the worker's answer then goes nowhere), or the
service's. Its stdin still closes as it is stopped: read the lines until
EOF, then end.

## What a plugin makes

A plugin can make a photo's renditions a source has no file of, while
someone waits for them: the Live Photos plugin makes a Live Photo's Bounce
and Long exposure from its motion. Its manifest says what, from what, and
the program of its own that makes it:

```json
"makes": [
  { "kind": "bounce", "from": "live", "type": "public.mpeg-4", "extension": "mp4",
    "program": "live-effect", "args": ["bounce"] }
]
```

| Field | |
|---|---|
| `kind` | what a window's `fetch` asks for: `bounce` or `long_exposure`, one plugin each, or a kind of its own, `"<its id>/<name>"` (`"me.steady/steadied"`), the name lowercase letters, digits, `_` and `-` |
| `from` | the rendition it is made from: `live`, `full`, `original` or `video` |
| `type`, `extension` | what it makes: its UTI, and its file's extension, lowercase |
| `program`, `args` | a program in the plugin's folder, and what it is given first |

When `fetch` asks for that kind of a photo whose source has none, pinholed
gets the rendition it is made from, then runs the program in its plugin's
folder with `args`, the file it is made from and the file to write (with
the extension given): `live-effect bounce <motion> <…>.part.mp4`. It runs
at normal priority, someone waiting, within the limits background work
has (two minutes without output, five of CPU), in a process group of its
own that ends with it; exit 0 with the file written, and pinholed keeps it
in the cache, made once. Turned off, nothing makes that kind, and `fetch`
is answered `turned_off`. `plugin check` checks `makes` as pinholed takes
it.

A kind of its own is always made, a source never has one: `fetch(photo,
"me.steady/steadied")` from any plugin gets it, made from the photo's
`from` and kept in the cache beside that, as long as its plugin is on. Its
id in front keeps it apart from another plugin's and from the kinds a
source has.

## What a plugin is given

The root is a `PinholePlugin`. It covers the window, above the photos and
the viewer and below Pinhole's own panels, and what it shows there is its
own. It loads once the library has been read, before the window's first
frame: what it sets in `Component.onCompleted` from the library, or from
its settings, is there as the window first shows. It sets:

- `actions`: `PluginAction`s, drawn as Pinhole's buttons. `place` is
  `"grid"` (top right, before Select), `"viewer"` (a photo full window,
  before Copy) or `"selection"` (select mode's bar, after Copy); then
  `text`, `key`, `helpText` (what the help says it does; its
  `text` if left out), `shown`, `usable`, `active` (a button filled with the
  accent; in a menu, a subtle fill and a check), `textColour` (its words in a colour while not lit: a theme
  colour's name, as a mark's `colour`, or `"#rrggbb"`; the theme's text
  if left out), `busy` (its arc turning once it has been for `busyDelay`
  ms, 1000 unless said), and `onTriggered`.
  `order` places it among the others of its place, lowest first, then in
  the plugins' order (0 if left out); in select mode's bar, those below 0
  come before Copy, and in the viewer's they stand as Pinhole's own
  buttons do, never folded into More ▾ (Favourites' heart is -20, Live
  Photos' -10). Its
  `complain(text)` says what it could not do in a few words under its
  button for four seconds, the button shaking, as Copy does (under More ▾
  while it is folded there).
  `key` is a sequence as Qt writes one ("F2", "Shift+S", "Ctrl+Return"),
  one no other plugin's action and none of Pinhole's own keys in that place
  has: two shortcuts on one key are ambiguous and neither works, so a key
  already taken is said in red under the plugin;
  and the button and the help show it as Pinhole names keys: F2, ⇧ S,
  Ctrl ⏎. In
  the viewer and select mode, actions with the same `group` are one button,
  the first of them shown, with the rest in a menu under ▾ beside it, opened
  with ⇧ and that button's key; there an action's `hint`, a word, stands
  at the right of its row in place of its key ("iPhone"). Pinhole sets its
  `hovered` while the pointer
  is on it, for the plugin to show what pressing it would do, as Series
  shows the series a photo would join. Where the window is too narrow for
  them all, their keys go from the buttons, then the plugins' buttons fold
  into one, More ▾, whose menu lists them with their keys; the keys work
  either way, and a group's ⇧ key opens that menu on its row. More ▾ is
  lit while one of them is `active`, and turns while one is `busy`.
  `places`, in place of `place`, puts one action in several:
  `places: ["viewer", "selection"]` is a button in each, its key in
  each, where a plugin declared twins before (Send's). Pressed,
  `pressedIn` says where, and `photos` the ids it is for: the photo
  shown in `"viewer"`, those ticked in `"selection"`, none in `"grid"`:
  `onTriggered: panel.open(photos)`. In each it is `usable` only with
  something to do it on (a photo shown, photos ticked, any photos at
  all), unless it is `busy`, so what is on its way can still be shown.
  Its `complain(text)` says it under the button it was pressed with.
  `place: "file"` is no button of its own but something it does
  with a file another plugin made, which that plugin offers where it shows
  the file: Background's `"Background"` on Series' video. `accepts` lists
  the extensions it does, lowercase (`["mp4", "jpg"]`; none, any), and
  `file` is the one it is done with as it is triggered. A plugin finds
  them in `pinhole.fileActions`, keeps those that are `shown` and whose
  `takes(path)` is true, shows each as a button with its `text`, `key`
  and `busy`, and presses it with `trigger(path)`, which sets `file` and
  triggers it (false, doing nothing, when it is not `usable`, is `busy` or
  does not take it). Its `key` is the offering plugin's to press it with,
  on its own keys: no clash with the grid's or the viewer's.
  Menus share the photo-order menu's layout and interaction: a pointer
  opening does not focus the first action; hover and keyboard focus are
  distinct from the saved active state. Press the opener again, click
  outside or press Escape to close. Arrow keys skip unavailable actions.
- `covering`: true while it is over the photos: the grid's controls fade
  and its keys rest, as for Pinhole's panels. It has the keys then: unless
  something of its own took them first (a field, a list), Pinhole gives
  them to its root, so a `Keys` handler there and a `Command` work at once;
  give them to an item of yours with `forceActiveFocus()` when its `usable`
  asks for that item's `activeFocus`. A `FieldInput` with something typed
  takes Escape first and clears it (`cleared()`); with nothing typed,
  Escape goes on to the plugin's own. Its own `Shortcut`s and
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
  is on, unfolded with a tap on the row (or ⏎ there), as its tour is. A
  `ScrollList` of its own in it has the wheel and the touchpad first while
  it can still go their way, a swipe to its end, then the panel.
- `help`: its keys for the help (?), as `[["key", "what it does"], ...]`
  (a key as a sequence, "Shift+S", is shown as Pinhole names it: ⇧ S),
  listed under its name with its `Command`s that give a `section` (any
  but Pinhole's own parts: `"photo"`, `"share"`…). A few words each, as
  Pinhole's are: the heading says whose they are. Its actions with a
  `key` are there by themselves, each with its `helpText`, or its
  `text` without one, after what `help` lists, unless `help` says what
  that key does: list in `help` only keys of no action (a panel's), or
  what one key does in several places.
- `viewerLayer`: a `Component` drawn over a photo shown full window,
  inside it, so it is zoomed, panned, grown from its cell and clipped with
  the photo, as Live Photos plays its motion there. It is made afresh for
  each photo shown, given `photo` (its library position) and `shown`, which
  its root declares:

  ```qml
  viewerLayer: Item {
      property int photo
      property bool shown       // false the moment another photo shows or the viewer starts to close
      anchors.fill: parent
      // …stop what it plays on `shown` false
  }
  ```

  It goes once that photo is no longer shown. Photos passing quicker than
  anyone looks at them (an arrow held down, the strip scrubbed) get none:
  the one it stops on does. Layers stack in the plugins' order, above
  Pinhole's own picture and video. A layer that declares `loading` turns
  the arc beside the photo's date while it is true, as the photo still
  coming does. Live Photos (`plugins/live`) is one: its motion and
  effects play in its layer.
- `stacks`: photos shown in the grid as one card, each a list of photos
  (ids, or library positions), as Series does with each series. The card stands where the
  first one is, shows it and carries the mark for several photos; opening
  it opens that photo, selecting or dragging it takes them all, and the
  strip under a photo shown full window underlines a stack's photos. A
  photo in another plugin's stack already stays there. When the library is
  read again, each stack's photos stay together where they are now until
  it gives `stacks` again (on `library.changed()`, from the ids it keeps,
  as Series does).
- `hidden`: photos not in the grid at all, a list of photos (ids, or
  library positions),
  as Screenshots hides screenshots. They leave any stack they are in, the
  viewer's ← and →, its strip and Home/End pass over them, and Ctrl A and a
  range leave them out; a plugin can still `openPhoto` one. Say somewhere
  how many are away and how to have them back; with every photo hidden,
  the window says so, and names the plugin. Other plugins see what is
  hidden with `pinhole.isHidden`. When the library is read again, the
  photos it hid stay hidden where they are now until it gives `hidden`
  again (on `library.changed()`, from the ids it keeps).
- `marks`: marks in the bottom left corner of grid cells, beside
  Pinhole's own (a video's, a stack's) and its plugins' (Favourites' on a
  favourite, Live Photos' on a Live Photo),
  each a `PluginMark`, four at most (32 in all, Pinhole's among them; one
  past that is left out, and said under the plugin, as is one that is not
  right). Each has a switch of its own under the side panel's "In the
  grid", kept for this computer by its id, `<plugin id>/<name>`:

  ```qml
  marks: [
      PluginMark {
          name: "picked"            // unique in the plugin
          text: qsTr("Picked")      // its switch's words in the side panel
          icon: "\uf005"      // video, live, heart, stack, or one character (a Nerd Font glyph)
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
- `sources`: how the window shows the kinds of source the plugin brings,
  each a `PluginSource` of a kind its manifest's `sources` names (one of
  another kind is left out, and said under the plugin). iCloud and Folders
  are plugins made this way (`plugins/icloud`, `plugins/folder`); a source
  of your own is the same over its provider. Each function is given a
  `source`, one of `pinhole.sources.list` (`id`, `kind`, `plugin`, `name`,
  `count`, `thumbs`, and its kind's own fields: `account`, `path`, `key`…),
  with `shown` and `health` for the one shown:

  ```qml
  sources: [
      PluginSource {
          kind: "me.immich"
          icon: ""                       // a Nerd Font glyph, for its rows and offers
          noun: qsTr("Immich")           // before a source's count in its row: "Immich · 2,301"
          place: source => source.name   // the photos' place in a sentence: "Nothing in %1 yet"
          // What was found here that could be added, best first: the first
          // five of all the plugins' are offered on ⏎ and 2–5, on the first
          // run and in Add a source.
          offers: []                     // [{ title, detail, icon, triggered }]
          // The ways in that are always there, each a button there.
          ways: [{ title: qsTr("Add an Immich server…"), key: "M", triggered: () => plugin.ask() }]
          // A line in place of its count, in red, when it needs something.
          note: source => source.health === "needs_sign_in" ? qsTr("Sign in again") : ""
          // Its buttons beside Remove, each asked first on its row when `ask` is said.
          actions: source => [{ text: qsTr("Sign out"), key: "O", ask: qsTr("Sign out?"),
                                triggered: () => pinhole.sources.signOut(source.id) }]
          forgets: source => qsTr("Forget its index? The photos stay on the server.")  // what Remove asks
          adds: source => qsTr("Uploaded to Immich.")    // under "Add 3 files to %1" as files are dragged in
          pushed: qsTr("Immich says when something changes")  // for one that tells of its changes
          signIn: source => plugin.signIn(source)        // the card's Sign in, I, its row; null: never
      }
  ]
  ```

  Choosing an offer or a way leaves the rest to the plugin: add the source
  (`pinhole.sources.add`), or cover the window with a panel of its own to
  sign in (`covering`), as iCloud's does; the picker gives way to it.
- `tour`: a few steps that show what it does, on the photos and buttons as
  they are: `[{ title, text, photo, open, action, settings }, ...]`.
  `photo` is a library position (or a function giving one) the grid goes to
  and lights, `open` one shown full window, `action` one of its
  `PluginAction`s whose button is lit, and `settings: true` lights its part
  of the side panel. A tour with no step of `settings: true` ends in one
  Pinhole adds: its part of the side panel, where its tour can be
  shown again; give one of your own to say there what its settings are.
  Pinhole shows it by itself when the plugin is turned
  on in the side panel (once the panel closes, `tourReady` is true and the
  grid is free), and again from the side panel. Series' shows a series of
  your own. Plugins on at launch whose tour is not seen show none by
  themselves: a notice over the grid says how many are new, opens them in
  the side panel marked New (they then count as seen), and its × puts it
  off for three days, or until another is new.
- `holdSdr`: set true while drawing an SDR overlay over the
  viewer's photo without covering it (a `viewerLayer`'s, as Live Photos'
  motion), so the photo uses the same brightness range as the overlay.
  Pinhole restores the photo's wider range when no plugin holds it.
  Use this only while the overlay is visible. A plugin that is `covering`
  needs none: while one covers, Pinhole holds SDR itself.

`Pinhole.Plugin` also has Pinhole's own `ShellButton`, `Chip`, `UiText`,
`Spinner`, `Toggle`, `FieldInput` (a line to type into: `text`, `input`,
`placeholder`, `password`, `done`, `accepted()`), `Command` (a key,
with what it does in the help), the kit (above), and `Scroller`, what
`ScrollList`, `ScrollGrid` and `ScrollArea` are made of, and `theme` has the Omarchy theme's colours and type, so a plugin can look
like the rest.

One word for each thing across them: **`triggered()`** when it
is used (a `PluginAction`, a `Command`, a `ShellButton`, a `Panel` where no
control is), **`usable`** whether it can be now (an action, a button, a
`SettingSwitch`, a `Command`, `Choices`), **`shown`** whether it shows, **`key`** for one
key and **`keys`** for several (a `Command` takes either). Something that
says what it chose has its own: `Choices`' `chosen(value)`, a `Toggle`'s
`toggled()`, a `FieldInput`'s `accepted()`. A colour named from the theme
is a `colour` (`"accent"`, `"#rrggbb"`: an action's `textColour`, a mark's
`colour`); one of Qt's `color` values keeps Qt's word (`labelColor`).

These controls support Tab/Shift+Tab focus and Space/Enter activation.
For a button representing a selected option, set `ShellButton.checkable: true`
and bind `active` to its selected state; `Choices` does this for its options.
Give an unlabelled `Toggle` an explicit `Accessible.name` (for example,
`Accessible.name: qsTr("Show captions")`) so its purpose is available to
screen readers. `FieldInput` keeps normal text-editing keys inside the field.

A plugin that plays video uses `VideoView` as its `MediaPlayer`'s
`videoOutput`. It handles the video's colour and brightness automatically.
Use its `fillMode`, `contentRect` and `endOfStreamPolicy` as with Qt's
`VideoOutput`. `hdr` reports whether the current video is HDR, and
`framesDrawn` counts displayed frames. `plugin check` refuses `VideoOutput`
and `Video`; use `VideoView` instead.

```qml
MediaPlayer { source: url; videoOutput: video }
VideoView { id: video; anchors.fill: parent; fillMode: VideoView.PreserveAspectCrop }
```

Pinhole's keys are application shortcuts, and a field with the keys takes
letters, Space and the arrows first: a plugin's `TextInput` takes focus only
while it shows, and lets go (`focus = false`, or `enabled: false`) as its
panel closes. One hidden or turned off with the keys anyway gives them back
to the photos at the next key, never typed into.

### `pinhole`

`pinhole` is the plugin's way into Pinhole. A photo is named by its
position in the library (capture-date order, oldest first) or by its id;
positions move when the library is read again, ids do not, so keep ids.
The main grid can use Automatic or addition-date order independently of
this API. A library position is not a visual grid position.

| | |
|---|---|
| `library` | `count`; `item(photo)`: `index`, `id`, `date` (when it was taken: ms since 1970 UTC, an instant; for its hour, `new Date(date)` gives this computer's time zone's, not the one it was taken in), `filename`, `thumbnail` (a URL for an `Image`, in sRGB: `image://file/…`), `thumbPath` (the thumbnail's file, to hand to a program), `width`, `height` (the thumbnail's), `video`, `live`, `favorite`, `effect` (a Live Photo's effect as set on the iPhone: `loop`, `bounce`, `long_exposure`, or `""`), `avgColor` (the thumbnail's average colour, `"#rrggbb"`, or `""` before it has one), or `{}` if there is no such photo; `field(name)`: one of those for every photo at once, in the library's order (`field("date")`: all the dates), far quicker than `item` for each, or `field("day")`: the day each was taken in local time, as a number `yyyymmdd` (20250108), before 1970 too, 0 for one with no date (its `date` 0); `field("hidden")`: whether the grid hides each; `field(name, { shown: true })`: only those the grid shows, in their order, the hidden left out (a histogram of what shows, with `field("id", { shown: true })` beside it); `[]` for `index`, `thumbnail` or a name it has not (said on stderr, and refused by `plugin check`); `indexOf(id)`, -1 if none; `dir` (the source's library: `library.sqlite`, to read, and its thumbnails); `key` (which source's library it is, wherever it is kept: `icloud:me@example.com`, `folder:/home/me/Pictures`, `""` for one that does not say; for what a plugin keeps for each source apart); `keys()`, the key of every source added, as the settings say now (what a plugin keeps for one no longer there can go); `changed()` when it is read again; `settled(how)` once it is read again or what the grid hides changed, once for both when they come at once (a read moves what is hidden), with `how`: `{ read, hidden, switched, revision }`, `read` whether it was read again, `switched` whether that is another source's library (its `key` another: drop what you keep of the one before), `hidden` whether what the grid hides changed: listen to it alone, rather than to `changed` and `pinhole.hiddenChanged` both; and `revision`, which goes up whenever what `item` and `field` say may have changed (read again, or a favourite set), for a binding over them |
| `viewer` | the photo shown full window, read only: `open`, `index`, `photo` (it, as `library.item()` gives it, `{}` with none) and `id` (its id, `""` with none), `close()` (as Esc closes it); `hdr`, what makes it HDR, read from its file once it shows: `{ kind, headroom }`, `kind` `"apple"` or `"iso"` (a gain map: Apple's own, or ISO 21496-1's, which Android's and Adobe's Ultra HDR are too, ISO's when it has both), `"pq"` or `"hlg"` (its picture's own curve), and `headroom` how many stops its highlights go above SDR white; `{}` for SDR, a video, or until it is read |
| `display` | the display the window is on, as the compositor says: `hdr`, whether it can show anything brighter than SDR white; `headroom`, by how many stops; `wide`, whether its colours go beyond sRGB. On a display without HDR, `hdr` false and `headroom` 0. `pinhole display` prints what the compositor says of each display |
| `srgbProfile` | the path of an sRGB ICC profile installed with Pinhole: for a program to make a picture sRGB with, as Background and Theme give it to `magick -profile` so a wallpaper and a palette are of the colours the screen shows, not an iPhone's Display P3 codes taken as sRGB |
| `currentIndex()` | the photo under the pointer in the grid, else the middle one |
| `selection()`, `selectionCount` | the photos ticked in select mode, a card's all of them, as library positions |
| `selectionIds()` | the same by id: what to keep across a step that takes a while, as a reload moves positions |
| `reveal(photo)`, `openPhoto(photo)` | the grid to a photo, marked there with a ring that fades (a hidden one: the next shown, where it would be); a photo full window |
| `openPhotos(photos, photo, options)` | a photo full window from a plugin's view, without revealing or animating from the library grid (nor back to it as it closes), with `photos` (ids or positions, in the view's order: search results by relevance, a person's photos, a place's) as the only ones ← →, the strip, Home and End go through, until the viewer closes. `photo` must be one of them. `{ comeBack: true }`: its view comes back when the photo closes. Meanwhile `away` is true: it covers nothing, the viewer has the keys, and its view hides itself (`visible: shown && !pinhole.away`); then `returned(id)`, the id of the photo shown last, to put its cursor on. If another plugin went over the photos meanwhile, `dismissed()` instead: the way back leads to that one |
| `away`, `returned(photo)` | see `openPhotos`' `comeBack` |
| `keyboardInUse` | the keys are what was used last, not a pointer: when to show the ring on the one they are on, as Pinhole's own lists do |
| `workers`, `workerSaid(name, said)` | its manifest's workers as pinholed runs them, and what one said; see Workers |
| `input` | what was used last: `"keyboard"`, `"mouse"` or `"touchpad"`, to say "Pinch to zoom" or "Scroll to zoom" |
| `hasKeys` | whether the keys are the plugin's now: it covers the photos and no panel of Pinhole's is over it, for what takes keys or gestures of its own |
| `isHidden(photo)`, `hiddenCount`, `hiddenChanged()` | whether a photo is hidden from the grid by any plugin's `hidden`, how many are, and when that changes (after `library.changed()` when the library is read again): what a plugin counting or showing photos leaves out, as the grid does |
| `fetch(photo, kind, options)` | a photo's file from the service (`medium`, `full`, `original`, `video`, `live`, and a Live Photo's `loop`, `bounce` and `long_exposure`, which the plugin that makes them (Live Photos', What a plugin makes) makes when the source has none; and `jpeg`, its `full` as a JPEG for whatever reads no HEIC, with its date, place and camera and upright, made once into the cache and the `full` itself when that is a JPEG already, `not_found` for a video, as Pinhole's Export and Send use it; and a kind a plugin makes of its own, `"<its id>/<name>"`): a promise of its path. An `Image` shows a file in its right colours, sRGB whatever profile it carries (an iPhone's Display P3), as `"image://file/" + encodeURIComponent(path)`: as it would show the `file://` URL, at once or not as its `asynchronous` says, at the size its `sourceSize` and `fillMode` make. A `file://` URL shows its codes as if they were sRGB, an iPhone photo duller than it is. `full` and `original` are mostly HEIC, which an `Image` shows as `"image://photo/" + encodeURIComponent(path)`, upright, in sRGB, decoded on threads of their own at its `sourceSize` at most. `options`: `{ stream: true }` gives a URL to play in a `MediaPlayer` instead, the service's `http://` one while it is still downloading (it plays as it comes), else the file's |
| `fetchable` | the kinds `fetch` can ask for now: a source's (`medium`, `full`, `original`, `jpeg`, `video`, `live`, `loop`) and those the plugins on make (What a plugin makes): `bounce` and `long_exposure` while Live Photos is on, `"<id>/<name>"` while its plugin is. Ask for what can be had, not for a plugin: `shown: pinhole.fetchable.includes("bounce")` offers Background's Bounce only while something makes one |
| `store(name)` | what it keeps a value for each of many keys of, for the library shown (a file in its `cacheDir`, so each source's apart): `get(key)`, a promise of the value or `null`; `put(key, value)`, a value JSON can hold, `null` taking it out; `remove(key)`; each a promise. One key read or written at a time, never the whole, and two windows' writes both kept: for what is kept per photo, as Info keeps what it read of each original. It goes with the cache, so only what can be worked out again. `null` for a `name` that is not lowercase letters, digits, `_` and `-` (at most 32) |
| `ask(name, request, options)` | what its worker `name`, one whose manifest says `"answers": true`, answers to `request`: a promise of its result; `options`: `{ timeout }` in ms, 30 s unless said (Asking a worker) |
| `setFavorite(photos, favorite)` | photos (a list of positions or ids, or one) made favourites, or not, as the Favourites plugin's heart does: at once in the library (`item().favorite`, the grid's mark, `library.revision`), then in the source, iCloud's favourites or a folder's kept in its library. A promise of `{ changed }`, the ids of those that were not so already, once the source has them; rejected with the service's `code` (`read_only` for a library only looked at, `needs_sign_in`, `unavailable`, `unsupported`, `other_source` when another source is shown, `no_service`; `not_found` when none of them is in the library), the photos put back as they were. It makes no step to undo: record one with `did`. `cancel()` stops only the answer: the change, once asked, still goes through, so read `library.revision` rather than take `cancelled` for nothing changed |
| `run(program, args, options)` | a program, its own (in its folder) or any on PATH: a promise of `{ stdout, stderr, code }`, and `json`, its stdout read as JSON when it is. `options`: `input` (to its stdin), `json` (a value given as its stdin as JSON, in place of `input`), `timeout` (ms), `env` (added to Pinhole's), `output(line)` (each line it writes, as it comes). Its stdin ends after `input`, or at once without one, and its stdout is read as UTF-8 text: a binary file goes through a path instead. `cancel()` asks it to end (SIGTERM) and kills it a second later. `sqlite3`, a clipboard tool (`wl-copy`, `xclip`…) and `xdg-user-dir` are refused, by name, path or in a shell's line: rejected with `refused` (`pinhole.query`, `copyFile`, `picturePath`) |
| `record(item, options)` | an item as it looks over a number of frames, as a GIF or an MP4: a promise of the file's path. `options` are `path`, `format`, `width`, `height`, `fps`, `frames`, `seek(frame)` and `progress(fraction)` |
| `notify(text)` | a few words at the foot of the window for a while, as undo's: "Saved to Pictures" |
| `keepAwake(on)` | the screen kept on, no screensaver and no lock, while `on`, as the Photo frame does while it shows; `false` lets it go, and so does the plugin turned off or loaded again, and Pinhole quitting or crashing. Whether the screen is kept on now: not where it cannot be (not on Wayland, the window hidden, or on a workspace not shown), which it then is once it can |
| `warn(error)` | something gone wrong, under the plugin in the side panel and on stderr |
| `settings` | its settings, the user's choices (what its part of the side panel sets), kept in `~/.config/pinhole/plugins.json`: read one (`pinhole.settings.size`), bind to one, assign one and it is kept at once. Nothing else goes there (see below) |
| `fileActions` | what the plugins on do with a file: their `PluginAction`s of `place: "file"`, each with `takes(path)` and `trigger(path)` (Actions, above). Series offers them on the video it made |
| `synced` | what the user made of the library shown, which goes with it to every computer it is shown on: People's names, Series' series made by hand. As `state` is (read one, bind to one, assign one; `null` takes one out; `keys()`), each library's its own, and `values()` all of them at once, as an object; `changed(keys)` says which changed, here, in another window or on another computer, or that another library is shown (all of them). What is read is a copy. Two computers that set one key before they hear of each other agree on the later: a key is kept whole, so keep what is changed apart under keys of its own (People keeps each face under its own). See Settings, state and cache |
| `state` | what the user made with it, which must outlive Pinhole but is no setting, on this computer: Info's panel left open, the devices Send sent to. As `settings` is (read one, bind to one, assign one; `null` takes one out: it reads `undefined`, and `keys()`, the names it has, leaves it out), kept in `~/.local/state/pinhole/plugins/<id>/state.json` |
| `cache` | what it can work out again, kept to have it at once: what Screenshots found in each library, so they are hidden in the first frame. The same, kept in `~/.cache/pinhole/plugins/<id>/cache.json` |
| `did(name, undo, redo)` | a change just made, to undo with Ctrl Z and do again with Ctrl ⇧ Z: `name` says what in a word or two ("ungrouping"), `undo()` and `redo()` do it. Forgotten when the plugin is turned off or loaded again, and when another source's library is shown, however it came to be. It gives the step's number, for `dropStep` |
| `dropStep(step)` | a step it recorded (`did`'s number) that did not happen after all: what it changed was refused and put back, as a favourite the source would not set. Out of the history, so Ctrl Z undoes what was done before it, and no longer offered at the foot of the window. Record a step as the change is made, so Ctrl Z straight after works however slow the service is, and drop it if it fails |
| `adjustment(photo)`, `adjust(photos, adjustment)`, `turn(photos, quarters)` | what was done to a photo, never to its original: `{ turn, crop: { x, y, width, height }, kept }`, quarter turns clockwise and the part kept as fractions of the photo turned; `kept` is `"source"`, `"here"` or `""`. Where the source keeps edits (iCloud) `adjust` and `turn` write it there as its devices write theirs, the photo rendered from its original: an iPhone shows it and can revert it, though it opens no edit made elsewhere to change it (docs/api-findings.md, Edits); edits made on an iPhone read back the same way. Where it keeps none (a folder), Pinhole keeps it (`~/.local/state/pinhole/adjustments/`) and draws it so: the grid, the viewer, an export as JPEG, a copy. Either way a promise of `{ photos: [{ id, turn, crop }], kept }`; `{ turn: 0, crop: null }` takes it back; `turn` turns from where each is, its crop turning with it. `library.item()` says the size drawn; `thumbnailAsIs` is the thumbnail as its file is. No step to undo is made: record one with `did`, as Turn and crop does |
| `readText(path)`, `readJson(path)` | a file of its own read whole, away from the window: one in its folder (`dir`) or its cache (`cacheDir`, what its worker writes). A promise of its text, or of what its JSON says; rejected `not_found`, `not_yours` (a file elsewhere), `too_large` (over 32 MB), `unreadable` or `json` |
| `copyText(text)` | words on the clipboard as text (paths, a link): a promise of them once it has them. As with `copyFile`, a self-test's clipboard is its window's own |
| `copyFile(path)` | a file on the clipboard, an image as one a chat takes pasted too (a JPEG as it is, a GIF as it is, and a PNG of at most 2560 px, a GIF's first frame), as Pinhole's own copy has it: a promise of the path once the clipboard has it, which for a big picture is a moment later (its PNG is made first, away from the window), so say "Copied" then, not before; `not_found` if there is no such file. The last copy asked for wins: if another (a plugin's or Pinhole's own) is asked for while its picture is made, this one is never set, and is `cancelled` |
| `showFile(path)` | its folder in the file manager |
| `deliver(path)` | a file it made handed over, as Series' and Sheet's are: on the clipboard (`copyFile`), kept where it is. A promise of how it went, never rejected: `"copied"`, `"kept"` (another copy asked for since has the clipboard) or `"failed"` (said, as `warn` says it) |
| `has(program)` | whether a program is there to run: a name on PATH (`"localsend"`), or one of its folder's by a relative path; for one it can do without, as Send's LocalSend (what it cannot do without goes in the manifest's `requires`) |
| `picturesDir()`, `exists(path)`, `fileSize(path)`, `sizeText(bytes, decimals)` | `picturesDir()`: the desktop's Pictures folder (`~/Pictures`), for a folder of one's own in it (`picturesDir() + "/Exports"`); in a self-test, the scratch Pictures, never the person's. `sizeText`: "1.8 MB", with one decimal unless said |
| `picturePath(name, extension)` | a file to make in Pictures/Pinhole, the folder made: `name.extension`, or `name 2.extension`… if that is taken. In a self-test, the scratch Pictures, never the person's |
| `query(sql, values)` | the library's index (`library.sqlite`; schema below) asked, read only, away from the window: a promise of rows by column name, with `values` bound to its `?`s. One read-only statement, bounded time and results; `cancel()` stops execution, including before the first row. Rejected `sql`, `no_library`, `query_timeout`, `query_limit`, `query_busy`, `cancelled` (the plugin's own `cancel()`) or `reloaded` (the library read again meanwhile: ask again); see Bounded library queries below |
| `screenshots()` | the ids of the photos the library says are screenshots, as Screenshots hides them: the index's `subtype` (iCloud's mark for an iPhone's or an iPad's), a name screenshots are given (screenshot-…, Screenshot …, Zrzut ekranu …, hyprshot's, grim's…), or a PNG with no place shaped like a phone's screen. A promise, a query's (bounded and cancellable the same) |
| `chooseFolder(title)` | a folder chosen in the desktop's own picker: a promise of its path, rejected `cancelled` when none is |
| `sources` | the sources: `list` (every source added whose kind is on, as `PluginSource` is given them), `current` (the id of the one shown), `health` (how the one shown is doing: `fine`, `needs_sign_in`, `offline`, `failing`, or `no_service` when no service runs), `progress` and `rounds` (a library being built; when the one shown was last and is next looked at), `discovered` (what could be added of its kinds, by kind, as the service last found: iCloud's `{ icloudpd: […] }`, a folder's `{ folders: […] }`, a provider's whatever its `discover` says) and `discover()` to look again, `show(id)`; and, only of the kinds its manifest names, else rejected `not_yours`: `add(kind, entry)` (a promise of the source added and shown), `signIn(kind, params)` and `submitCode(kind, params)` (promises of `{ state }`, `signed_in` once added and shown or `needs_code`; rejected with the service's or the provider's code: `wrong_password`, `wrong_code`, `turned_off`…), `signOut(id)` (a promise of the sources left) |
| `dir`, `cacheDir` | its own folder; a folder for files it can make again, one to each source's library (`cacheDirChanged()` when another source's is shown), beside its `cache` |
| `coveredBy` | the plugin over the photos now, its id, `""` for none (see `covering`) |
| `dismissed()` | the plugin over this one, covering, handed a photo over (`openPhoto`, `reveal`): close, and do not come back when that photo closes |
| `id`, `api` | its id; the version of all this |

### Read-only SQL schema

Use `query()` for aggregates or bounded pages, and `library.field()` when
one in-memory column is enough. The public catalog tables are:

| Table | Columns |
|---|---|
| `assets` | `id` (stable TEXT primary key), `filename`, `item_type`, `asset_date`, `added_date`, `favorite`, `has_location`, `latitude`, `longitude`, `is_video`, `is_live`, `duration`, `orientation`, `live_effect`, `size`, `thumb_path`, `thumb_w`, `thumb_h`, `avg_color`, `camera_make`, `camera_model`, `lens_model`, `camera_read`, `edit`, `subtype` |
| `renditions` | `asset_id` (references `assets.id`), `kind`, `name`, `key`, `width`, `height`, `size`, `file_type`; primary key `(asset_id, kind)` |
| `meta` | `key`, `value`: `source` is the library's source key (as `library.key`), which a worker checks against `--library-key` |

Dates are milliseconds since 1970, sizes are bytes, and flags are integers
0/1. Optional metadata can be SQL `NULL`; `asset_date` is capture time and
`added_date` is addition time. `live_effect` is `loop`, `bounce`,
`long_exposure` or `NULL`. Thumbnail paths are relative to `library.dir`.
A rendition describes a file; fetch it through `pinhole.fetch`, rather than
constructing paths into Pinhole's cache. SQL row order is unspecified until
you use `ORDER BY`; the grid's visual ordering is independent of this API.

Inspect the installed schema with `query("PRAGMA table_info(assets)")` or
`query("PRAGMA table_info(renditions)")` before using an optional column.
Catalog schema 2 adds nullable `latitude` and `longitude`: a pair of signed
WGS 84 decimal degrees. `has_location` can be true without a readable pair.

Catalog schema 5 adds `subtype`: what kind of picture the source says it
is, `screenshot` (iCloud's mark of an iPhone's or an iPad's) or NULL.

Catalog schema 4 adds `edit`: what a source that keeps edits (iCloud)
says was done to the photo, `turn,x,y,width,height` as `adjustment()`
gives it; its renditions are of the photo so already.

Catalog schema 3 adds the camera, as the photo's own EXIF says:
`camera_make` ("Apple"), `camera_model` ("iPhone 15 Pro") and `lens_model`,
each NULL when it does not say. pinholed reads them from the file of a
photo that is on this computer: a folder's every photo, and of iCloud's
those whose original has been fetched (opened full size, or its Info), a
few thousand each round. `camera_read` is 1 once it has been read, found
or not; 0 until then. Group by camera without fetching any originals:

```js
pinhole.query("SELECT camera_model AS model, count(*) AS n FROM assets " +
              "WHERE camera_model IS NOT NULL GROUP BY 1 ORDER BY n DESC")
```
For a map, page `SELECT id, latitude, longitude FROM assets WHERE latitude IS NOT NULL AND id > ? ORDER BY id LIMIT ?`
using the last id from the previous page (omit the cursor condition on the
first page). Keep locations and thumbnails local; loading a base map only
needs tiles for its visible area.
`query("PRAGMA user_version")` reads its schema version. Source-specific
tables (`icloud_assets`…) are private to their provider: they change with
it and hold what is not a plugin's, so a query that reads one, or its
`PRAGMA table_info`, is refused `sql`. Never write the database or change
its connection settings from a UI plugin.

### Bounded library queries

`query(sql, values)`: SQL `NULL`
keeps the historical Qt SQL mapping: an empty JavaScript string, rather
than `null` or `undefined`. Use SQL `IS NULL` to test missing values before
returning them when this distinction matters. Empty text and empty blobs
retain their respective string/byte-array types.

Bound values accept null, booleans, numbers, strings and byte arrays.
Serialize structured values explicitly, for example with `JSON.stringify`.
Its safety limits apply to every plugin, including those with older manifests:

- **Two seconds total**, including worker queue time, SQLite lock waits,
  preparation and execution. The timeout interrupts SQLite computation,
  not just its wait for a database lock.
- At most **100,000 rows** and **32 MiB** of accounted result memory, including
  conservative per-row/column overhead. A cell is at most **1 MiB**. A
  failure returns no partial rows. Select the columns you need, and use
  SQL aggregation or paginated queries for larger collections.
- At most **two outstanding queries per plugin**, eight across Pinhole,
  with two dedicated workers. Queries cannot occupy photo decoding's worker
  pool. `query_busy` asks the caller to wait for an existing request.
- SQL is at most **64 KiB**, 64 result columns and 256 bound values.
  `LIKE`/`GLOB` patterns are at most **1,024 bytes**: these functions can do
  expensive work inside one SQLite opcode, beyond the progress callback.
  Oversized patterns fail with `sql` and SQLite's diagnostic. Only
  one read-only statement is accepted; trailing comments are fine. Writes,
  `ATTACH`, extension loading and configuration-changing pragmas are refused.
  Read-only schema introspection, such as `PRAGMA table_info(assets)`, works.

An error has a stable `code`: `sql` carries SQLite's diagnostic,
`no_library` means there is no current index, `query_timeout` exceeded the
time budget, and `query_limit` exceeded the row/data budget. Structural SQL
limits can also produce `sql`. `query_busy` means two of the plugin's own
(or eight in all) are already running. `cancelled` means the plugin
cancelled the promise; `reloaded`, that the library was read again while
it ran. Turning a plugin off or
reloading its code stops computation and discards its old callbacks.
Cancellation is checked inside SQLite and while waiting for locks, even
for an aggregate that has not produced its first row.

Keep the promise when opening a view and cancel it when closing.
`.catch(pinhole.warn)` says neither `cancelled` nor `reloaded`: they are
no faults. On `reloaded` the library was read again, so ask again (on
`library.changed()`). For paging, use a stable id and a bounded
`LIMIT` (for example, `WHERE id > ? ORDER BY id LIMIT 1000`) rather than
fetching the entire library into JavaScript. The working Places example
aggregates in SQL, so it returns at most four rows regardless of library size.

### Settings, state and cache

Keep each thing in the one it is:

- `settings`: what the user chose, the switches and choices in its part of
  the side panel. `plugins.json` is theirs, and pinholed reads it again
  each time it changes, so a plugin writes there only when the user
  changes a setting. Its defaults are its manifest's, what
  `pinhole.settings` says of one never chosen, from the first binding on:

  ```json
  "settings": { "shuffle": true, "seconds": 8, "photos": "favourites" }
  ```

  A `SettingSwitch` or `Choices` given a `setting` shows that one and
  keeps what is chosen, with nothing more to write:

  ```qml
  settings: Component {
      Column {
          SettingSwitch { setting: "shuffle"; text: qsTr("Shuffle") }
          Choices { setting: "photos"; options: [{ value: "favourites", text: qsTr("Favourites") },
                                                 { value: "all", text: qsTr("All") }] }
      }
  }
  ```

  Setting one to `null` takes the choice back: its default again.
- `synced`: what the user made of the library and would miss on
  another computer: names given, series made by hand. It is the library's,
  so another library shown has its own, with nothing to put in its keys.
  Each computer has it at once and keeps it in
  `~/.local/state/pinhole/synced/<kind>-<name>/<id>.json`, each key with
  when and on which computer it was set; pinholed carries it between the
  computers the library is shown on, as its source can: an iCloud library
  in a `Pinhole` folder of that account's iCloud Drive, one file to each
  computer. A folder's library keeps it on this computer. Two computers
  that change one key before they hear of each other agree on the later,
  so a key is the most that can be lost to another computer's change:
  keep each thing the user changes on its own under a key of its own
  (People: one to a face), not all of them under one.
- `state`: what the user made and would miss, on this computer: a panel
  left open, the devices they sent to.
- `cache`: what can be worked out again, and is kept only to be quick; it
  may be gone (`rm -r ~/.cache/pinhole`), and the plugin works it out again.

What is assigned is the plugin's at once. A setting is written to
`plugins.json` straight away, as the user changes it: other windows and
pinholed follow that file, and each change is written over what is there
now, so no window undoes another's. State and cache follow soon after,
away from the window, in one step (written beside it, then renamed over
it), and only if something changed, so assigning the same again costs
nothing; several changes in a moment are one write. What could not be
written is said under the plugin in the side panel. Each is read whole the
first time the plugin uses it, before the window's first frame if it does
so as it loads, so keep them small: ids, not pictures (files go in
`cacheDir`). What is kept in `state` or `cache` for each source's library
goes under a key with its `library.key`, as Screenshots' `found:…`; Screenshots keeps none for a library whose `key`
is `""`, nor a list past 5,000, and lets go of those of sources no longer
added.

`synced` is written as `state` is, soon after and in one step, merged into
its file under the folder's lock, so what pinholed brought from another
computer meanwhile stays; a file changed elsewhere is read again, and
`changed` says what it changed. A file that cannot be read is never
written over: what was set is said under the plugin in the side panel.

### Promises

What takes a while (`fetch`, `query`, `run`, `record`, `copyFile`) gives a
promise, which also has `cancel()` (`copyFile`'s does nothing). So does
every promise made from it with `then` and `catch`, and cancelling any of
them stops the whole chain: the first job and any Pinhole job a step
returned, so keeping `pinhole.run(…).then(…).then(…)` to cancel later is
fine:

```js
const steadying = pinhole.run("pinhole-series", ["steady", dir].concat(ids), { timeout: 30000 })
steadying.then(result => use(result.stdout)).catch(pinhole.warn)
// …another series shown in the meantime:
steadying.cancel()
```

A promise is rejected with an `Error` whose `code` says why:

| | |
|---|---|
| `fetch` | the service's: `not_found` (no such photo in this library), `offline`, … |
| `copyFile` | `not_found` (no such file), `cancelled` (another copy supersedes it while its image is being prepared) |
| `run` | `exit` (it ran and did not exit 0: `exitCode`, `stdout`, `stderr` too, and the message is what it wrote to stderr), `start` (it could not be started), `timeout`, `cancelled` |
| `record` | `failed` (ffmpeg, or the item gone), `cancelled` |
| `query` | `sql`, `no_library`, `query_timeout`, `query_limit`, `query_busy`, `cancelled`, `reloaded` |
| any | `refused` (`run` of a program Pinhole does the work of, with what to use instead) |

`pinhole.warn` says nothing of `cancelled` (the plugin's own `cancel()`,
or a newer copy) and `reloaded` (the library read again during a query),
so a chain needs no test for them before its `.catch(pinhole.warn)`.

End a chain with `.catch(pinhole.warn)`: what a `then` throws is otherwise
said nowhere. There is no `finally` in Qt's JavaScript; a `.then()` after
the `.catch()` does the same. Nor is there `async`/`await` in QML.

Everything a plugin started ends with it: turned off or loaded again, its
programs are stopped (asked to end with SIGTERM, killed a second later; so
too when the window closes), its recordings dropped, and no promise of its
old self is answered.

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
only turn on one whose code you would run yourself. Under the switch of one
of your own that brings a source, the side panel says so: "It reads your
photos, and the accounts you sign in to with it. Turn it on only if you
trust it." A copy of iCloud's plugin in your folder (the same id) replaces
its QML only, never pinholed's Rust, and its row says: "Your own copy of
iCloud: it sees the password you type." (a copy of one whose sources never
sign in, Folders', that it reads the photos of every source shown with
it). It is off, iCloud's sources with it, until you turn it on (its switch is
its own, `copy:pinhole.icloud` in `plugins.json`: the built-in's, turned
off and on once, is not it): the middle
of the window then says so, and its button there opens the side panel at
its switch rather than turning it on.

A plugin's code is QML and JavaScript, and programs of its own that run
apart (`pinhole.run`, Workers): none loads native code into the window.
Pinhole's own plugins alone may bring a native QML module, for what QML
cannot do fast enough (Info's map, `Pinhole.Internal.Info`): its source in
the plugin's `module/`, built with the window against its Qt into the
plugin's `qml/`, which the window looks in only in Pinhole's own plugins
folder. A copy of Info in your folder imports the module installed with
Pinhole, as any `Pinhole.Internal` import of a `pinhole.…` id does; one of
another id cannot import it at all.

What every plugin can see of the sources follows from that. Reading
`pinhole.sources` is open to every plugin, whatever kinds its manifest
names: `list` (each source's `name`, an Apple ID's account or a folder's
`path`, its count), `current`, `health`, `progress`, `rounds` and
`discovered` (the icloudpd sign-ins and the folders found), `discover()`,
and `show(id)`, which switches the source the window shows. Only what adds,
signs in to or out of a source is kept to the kinds the manifest names.
