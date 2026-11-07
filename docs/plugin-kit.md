# The kit, member by member

What each of `Pinhole.Plugin`'s QML types has: its properties, its
signals (a handler is `on` and the signal's name, capitalised:
`tapped` is `onTapped`) and its functions, with what their own source
says of them. Written from the files themselves by `bin/kit-reference`;
[plugins.md](plugins.md) says how to use them together.

## PinholePlugin

The root of a plugin's entry point (docs/plugins.md). It covers the window, above the photos and the viewer, and below Pinhole's own panels; what it shows there is its own. Pinhole draws its actions as its own buttons, in the places they name, and its settings in the side panel.

Its root is a `Item` (Qt Quick's: its properties too).

Properties:

- `var pinhole`: What Pinhole gives the plugin (plugins.h: PluginHost).
- `list<PluginAction> actions`: Buttons, each a PluginAction.
- `Component settings`: Under the plugin's switch in the side panel, while it is on.
- `bool covering`: Over the photos and taking the keys: the grid's controls fade and its keys rest, as for Pinhole's own panels. One plugin at a time, the last to start covering: the others wait beneath, unseen and without keys (pinhole.coveredBy says whose it is). By itself while one of its PluginViews is open.
- `var viewStack`: Its PluginViews open, the last opened last (they keep it), and how many.
- `int openViews` (readonly)
- `bool coveringAway` (readonly): The last of them opened over a photo it opened to come back from (pinhole.away): it covers that photo meanwhile, as People's W does.
- `var help`: Its keys, as the help (?) lists them: [["G", "Series"], ...].
- `var stacks`: Photos shown in the grid as one card, each a list of library positions: the card stands where the first is and shows it, with the mark for several photos. Opening it opens that photo.
- `var hidden`: Photos not shown in the grid at all, library positions: no cell, out of any stack, and passed over by the viewer's ← and → and its strip.
- `list<QtObject> marks`: Marks in the corner of grid cells, each a PluginMark: the photos whose library field is true, or those it lists, with a switch of their own in the side panel. Four at most.
- `Component viewerLayer`: Drawn over a photo shown full window, inside it: zoomed, panned, grown from its cell and clipped with it. Made afresh for each photo shown, with `photo` (its library position) and `shown` (true while that photo is shown, false the moment it changes or the viewer starts closing), which its root declares.
- `list<QtObject> sources`: The kinds of source it brings, each a PluginSource: how Pinhole's side panel, its first run and Add a source show them, and what signing in to one does. Only kinds its manifest's "sources" names.
- `var tour`: A few steps that show what it does, on the photos and its buttons as they are (Tour.qml): shown by itself the first time it is on, once `tourReady`, and again from the side panel.
- `bool tourReady`
- `bool holdSdr`: While true, a photo shown full window is kept at SDR: on a display that shows more (wide gamut, HDR), what is beyond fades out, and back in once no plugin asks. For something SDR drawn over the photo, as Live Photos' motion is (API 6).

## PluginAction

A button a plugin adds, drawn as Pinhole's own (docs/plugins.md).

Its root is a `QtObject` (Qt Quick's: its properties too).

Properties:

- `string place`: "grid": at the top right of the grid, before Select. "viewer": in the row at the top right of a photo shown full window, before Copy. "selection": in select mode's bar, after Copy, for what is ticked (pinhole.selection()); its key works only while it shows. "file" (API 15): no button of its own, something it does with a file another plugin made, which that plugin offers where it shows the file (pinhole.fileActions: Series' video as the background). Its key is what that plugin presses it with.
- `var accepts`: A "file" action's: the files it does, by their extension, lowercase ("mp4"); none, any. The file it is done with now, as it is triggered.
- `string file`
- `string group`: In the bars (the viewer's, select mode's): actions with the same group are one button, the first of them shown, and the rest in its menu under ▾, opened with ⇧ and its key too. Without one, alone.
- `string text`
- `string key`: A key that presses it while it shows: "G", "Shift+G".
- `bool shown`
- `bool usable`
- `bool active`: Lit, in the accent.
- `string textColour`: Its words' colour on the button when not lit: a theme colour's name (red, green, yellow, blue, magenta, cyan, accent) or "#rrggbb"; "" the theme's text. A favourite's heart is "accent".
- `bool busy`: On its way: an arc turns in its key's place, once it has been for `busyDelay` ms (soon, for what is mostly at once: Live's 300).
- `int busyDelay`
- `bool hovered`: The pointer over its button, for a plugin to show what pressing it would do: Pinhole sets it.
- `Item button`: Its button, where Pinhole draws it, for a tour step to light it.
- `int order`: Where it stands among the actions of its place: lowest first, then in the plugins' order. In select mode's bar, below 0 is before Pinhole's Copy, 0 and up after it.
- `string hint`: In its group's menu, a word at the right of its row in place of its key: "iPhone".

Signals:

- `triggered()`
- `complained(string text)`: What it could not do, in a few words: said under its button (under More ▾ when folded there) for a few seconds, the button shaking.

Functions:

- `takes(path)`
- `trigger(path)`: Done with `path`: `file` set to it, then triggered(), as long as it shows, can be used, and takes it. Whether it was.
- `complain(text)`

Its handler in QML: `onTriggered`.

## PluginSource

A kind of source, as its plugin shows it (PinholePlugin.sources, docs/plugins.md): what Pinhole says of a source of that kind and the buttons it has, where Pinhole lists sources (the side panel), offers new ones (the first run, Add a source) and says one needs signing in. Only a kind its manifest's "sources" names is taken.

Each `source` given is one of pinhole.sources.list (id, kind, plugin, name, count, thumbs, and its kind's own fields, such as account or path), with `shown` and `health` for the one shown.

Its root is a `QtObject` (Qt Quick's: its properties too).

Properties:

- `string kind`: One of its manifest's kinds: "icloud".
- `string icon`: A Nerd Font glyph for its sources' rows and its offers.
- `string noun`: The kind in a word, before a source's count in its row: "iCloud", "Folder". Its plugin's name when left out.
- `var place`: The photos' place in a sentence ("Nothing in %1 yet", "Add 3 files to %1"): source => "iCloud". The source's name when left out.
- `var offers`: What was found here that could be added (pinhole.sources.discovered), best first: [{ title, detail, icon, triggered }]. The first five of all the plugins' are offered on ⏎ and 2–5.
- `var ways`: The ways in that are always there: [{ title, key, triggered }], each a button with its key ("Sign in to iCloud", I).
- `var note`: A line in place of a source's count when it needs something, said in the theme's red: source => "Sign in again"; "" when all is well.
- `var actions`: Its buttons in its row beside Remove: source => [{ text, key, ask, triggered }], `ask` the question its row asks first (Yes, No).
- `var forgets`: What Remove asks of a source: source => "Forget its index? The photos stay in the folder."
- `var adds`: Under "Add 3 files to %1" as files are dragged over the window: source => "Copied into the folder…".
- `string pushed`: The line for a source that says itself when something changes, in place of when it is next looked at.
- `var signIn`: What "Sign in" does where Pinhole says a source needs it (the card in the middle, I, its row): source => {…}; null for a kind that never needs signing in.

## Command

One thing Pinhole does, and every way to do it (keymap.h): its keys, and how it goes with a mouse and with a touchpad. The keys work while its `keyLayer` has them (the one on top: the grid, the viewer, a menu, a panel) and `when` allows; a ShellButton given it as its `action` presses it, and counts as the way there for a pointer. In the help, under its `section`, if it has one.

Its root is a `Item` (Qt Quick's: its properties too).

Properties:

- `string name`: For tests and the coverage check: "grid.open", "viewer.close".
- `string text`: What it does, for the help: "Open", "Newest".
- `string section`: Its part of the help: "library", "select", "photo", "video", "share", "pinhole", a panel's ("sources", "storage", "tour"), or a plugin's name: a plugin's own are under its name in the language, whatever other than Pinhole's parts they say. None: not in the help (a button's own key, shown on it).
- `var keys`: The keys that do it, as sequences ("Space", "Shift+Up"); the first is the one named, unless `keysText` says it better ("1–9", "↑ ↓").
- `string keysText`
- `string mouse`: How with a mouse, and with a touchpad, when not by a button it has: "Wheel", "Pinch". A ShellButton showing it is way enough.
- `string touchpad`
- `string noKeyboard`: Why there is none, for an action that cannot have one: typing a password needs a keyboard. Counts as covered.
- `string noMouse`
- `string noTouchpad`
- `string keyLayer`: Whose keys these are: "grid", "viewer", "menu", "side", "addSource", "firstRun", "tour", "help", "plugin"; "chrome" for a button's, on the grid or the viewer, whichever has them; "any" for the window's own.
- `bool when`
- `bool autoRepeat`
- `bool bound`: False for keys read elsewhere (the viewer's arrows, timed as they are held): listed, but no shortcut of its own.
- `bool keyHelp`: Which of the help's tabs list it: the keys', and the mouse's and the touchpad's (where its `mouse` and `touchpad` say how, briefly).
- `bool pointerHelp`
- `string file`: Where it is written (keymap.h sets them), for the help's order.
- `int line`
- `int buttons`: ShellButtons showing it now (they count themselves).
- `bool pinholes`: A row of one of Pinhole's own menus (MenuGroup): its key is Pinhole's, though written in the kit's file.
- `string keyName` (readonly): The key as a button or the help shows it: "⇧ E", "Esc".
- `bool live` (readonly)

Signals:

- `triggered()`

Functions:

- `plugin()`: The plugin it is in, by the Loader Main.qml loads each in, else by its file; "" for Pinhole's own, or not known yet.
- `ours()`: The "plugin" layer's keys are the covering plugin's alone (keymap.h).
- `trigger()`

Its handler in QML: `onTriggered`.

## ShellButton

A button as Omarchy's shell draws one (shell.toml's controls): a faint fill and a hairline in the text colour, both stronger under the pointer and when pressed. `active` is the chosen state, filled with the accent. `flat` drops the hairline at rest, for rows inside a bar or menu.

Its key beside the label, as Omarchy's menus show theirs. Given an `action` (Command.qml), it presses that and shows its key; else `key` presses it while it shows (a sequence: "C", "Shift+E", "Escape"), on `keyLayer`'s keys, and `section` and `helpText` put it in the help. `hint` only shows a key, for one the window handles itself.

`busy`: on its way, after `busyDelay` an arc turns in the key's place (in the label's, without one), as Spinner.qml turns.

`justified`, for a row of a menu: the label at the left, the key at the right, as wide as the menu makes it; `lit`, chosen there with the keys, lit as under the pointer.

Its root is a `ControlSurface`:

Properties:

- `string text`
- `var action`
- `string key`
- `string keyLayer`
- `string section`
- `string helpText`
- `string hint`
- `bool active`
- `color labelColor`: The label's colour when not active: the theme's text, or the accent for a ♥ that is filled.
- `bool flat`
- `bool floating`: Over the photos rather than on a surface of the app: backed with the theme's background, as the chips are, so it reads on any photo.
- `bool busy`
- `int busyDelay`
- `bool justified`
- `bool lit`
- `bool checkable`
- `alias color`
- `alias radius`
- `alias border`
- `int inset`: Its space at each side of what it says.
- `bool hovered` (readonly): The pointer over it, while it shows.
- `color ink` (readonly)
- `real naturalWidth` (readonly): Omarchy's control metrics (control-height 28, control-padding-x 10 at a base size of 12), with the type scale.
- `bool lighter` (readonly)
- `bool turning` (readonly): The arc in the key's place: the key stays, unseen, so the button keeps its width.
- `var counted`: The action's way for a pointer, while this shows it.
- `bool usable`: Whether it can be used now: not, and it is drawn faint and takes no press.

Signals:

- `tapped()`: Pressed: a click, a tap, its key (a click on a button is a tap here, not `clicked`).
- `activated()`: Pressed: a click, a tap, or Space or Return while it has the keys.

Functions:

- `press()`: As a tap does: from the keys of a panel it is a stop in.
- `count()`
- `complain(text)`: What pressing it could not do, in a few words under it for four seconds, the button shaking, as a PluginAction's complain() says it under its button (Complaint.qml).

Its handler in QML: `onTapped`.

## Chip

A label over the photos, as Omarchy's shell draws its cards: the theme's background, nearly opaque, a hairline in the text colour, square corners (or the theme's own rounding).

Its root is a `Rectangle` (Qt Quick's: its properties too).

Properties:

- `string text`
- `alias label`
- `color textColor`
- `bool morphing`: While true, a new text morphs from the old: what the two share at the start stays, what they share at the end slides to its new place, and only what differs between cross-fades ("24–28 July" to "25–28 July" changes the one digit). The chip's width eases to fit. A morph waits for the text to hold still a moment, and is never cut short: texts coming faster (scrolling quickly past the days, zooming out all the way in one pinch) change at once, as a counter does. Morphs cut off by the next text, or one after another, flickered.
- `bool centred`: The text inside, centred; or from the left, the chip's right edge moving as it changes.
- `real naturalWidth` (readonly): Omarchy's control metrics (shell.toml: control-height 28 and control-padding-x 10 at a base size of 12), with the type scale.
- `string shownText`: What the label shows: the text, or on its way to it.
- `string morphStart`: A change in parts: the shared start, the part going and the part coming, the shared end; and how far it has gone, 0 to 1.
- `string morphGoing`
- `string morphComing`
- `string morphEnd`
- `real morphed`
- `bool ticking`: Changing at once, the chip's edge too, so it hugs the text.
- `int padding` (readonly)
- `real slid` (readonly): The end slides as the chip's edge does (out cubic).

Functions:

- `change()`
- `snap()`

## UiText

Text as Omarchy sets it: its typeface, size and the theme's text colour.

Its root is a `Text` (Qt Quick's: its properties too).

Functions:

- `centredIn(height)`: Where to put this text in a box `height` tall so its letters sit in the middle: centred on the capital height, as native toolkits align labels, rather than on the line box, whose room for descenders left the text riding high. (QML's FontMetrics has no capHeight; a capital's tight bounds give it.)

## Spinner

Something on its way, shown only once it is taking a while (photos mostly come quicker than a second): a small arc turning, as Omarchy's shell turns an icon while busy, on a chip's square so it reads over any photo.

Its root is a `Rectangle` (Qt Quick's: its properties too).

Properties:

- `bool busy`: Whether it is waiting; the spinner shows after `delay` of it.
- `int delay`
- `bool bare`: Without its square, for inside a button; drawn in `ink`.
- `color ink`
- `bool shown` (readonly)

## Toggle

A switch for something on or off, as Omarchy's shell draws one: a track in the accent when on, the knob sliding across. Its key beside it, as the buttons show theirs.

Its root is a `ControlSurface`:

Properties:

- `bool checked`
- `string hint`
- `bool revealOnPointerFocus`
- `bool lit`: Chosen with the keys: a ring around it, in the accent.
- `bool usable`: Whether it can be used now: not, and it is drawn faint and takes no press.

Signals:

- `toggled()`
- `activated()`: Pressed: a click, a tap, or Space or Return while it has the keys.

Functions:

- `press()`

Its handler in QML: `onToggled`.

## FieldInput

A line to type into, as the rest of the shell draws its controls: a hairline that lights in the accent while typing, the hint in the muted text colour until something is typed, and a tick once it is settled.

Its root is a `Rectangle` (Qt Quick's: its properties too).

Properties:

- `alias text`
- `alias input`
- `string placeholder`
- `bool password`
- `bool done`

Signals:

- `accepted()`
- `cleared()`: Escape took back what was typed.

Its handler in QML: `onAccepted`.

## ScrollList

A list that scrolls as the grid does: a ListView, its wheel, touchpad and pinch taken by a Scroller (scroller.h), so they move it with the grid's own inertia, glide and springs, and none reaches what is beneath. Across, as a strip, with `orientation: ListView.Horizontal`. Inside another (a list in a panel), the inner one moves first, to its end.

Its root is a `ListView` (Qt Quick's: its properties too).

## ScrollGrid

A grid of cells that scrolls as Pinhole's grid does: a GridView, its wheel, touchpad and pinch taken by a Scroller (scroller.h). In a PinchZoom, its cells grow and shrink under the fingers.

Its root is a `GridView` (Qt Quick's: its properties too).

## ScrollArea

Anything larger than its room, scrolled as the grid scrolls: a Flickable (give it a contentWidth and a contentHeight), its wheel, touchpad and pinch taken by a Scroller (scroller.h); up and down, and across where the content is wider than it. A long text: `TextArea.flickable: TextArea {}` in it.

Its root is a `Flickable` (Qt Quick's: its properties too).

## PinchZoom

What it holds, zoomed by a pinch, Ctrl and the wheel, and the sideways wheel through a few sizes, as the grid zooms through its columns (zoomer.h): laid out at `size`, one of `levels`, and drawn larger or smaller under the fingers until it lands on one. `zoomBy(1)` and `zoomBy(-1)` step, eased, as + and - do on the grid. A ScrollList, ScrollGrid or ScrollArea in it passes its pinches on to it.

Its root is a `Zoomer` (Qt Quick's: its properties too).

Properties:

- `alias content`

## Panel

A plugin's screen over the photos: its parent's whole room, in the theme's background or a `color` of its own, taking every click, touch, wheel turn, swipe, pinch and hover over it (Shield), so the photos beneath get none. Its controls go after it. `clicked()` is a click on it where no control is (on a dimmed one beside a card, it puts the card away); with `steps` (Qt.Horizontal, Qt.Vertical), the wheel and a swipe step through something: `stepped(1)` the next, `stepped(-1)` the one before.

Its root is a `Rectangle` (Qt Quick's: its properties too).

Properties:

- `alias steps`

Signals:

- `clicked()`
- `stepped(int step)`

Its handler in QML: `onClicked`.

## Card

A card: a box in the theme's background with a hairline round it, that lets nothing over it through to what is beneath (Shield). Its controls go in it. Over the photos for a moment (a sign-in, a question), on a dimmed Panel whose click puts it away:

```qml
Panel { color: Qt.rgba(0, 0, 0, 0.45); onClicked: plugin.close() }
Card { anchors.centerIn: parent; width: 400; height: 300 }
```

Its root is a `Rectangle` (Qt Quick's: its properties too).

## Choices

One of a few, chosen on a row of buttons, each also its number's key (1, 2, 3…) while `when`: `options` as [{ value, text }], the one chosen `value`, and `chosen(value)` when another is. The keys are in the help under `section`, as `helpText`. Given a `setting` (API 13), it shows and keeps that one of pinhole.settings, as a SettingSwitch does.

Its root is a `Row` (Qt Quick's: its properties too).

Properties:

- `var options`: [{ value, text }], in order.
- `string setting`: The name of one of the plugin's settings, whose value it is.
- `var value`: The one chosen.
- `bool when`: While true, the number keys choose.
- `string keyLayer`: Whose keys they are: the covering plugin's ("plugin"), or "grid" or "viewer".
- `string section`: Where the help lists the number keys, and in what words.
- `string helpText`
- `bool flat`
- `bool numbered`: Each its number's key (1, 2, 3…), shown on it; false for a setting's row in the side panel, where the numbers are other things' keys.

Signals:

- `chosen(var value)`: Another chosen, with its value (`value` is not set by itself).

Its handler in QML: `onChosen`.

## PhotoTiles

Photos as square tiles, as a plugin's view shows what it found (API 11): search results, a place's photos, the newest additions. Each cropped to fill its square at the display's own pixels, a ring in the accent on the one the keys are on and a fainter one under the pointer; ← → ↑ ↓, Home and End, PgUp and PgDn move, ⏎ and Space open, while `active`. The wheel and the touchpad scroll it as the grid does (ScrollGrid).

```qml
PhotoTiles {
    library: pinhole.library
    photos: results.map(row => row.id)
    active: shown
    onOpened: (id, index) => pinhole.openPhotos(photos, id, { comeBack: true })
}
```

`caption`, a Component, goes under each tile, `captionHeight` tall: its root declares `property string photo` (the id) and `property int index`, which are set.

Its root is a `FocusScope` (Qt Quick's: its properties too).

Properties:

- `var library` (required)
- `var photos`
- `int cursor`
- `bool active`
- `bool showCursor`: The ring on the one the keys are on: while `active`, or as the view says (on a search's best match while its field has the keys).
- `real minCell`
- `real gap`
- `Component caption`
- `real captionHeight`
- `int count` (readonly)
- `int columns` (readonly)
- `real cell` (readonly)
- `bool keyboard` (readonly)
- `alias view`

Signals:

- `opened(string photo, int index)`: Each with the photo's id and its place among `photos`.
- `pastTop()`: Up from the first row: for a field above to take the keys.

Functions:

- `move(by)`
- `moveTo(photo)`: The keys to the photo `photo` (an id), in view: as a view comes back to the one last looked at.
- `open(index)`

Its handler in QML: `onOpened`.

## PanelHeader

The title bar of a plugin's view over the whole window (API 11), as the map's, search's and People's: its `title` in the larger size, a quieter `detail` after it, the `tools` (buttons, a field) at the right, then ✕ with its key, which `closed()` says. Items given as its children go before the title (a ‹ back button); `progress`, 0 to 1, draws a line along its foot while it is under 1 (-1 for none).

Its root is a `Item` (Qt Quick's: its properties too).

Properties:

- `string title`
- `string detail`
- `QtObject closeCommand`: The Command that closes the view: ✕ presses it and shows its key.
- `Item pluginView` (readonly): The PluginView it is in, if any: ✕ closes that, from any level of it (Esc may mean back there, its back()).
- `string closeHint`: ✕'s key without a closeCommand: "" where Esc means back, not close.
- `real controlHeight` (readonly): A control's height here, for what stands beside the buttons.
- `real room` (readonly): What a field in place of the title (search's question) may take.
- `real progress`
- `color progressColor`
- `alias leading`
- `alias tools`

Signals:

- `closed()`

Its handler in QML: `onClosed`.

## SettingNote

A plugin's settings' few words (API 11): what a choice does, or how it is now, in the side panel's smaller type, wrapped; `dim` for a remark.

A `UiText`, with all it has (below), and:

Properties:

- `bool dim`

## SettingSwitch

A plugin's setting that is on or off (API 11): its words, and a switch at the right that says `toggled()`; with `usable` false, dimmed and still. Pressing the words presses the switch.

Given a `setting` (API 13), it is that one of pinhole.settings: shown as it is (its default from the manifest's "settings" until chosen), and flipped and kept when pressed, with nothing more to write:

```qml
SettingSwitch { setting: "shuffle"; text: qsTr("Shuffle") }
```

Its root is a `Item` (Qt Quick's: its properties too).

Properties:

- `string text`
- `string setting`: The name of one of the plugin's settings, true or false.
- `bool checked`
- `bool usable`
- `alias hint`: The switch's key, shown beside it ("⇧ S").

Signals:

- `toggled()`: Pressed. It does not flip `checked` itself: set the setting, which `checked` is bound to.

Its handler in QML: `onToggled`.

## PluginView

A view of the plugin's own over the photos (API 13): what a calendar, a gallery or a studio is drawn in. Put it in the plugin's root and open it from an action:

```qml
PluginView { id: calendar; source: "Calendar.qml"; toggleKey: "Y" }
PluginAction { place: "grid"; text: qsTr("Calendar"); key: "Y"; onTriggered: calendar.open() }
```

While it is open the plugin covers the photos (its root's `covering`, unless the plugin binds that itself), and it fades in and out. It is made the first time it opens and kept after, so a launch that never shows it pays nothing; `lazy: false` makes it at once. It closes on Esc and on its `toggleKey`, when a photo is handed over from it (pinhole's dismissed()), and is hidden, still open, while a photo opened from it with comeBack shows (pinhole.away). One opened while that photo shows is over it, covering it, as People's W names who is in it.

What it shows is `source`, a QML file of the plugin's, or `content`, a Component; `item` is that, once made. In a `source` file, `view` is this view from its first binding on (`view.shown`, `view.close()`, `view.plugin`, the plugin's root), and `pinhole` the plugin's, as anywhere in its files; declare neither. A PanelHeader in it closes it with ✕.

`open(…)` hands what it is given to a `start(…)` the item has, before it shows (an Apple ID, the photos to send); `close()` calls its `stop()`. `keep: false` lets go of the item as it closes, to be made afresh.

Esc goes to what it shows first: a function `back()` there that returns true has taken it (a close-up left, a making stopped), and the view stays open. Two of a plugin's views open at once (a studio over an editor) stack: only the last opened has its keys, and Esc closes it alone. `make()` makes what it shows without opening it.

Its root is a `Item` (Qt Quick's: its properties too).

Properties:

- `url source`: A QML file of the plugin's, relative to its main file ("Calendar.qml").
- `Component content`: Or what to show, written in place.
- `bool lazy`: Made the first time it opens (true), or at once.
- `bool keep`: Kept once made (true), or let go as it closes.
- `bool shown` (readonly): Open now.
- `string toggleKey`: The key that opens it (its action's), which closes it too, beside Esc.
- `string closeText`: Esc's words in the help, none by default (the action that opens it is listed); `closeCommand.mouse` and `.touchpad` say how else it closes ("Click"), as a Command's.
- `Item item` (readonly): What `source` or `content` made; null before it first opens.
- `alias closeCommand` (readonly): Its Esc: what a PanelHeader's ✕ presses.
- `Item plugin` (readonly): The plugin's root and what Pinhole gives it.
- `var pinhole` (readonly)
- `bool onTop` (readonly): The last opened of the plugin's open views: the one with the keys.
- `bool openedAway` (readonly): Opened over a photo the plugin opened to come back from: shown.

Signals:

- `opened()`
- `closed()`

Functions:

- `open(...given)`
- `close()`
- `toggle()`
- `make()`: What it shows, made now if it was not, without opening it.
- `back()`: Esc: back in what it shows first (its back()), else closed.

Its handler in QML: `onOpened`.

## Job

One thing the plugin asks for at a time, the latest winning (API 13): a query, a run, a fetch. What a plugin otherwise writes by hand around each promise: cancel the one before, drop an answer that came too late, keep a busy flag, ask again when the library was read meanwhile, and stop when the plugin does.

```qml
Job {
    id: reading
    onDone: rows => recent.rows = rows
    onFailed: error => recent.problem = error.message
}
reading.start(() => pinhole.query(sql, [limit]))
```

`start(ask, answered)` cancels what runs and calls `ask`, which returns the promise, or a list of them asked together (a page of previews): then `done` has their answers in order, the first to fail fails them all, and a cancel cancels each. Only its answer is said: `done(result)`, or `failed(error)`. Given `answered`, that has the answer instead of done(), for an answer that needs what it was asked with:

```qml
motion.start(() => pinhole.fetch(photo, kind), file => play(kind, file))
```

Cancelled is no failure and says nothing. Stopped by the library read again (`reloaded`), it asks again by itself, once.

Its root is a `QtObject` (Qt Quick's: its properties too).

Properties:

- `bool busy` (readonly): Something asked and not yet answered.
- `bool coalesce`: Asked while busy: wait for the one running, then ask once more with the latest (a search typed into), instead of cancelling it.

Signals:

- `done(var result)`
- `failed(var error)`

Functions:

- `start(ask, answered)`
- `cancel()`: Stop what runs; its answer is never said.

Its handler in QML: `onDone`.
