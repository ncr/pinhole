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
every QML file it uses and the JavaScript they import, its translations,
and the footguns (What check looks for, below). It takes a folder too
(`pinhole plugin check .`), and exits 1 if anything is wrong, for an
editor or a script to run on save. A manifest's `sources`
are checked as pinholed takes them (a provider's kind namespaced by the
plugin's id, its program in the folder and runnable, `accepts` and
`rounds_every_secs` as [source-providers.md](source-providers.md) says),
so what pinholed would refuse is said here, and under the plugin in the
side panel. It refuses anything the plugin uses of an API newer than its
manifest asks for, saying the number to give, and warns of a file named as
one of another plugin's or of Pinhole's own (see Translations).

## The kit

What Pinhole's own plugins once wrote by hand, and got wrong, is in
`Pinhole.Plugin`, right by default:

- `ScrollList`, `ScrollGrid`, `ScrollArea`: a `ListView`, a `GridView` and
  a `Flickable` (anything larger than its room: give it a `contentWidth`
  and a `contentHeight`), each scrolled by the wheel and the touchpad
  exactly as the grid is: the same physics, its inertia, its springs at
  the ends and its wheel's glide, measured against the grid's own by a
  self-test (`PINHOLE_SELFTEST=physics`). Up and down, and across where the
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
- `Panel`: a screen over the photos, its parent's whole room in the theme's
  background (or a `color`), taking every click, touch, wheel, swipe, pinch
  and hover, so nothing reaches the photos; `clicked()` where no control is,
  and with `steps` (`Qt.Horizontal`, `Qt.Vertical`) the wheel and a swipe
  step: `stepped(1)` the next, `stepped(-1)` the one before (a notch a
  step, a swipe one), as the Photo frame and the Series editor step.
- `Card`: a box in the theme, the same: nothing over it goes through. Over
  the photos for a moment, on a dimmed `Panel` whose click puts it away.
- `Choices`: one of a few on a row of buttons, each also its number's key:
  `options` as `[{ value, text }]`, `value`, `chosen(value)`.
- `ShellButton`'s `complain(text)`: what pressing it could not do, under it
  for four seconds, the button shaking, as a `PluginAction`'s.
- `pinhole.picturePath(name, extension)` and `pinhole.copyFile(path)`: a
  file to make in Pictures/Pinhole, named as Pinhole names what it makes
  (`name 2.png` when `name.png` is there), and a file on the clipboard. In a
  self-test both are a scratch folder's and the window's own: never the
  person's Pictures or clipboard.
- `pinhole.query(sql, values)`: the library's index asked, read only, away
  from the window, waiting while the service writes it: a promise of its
  rows. Never `sqlite3` on the file, which opens it while the service
  writes ("database is locked"). `pinhole.library.field(name)` has a field
  of every photo at once, `"day"` among them.
- `settings`, `state` and `cache` (below): the user's choices, what they
  made, what can be worked out again.

A plugin covers the photos only while its `covering` is true, under
Pinhole's own panels and over the other plugins, with the `"plugin"` keys
its own: the template's `covering: open` is all it takes.

Four examples, each a whole `Main.qml` (beside a manifest asking for
`"api": 5`), are in `app/tests/examples` and run by a self-test
(`PINHOLE_SELFTEST=examples`):

A screen of its own with a `ScrollList`, a count in its words (`%n`):

```qml
import QtQuick
import Pinhole.Plugin

// F5: how many photos each year has, on a screen of its own.
PinholePlugin {
    id: plugin
    property bool open: false
    property var years: []
    covering: open
    function count() {
        const counts = {}
        for (const day of pinhole.library.field("day"))
            if (day > 0)
                counts[Math.floor(day / 10000)] = (counts[Math.floor(day / 10000)] ?? 0) + 1
        years = Object.keys(counts).reverse().map(year => ({ year: year, count: counts[year] }))
    }
    actions: [PluginAction { place: "grid"; text: qsTr("Years"); key: "F5"; onTriggered: { plugin.count(); plugin.open = true } }]
    Command { name: "years.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    Panel {
        visible: plugin.open
        ScrollList {
            anchors.fill: parent
            anchors.margins: 32
            model: plugin.years
            delegate: UiText {
                required property var modelData
                height: 40
                text: modelData.year + " · " + qsTr("%n photo(s)", "", modelData.count)
            }
        }
    }
}
```

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
    Command { name: "tiles.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }
    Command { name: "tiles.in"; keys: ["+", "="]; keyLayer: "plugin"; when: plugin.open; onTriggered: zoom.zoomBy(1) }
    Command { name: "tiles.out"; keys: ["-"]; keyLayer: "plugin"; when: plugin.open; onTriggered: zoom.zoomBy(-1) }

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
                    model: Math.floor(parent.width / zoom.size) * Math.floor(parent.height / zoom.size)
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

A `Card` on a dimmed `Panel`, with `Choices`, making a file with
`picturePath` and putting it on the clipboard:

```qml
import QtQuick
import Pinhole.Plugin

// F7: a card over the photos asking how big a swatch of the colour of the
// photo under the pointer to make: 1, 2, 3 or a click; Esc, or a click
// beside it, puts it away. The swatch is kept in Pictures/Pinhole and put
// on the clipboard.
PinholePlugin {
    id: plugin
    property bool open: false
    covering: open
    actions: [PluginAction { id: action; place: "grid"; text: qsTr("Swatch"); key: "F7"; onTriggered: plugin.open = true }]
    Command { name: "ask.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    function make(size) {
        open = false
        const photo = pinhole.library.item(pinhole.currentIndex())
        const path = pinhole.picturePath(photo.filename.replace(/\.[^.]*$/, "") + " " + size, "png")
        pinhole.run("magick", ["-size", size + "x" + size, "xc:" + photo.avgColor, path])
            .then(() => pinhole.copyFile(path))
            .then(() => pinhole.notify(qsTr("Copied, and kept in Pictures")), error => {
                action.complain(qsTr("Could not make it"))
                pinhole.warn(error)
            })
    }

    Panel { visible: plugin.open; color: Qt.rgba(0, 0, 0, 0.45); onClicked: plugin.open = false }
    Card {
        visible: plugin.open
        anchors.centerIn: parent
        width: sizes.width + 48
        height: sizes.height + 48
        Choices {
            id: sizes
            anchors.centerIn: parent
            options: [{ value: 64, text: qsTr("Small") }, { value: 256, text: qsTr("Medium") }, { value: 1024, text: qsTr("Large") }]
            when: plugin.open
            helpText: qsTr("Size")
            onChosen: size => plugin.make(size)
        }
    }
}
```

The index asked with `pinhole.query`, the answer in a `ScrollArea`:

```qml
import QtQuick
import Pinhole.Plugin

// F8: the library's files, by how many are videos and how many know where
// they were taken, asked of its index (docs/sources.md), in a ScrollArea
// wider and taller than the window.
PinholePlugin {
    id: plugin
    property bool open: false
    property string report: ""
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Places"); key: "F8"; onTriggered: plugin.ask() }]
    Command { name: "places.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    function ask() {
        open = true
        pinhole.query("SELECT is_video, has_location, count(*) AS n FROM assets GROUP BY is_video, has_location")
            .then(rows => report = rows.map(r => (r.is_video ? "video" : "photo") + (r.has_location ? ", placed: " : ": ") + r.n).join("\n"))
            .catch(pinhole.warn)
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
- a `PinchHandler`, `PinchArea` or `MultiPointTouchArea` (`PinchZoom`), a
  `WheelHandler`, and a `MouseArea`'s `onWheel` (a `Panel` or `Card` under
  the controls, a `Panel`'s `steps`): a pinch went through to the photos,
  and a `MouseArea` without `onWheel` lets the wheel through; so is one
  over the whole window, a `MouseArea` filling the plugin's root (or an
  `Item` filling it): what a plugin shows over the photos goes in a
  `Panel`;
- `Qt.createQmlObject`: QML made from a string, which check cannot look
  over (a file of the plugin's, with a `Loader` or `Qt.createComponent`);
- a `VideoOutput` or QtMultimedia's `Video` (`VideoView`): they show HDR
  video wrong;
- `sqlite3` (`pinhole.query`), named anywhere in a string: by its path,
  in a shell's line (`sh -c "…"`), put together (`"sqlite" + "3"`);
- `wl-copy`, `xclip` and the like (`pinhole.copyFile`), and Pictures found
  by hand, `~/Pictures`, a `/Pictures` to build a path with, or
  `xdg-user-dir` (`pinhole.picturePath`), in a string or a shell's line: a
  self-test of them wrote to the real Pictures and the session's
  clipboard. The words a plugin shows, in `qsTr()`, are not looked at
  ("Kept in ~/Pictures/Pinhole" is fine). `pinhole.run` refuses the same
  programs as it runs them, however their names were made: by name or
  path, after `env` and the like, or in a shell's line (`sh -c`), with an
  error `refused` saying what to use;
- an API newer than the manifest asks for: a Pinhole of that API would load
  it and fail where nobody sees. `api` stays asked for by hand, since it
  is what an older Pinhole reads to say it needs a newer one; check says
  the number to give.

And warns of:

- a setting written in a handler that runs often (`onTriggered`,
  `on…Changed`) or from a list or the library: settings are the user's
  choices, and `plugins.json` is written again each time (`state`,
  `cache`);
- a key named as Qt writes it in what is shown, `Shift+S` or `Return`, not
  as the buttons name it (⇧ S, ⏎);
- words shown as they are, in `text:`, `notify(…)` or the `help` without
  `qsTr()`;
- words in `qsTr()` with no Polish in `i18n/pl.json` (nor Pinhole's own).

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
it. `pinhole plugin check` refuses one that asks for less than it uses. `pinhole.api` says which one it is running in. API 2 brought promises
(`copyFile`'s too), `pinhole.settings`, `notify`, `warn`, photos by id,
`hidden`, `avgColor`, `library.field`, `isHidden`, `run`'s `output`,
`pinhole.coveredBy`, `dismissed` and `keepAwake`. API 3 brought `marks`,
`pinhole.state`, `pinhole.cache`, `viewerLayer`, an action's `order`,
`hint`, `busyDelay`, `textColour` and `complain`, `fetch`'s options, its
Live effects and `jpeg`, a photo's `effect`, `setFavorite`,
`library.revision`, `sources` in the manifest: a source of photos of
the plugin's own, whose provider pinholed runs
([source-providers.md](source-providers.md)), and what the window shows of
one: the root's `sources` (`PluginSource`), `pinhole.sources`,
`chooseFolder` and `sizeText`'s decimals. API 4 brought photos in their
right colours and HDR shown right in SDR ([hdr.md](hdr.md)): `image://file`,
a photo's `thumbPath` now that its `thumbnail` is a URL of that, not of its
file, `pinhole.display`, the viewer's `hdr`, `pinhole.srgbProfile` and
`SdrVideo`; and `Scroller`. API 5 brought the kit: `ScrollList`,
`ScrollGrid`, `ScrollArea`, `PinchZoom`, `Panel`, `Card`, `Choices`, a
`ShellButton`'s `complain`, `pinhole.query`, `pinhole.picturePath` and
`VideoView`; and
`Scroller` scrolling as the grid does, across too.

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
    help: [["F2", qsTr("Count favourites")]]
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
  `text`, `key`, `shown`, `usable`, `active` (lit, filled with the
  accent), `textColour` (its words in a colour while not lit: a theme
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
  is on, unfolded with a tap on the row (or ⏎ there), as its tour is. A
  `ScrollList` of its own in it has the wheel and the touchpad first while
  it can still go their way, a swipe to its end, then the panel.
- `help`: its keys for the help (?), as `[["key", "what it does"], ...]`
  (a key as a sequence, "Shift+S", is shown as Pinhole names it: ⇧ S),
  listed under its name with its `Command`s that give a `section` (any
  but Pinhole's own parts: `"photo"`, `"share"`…). A few words each, as
  Pinhole's are: the heading says whose they are.
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
`Spinner`, `Toggle`, `FieldInput` (a line to type into: `text`, `input`,
`placeholder`, `password`, `done`, `accepted()`), `Command` (a key,
with what it does in the help), the kit (above), and `Scroller`, what
`ScrollList`, `ScrollGrid` and `ScrollArea` are made of (API 4), and `theme` has the Omarchy theme's colours and type, so a plugin can look
like the rest.

A plugin that plays video shows it with a `VideoView` (API 5), the
viewer's own, as a `MediaPlayer`'s `videoOutput`: an HDR video (an
iPhone's HLG or Dolby Vision, PQ) is tone mapped on the GPU as it is
drawn, so it shows right in SDR, where Qt Multimedia's `VideoOutput` (and
its `Video`) shows one flat and grey or, decoded by the hardware, with its
brightest colours broken ([hdr.md](hdr.md), H2). Any other video is drawn
as a `VideoOutput` draws it. It has a `VideoOutput`'s `fillMode`,
`contentRect` and `endOfStreamPolicy`; `hdr` says whether the frames are
HDR, and `framesDrawn` counts the frames it has drawn. `plugin check`
refuses a `VideoOutput` and a `Video`. (API 4's `SdrVideo` between a
player and a `VideoOutput` made HDR SDR on the CPU, seven cores for a 4K
clip; it still works, and hands a `VideoView` its frames untouched.)

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
position in the library (oldest first, as the grid has them) or by its id;
positions move when the library is read again, ids do not, so keep ids.

| | |
|---|---|
| `library` | `count`; `item(photo)`: `index`, `id`, `date` (ms), `filename`, `thumbnail` (a URL for an `Image`, in sRGB: `image://file/…`), `thumbPath` (the thumbnail's file, to hand to a program; API 4), `width`, `height` (the thumbnail's), `video`, `live`, `favorite`, `effect` (a
Live Photo's effect as set on the iPhone: `loop`, `bounce`,
`long_exposure`, or `""`), `avgColor` (the thumbnail's average colour, `"#rrggbb"`, or `""` before it has one), or `{}` if there is no such photo; `field(name)`: one of those for every photo at once, in the library's order (`field("date")`: all the dates), far quicker than `item` for each, or `field("day")`: the day each was taken in local time, as a number `yyyymmdd` (20250108), before 1970 too, 0 for one with no date (its `date` 0); `[]` for `index`, `thumbnail` or a name it has not; `indexOf(id)`, -1 if none; `dir` (the source's library: `library.sqlite`, to read, and its thumbnails); `key` (which source's library it is, wherever it is kept: `icloud:me@example.com`, `folder:/home/me/Pictures`, `""` for one that does not say; for what a plugin keeps for each source apart); `keys()`, the key of every source added, as the settings say now (what a plugin keeps for one no longer there can go); `changed()` when it is read again; and `revision`, which goes up whenever what `item` and `field` say may have changed (read again, or a favourite set), for a binding over them |
| `viewer` | the photo shown full window: `open`, `index`; `hdr` (API 4), what makes it HDR, read from its file once it shows: `{ kind, headroom }`, `kind` `"apple"` or `"iso"` (a gain map: Apple's own, or ISO 21496-1's, which Android's and Adobe's Ultra HDR are too, ISO's when it has both), `"pq"` or `"hlg"` (its picture's own curve), and `headroom` how many stops its highlights go above SDR white; `{}` for SDR, a video, or until it is read |
| `display` | the display the window is on, as the compositor says (API 4): `hdr`, whether it can show anything brighter than SDR white; `headroom`, by how many stops; `wide`, whether its colours go beyond sRGB. On a display without HDR, `hdr` false and `headroom` 0. `pinhole display` prints what the compositor says of each display |
| `srgbProfile` | the path of an sRGB ICC profile installed with Pinhole (API 4): for a program to make a picture sRGB with, as Background and Theme give it to `magick -profile` so a wallpaper and a palette are of the colours the screen shows, not an iPhone's Display P3 codes taken as sRGB |
| `currentIndex()` | the photo under the pointer in the grid, else the middle one |
| `selection()`, `selectionCount` | the photos ticked in select mode, a card's all of them |
| `reveal(photo)`, `openPhoto(photo)` | the grid to a photo, marked there with a ring that fades (a hidden one: the next shown, where it would be); a photo full window |
| `isHidden(photo)`, `hiddenCount`, `hiddenChanged()` | whether a photo is hidden from the grid by any plugin's `hidden`, how many are, and when that changes (after `library.changed()` when the library is read again): what a plugin counting or showing photos leaves out, as the grid does |
| `fetch(photo, kind, options)` | a photo's file from the service (`medium`, `full`, `original`, `video`, `live`, and a Live Photo's `loop`, `bounce` and `long_exposure`, which the service makes when the source has none; and `jpeg`, its `full` as a JPEG for whatever reads no HEIC, with its date, place and camera and upright, made once into the cache and the `full` itself when that is a JPEG already, `not_found` for a video, as Pinhole's Export and Send use it): a promise of its path. An `Image` shows a file in its right colours, sRGB whatever profile it carries (an iPhone's Display P3), as `"image://file/" + encodeURIComponent(path)` (API 4): as it would show the `file://` URL, at once or not as its `asynchronous` says, at the size its `sourceSize` and `fillMode` make. A `file://` URL shows its codes as if they were sRGB, an iPhone photo duller than it is. `full` and `original` are mostly HEIC, which an `Image` shows as `"image://photo/" + encodeURIComponent(path)`, upright, in sRGB, decoded on threads of their own at its `sourceSize` at most. `options`: `{ stream: true }` gives a URL to play in a `MediaPlayer` instead, the service's `http://` one while it is still downloading (it plays as it comes), else the file's |
| `setFavorite(photos, favorite)` | photos (a list of positions or ids, or one) made favourites, or not, as the Favourites plugin's heart does: at once in the library (`item().favorite`, the grid's mark, `library.revision`), then in the source, iCloud's favourites or a folder's kept in its library. A promise of `{ changed }`, the ids of those that were not so already, once the source has them; rejected with the service's `code` (`read_only` for a library only looked at, `needs_sign_in`, `unavailable`, `unsupported`, `other_source` when another source is shown, `no_service`; `not_found` when none of them is in the library), the photos put back as they were. It makes no step to undo: record one with `did` |
| `run(program, args, options)` | a program, its own (in its folder) or any on PATH: a promise of `{ stdout, stderr, code }`. `options`: `input` (to its stdin), `timeout` (ms), `env` (added to Pinhole's), `output(line)` (each line it writes, as it comes). `cancel()` asks it to end (SIGTERM) and kills it a second later. `sqlite3`, a clipboard tool (`wl-copy`, `xclip`…) and `xdg-user-dir` are refused, by name, path or in a shell's line: rejected with `refused` (`pinhole.query`, `copyFile`, `picturePath`) |
| `record(item, options)` | an item as it looks over a number of frames, as a GIF or an MP4: a promise of the file's path. `options` are `path`, `format`, `width`, `height`, `fps`, `frames`, `seek(frame)` and `progress(fraction)` |
| `notify(text)` | a few words at the foot of the window for a while, as undo's: "Saved to Pictures" |
| `keepAwake(on)` | the screen kept on, no screensaver and no lock, while `on`, as the Photo frame does while it shows; `false` lets it go, and so does the plugin turned off or loaded again, and Pinhole quitting or crashing. Whether the screen is kept on now: not where it cannot be (not on Wayland, the window hidden, or on a workspace not shown), which it then is once it can |
| `warn(error)` | something gone wrong, under the plugin in the side panel and on stderr |
| `settings` | its settings, the user's choices (what its part of the side panel sets), kept in `~/.config/pinhole/plugins.json`: read one (`pinhole.settings.size`), bind to one, assign one and it is kept at once. Nothing else goes there (see below) |
| `setting(key, fallback)`, `setSetting(key, value)` | the same, by name; `setSetting` says whether it could be kept, and `null` takes the setting out (as assigning `null` to one does: it reads `undefined`, and `settings.keys()` leaves it out) |
| `state` | what the user made with it, which must outlive Pinhole but is no setting: Series' series made and taken apart by hand, Info's panel left open, the devices Send sent to. As `settings` is (read one, bind to one, assign one; `null` takes one out: it reads `undefined`, and `keys()`, the names it has, leaves it out), kept in `~/.local/state/pinhole/plugins/<id>/state.json` |
| `cache` | what it can work out again, kept to have it at once: what Screenshots found in each library, so they are hidden in the first frame. The same, kept in `~/.cache/pinhole/plugins/<id>/cache.json` |
| `did(name, undo, redo)` | a change just made, to undo with Ctrl Z and do again with Ctrl ⇧ Z: `name` says what in a word or two ("ungrouping"), `undo()` and `redo()` do it. Forgotten when the plugin is turned off or loaded again, and when another source's library is shown, however it came to be. It gives the step's number (API 3), for `dropStep` |
| `dropStep(step)` | a step it recorded (`did`'s number) that did not happen after all: what it changed was refused and put back, as a favourite the source would not set. Out of the history, so Ctrl Z undoes what was done before it, and no longer offered at the foot of the window. Record a step as the change is made, so Ctrl Z straight after works however slow the service is, and drop it if it fails |
| `copyFile(path)` | a file on the clipboard, an image as one a chat takes pasted too (a JPEG as it is, a GIF as it is, and a PNG of at most 2560 px, a GIF's first frame), as Pinhole's own copy has it: a promise of the path once the clipboard has it, which for a big picture is a moment later (its PNG is made first, away from the window), so say "Copied" then, not before; `not_found` if there is no such file. The last copy asked for wins: if another (a plugin's or Pinhole's own) is asked for while its picture is made, this one is never set, and is `cancelled` |
| `showFile(path)` | its folder in the file manager |
| `picturesDir()`, `exists(path)`, `fileSize(path)`, `sizeText(bytes, decimals)` | `sizeText`: "1.8 MB", with one decimal unless said |
| `picturePath(name, extension)` | a file to make in Pictures/Pinhole, the folder made: `name.extension`, or `name 2.extension`… if that is taken (API 5). In a self-test, the scratch Pictures, never the person's |
| `query(sql, values)` | the library's index (`library.sqlite`, [sources.md](sources.md)) asked, read only, away from the window, waiting up to five seconds while the service writes it: a promise of its rows, each an object by column name, `values` bound to its `?`s; rejected `sql` (its message) or `no_library` (API 5) |
| `chooseFolder(title)` | a folder chosen in the desktop's own picker: a promise of its path, rejected `cancelled` when none is |
| `sources` | the sources: `list` (every source added whose kind is on, as `PluginSource` is given them), `current` (the id of the one shown), `health` (how the one shown is doing: `fine`, `needs_sign_in`, `offline`, `failing`, or `no_service` when no service runs), `progress` and `rounds` (a library being built; when the one shown was last and is next looked at), `discovered` (what could be added of its kinds, by kind, as the service last found: iCloud's `{ icloudpd: […] }`, a folder's `{ folders: […] }`, a provider's whatever its `discover` says) and `discover()` to look again, `show(id)`; and, only of the kinds its manifest names, else rejected `not_yours`: `add(kind, entry)` (a promise of the source added and shown), `signIn(kind, params)` and `submitCode(kind, params)` (promises of `{ state }`, `signed_in` once added and shown or `needs_code`; rejected with the service's or the provider's code: `wrong_password`, `wrong_code`, `turned_off`…), `signOut(id)` (a promise of the sources left) |
| `dir`, `cacheDir` | its own folder; a folder for files it can make again, one to each source's library (`cacheDirChanged()` when another source's is shown), beside its `cache` |
| `coveredBy` | the plugin over the photos now, its id, `""` for none (see `covering`) |
| `dismissed()` | the plugin over this one, covering, handed a photo over (`openPhoto`, `reveal`): close, and do not come back when that photo closes |
| `id`, `api` | its id; the version of all this |

### Settings, state and cache

Keep each thing in the one it is:

- `settings`: what the user chose, the switches and choices in its part of
  the side panel. `plugins.json` is theirs, and pinholed reads it again
  each time it changes, so a plugin writes there only when the user
  changes a setting.
- `state`: what the user made and would miss: their edits, a panel left
  open, the devices they sent to.
- `cache`: what can be worked out again, and is kept only to be quick; it
  may be gone (`rm -r ~/.cache/pinhole`), and the plugin works it out again.

What is assigned is the plugin's at once. The file follows soon after,
away from the window, in one step (written beside it, then renamed over
it), and only if something changed, so assigning the same again costs
nothing; several changes in a moment are one write. What could not be
written is said under the plugin in the side panel. Each is read whole the
first time the plugin uses it, before the window's first frame if it does
so as it loads, so keep them small: ids, not pictures (files go in
`cacheDir`). What is kept for each source's library goes under a key with
its `library.key`, as Series' `edits:icloud:me@example.com` and
Screenshots' `found:…`; Screenshots keeps none for a library whose `key`
is `""`, nor a list past 5,000, and lets go of those of sources no longer
added.

Before API 3, Pinhole's own plugins kept all of these in their settings.
The first start of a Pinhole with API 3 moves them out of `plugins.json`,
once: Screenshots' `found:` lists to its cache, Series' `edits`, Info's
`shown` and Send's `devices` to their state, and the tours seen to
`~/.local/state/pinhole/toured.json`, each written there before
`plugins.json` is written without them.

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
only turn on one whose code you would run yourself. Under the switch of one
of your own that brings a source, the side panel says so: "It reads your
photos, and the accounts you sign in to with it. Turn it on only if you
trust it." A copy of iCloud's plugin in your folder (the same id) replaces
its QML only, never pinholed's Rust, and its row says: "Your own copy of
iCloud: it sees the password you type." (a copy of one whose sources never
sign in, Folders', that it reads the photos of every source shown with
it). It is off, iCloud's sources with it, until you turn it on: the middle
of the window then says so, and its button there opens the side panel at
its switch rather than turning it on.

What every plugin can see of the sources follows from that. Reading
`pinhole.sources` is open to every plugin, whatever kinds its manifest
names: `list` (each source's `name`, an Apple ID's account or a folder's
`path`, its count), `current`, `health`, `progress`, `rounds` and
`discovered` (the icloudpd sign-ins and the folders found), `discover()`,
and `show(id)`, which switches the source the window shows. Only what adds,
signs in to or out of a source is kept to the kinds the manifest names.
