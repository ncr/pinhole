# A smaller core, and plugins that can do more

This is a design to carry out, one phase at a time. It is meant to land as
`docs/core-and-plugins.md` and to be kept true as the phases land.

Jacek asked for this: plugins should be able to define marks. Where the
photos come from (iCloud, a folder) should be a plugin, and so should Live
Photos. These plugins are on by default, but they are plugins. Take the
path that makes the app itself smaller and the plugins able to do more.
There should also be one HEIC to JPEG conversion, which plugins can use too.

Everything below was read from `origin/main` at 7d06da2. The numbers come
from Jacek's real library, read only.

## What there is today

### How pinholed uses a source

`core/src/source.rs` has the `Source` trait. `service.rs` holds the source
shown as an `Arc<dyn Source>`, and `sync::source_for` makes one from a
settings entry. Four paths use it:

- **Sync** (`sync_loop`, then `sync_round`, then `sync::catch_up`). Each
  round calls `source.start()`. Then `sync::claim` checks that the library
  is this source's (`meta.source`, set from `source.id()`). Then
  `source.changes(&mut Index, token)` runs, or `source.list(&mut Index,
  progress)` when there is no token. The source writes through `Index::put`
  and `Index::remove`. iCloud also writes its own tables (`icloud_assets`,
  `icloud_resources`, `icloud/index.rs`) on the same connection. Then
  `sync::make_thumbs` runs: tickets come from `source.prepare(conn,
  &[Stored])`, a thousand at a time, and bytes from `source.open(ticket,
  None)`, 16 at once. It decodes each thumbnail for its size and average
  colour. Last, `sync::make_covers` makes covers for videos: it uses
  `source.local()`, or `prepare` and `open`. `source.watch()` (iCloud's push)
  starts a round 1.5 s after a push. `source.health(&error)` sets the
  health. `rounds_every()` sets how often rounds run.
- **Fetch** (`service::fetch`). It finds the rendition in `renditions`.
  It serves `source.local()` files as they are. Next it tries the cached
  file by key. Otherwise it queues a `Job` for `lookup_loop`. That loop
  batches requests for 25 ms, up to 100, and calls `source.prepare(db,
  ...)`. iCloud's `prepare` reads `icloud_resources.url` and writes fresh
  links back into it. The jobs then go to `download_loop`, which calls
  `source.open(ticket, None)`. A video that streams also calls `open` with
  byte ranges through `stream.rs`. An expired ticket is prepared again,
  once.
- **Effects** (`service::make`). A `loop` the source does not have is the
  `live` file itself. A `bounce` or `long_exposure` it does not have is
  made. pinholed fetches `live` through `fetch` with an `Out::Service`
  reply, runs `effects::bounce` or `effects::long_exposure` (ffmpeg), and
  keeps the result in `cache/live/` next to the motion. The source itself
  knows nothing of this.
- **Upload** (`service::add`). `add::gather` pairs Live Photos. Then
  `source.accepts()` and `source.add(addition, sent)` run, one asset at a
  time. iCloud uploads (`icloud/upload.rs`), and a folder copies the file
  in (`add::copy_into`). Each asset added starts a round.
  **Favourites** work the same way: `source.set_favorite(db, ids, on)`.

What pinholed knows of iCloud apart from the trait is spread out. It is in
`service.rs` (`sign_in`, `submit_code`, `sign_out`, `signing_in`, the
`account` field in `sources_report` and `source_info`, the `icloud` and
`folder` branches of `set_source`), `storage.rs` (`resolve`, which makes the
Apple IDs signed in to into sources when `sources` is missing),
`discover.rs` (icloudpd's stems) and `sync::source_for`.

### How marks reach the C++ grid

`marks.h` has `enum Mark { MarkVideo = 1, MarkLive = 2, MarkFavorite = 4,
MarkStack = 8 }` and `kMarkKinds = 4`. In `PhotoGrid::updatePaintNode`,
step 3 works out each visible cell's marks from `LibraryItem::video`,
`::live` and `::favorite`. It adds `MarkStack` when `m_stackOf` has the
photo, and masks the result with `m_marks`. The atlas is built once per dpr
and colour version: `kMarkKinds` plain marks side by side (`rs.marks`), and
`kMarkKinds × 2 × kNeonSteps` neon frames (`rs.neon`). The order is
`kMarkOrder = {Favorite, Video, Live, Stack}`. `m_marks` comes from
`SidePanel.qml`'s `Settings { location: …/window.conf; category: "grid";
property int marks: 13 }`. Jacek's `window.conf` says `marks=13`: video,
favourite and stack on, Live off. `markColours` is `[theme.cyan,
theme.yellow, theme.red, theme.magenta]`. No plugin can add a mark.

### How the viewer plays Live motion

`Viewer` (viewer.cpp) holds `m_effect` (the effect chosen, kept in
`m_chosen` by photo id for the run), `savedEffect()` (from
`LibraryItem::effect`) and `motionKind()`: `live`, or `loop` or `bounce`
while looping. It also holds `stillKind()`, which is `original` when a
photo saved as a long exposure is shown as anything else, because iCloud's
`full` *is* the long exposure.

`show()` calls `showEffect()`. That asks the service for `loop` or `bounce`
(streamed) or `long_exposure`. `playLive()` asks for the motion kind with
`stream: true`. The answer sets `m_live` and emits `liveChanged`.
`PhotoViewer.qml` has a `MediaPlayer livePlayer` and `VideoOutput
liveOutput` inside the photo's `frame`. It also has `longExposureImage`
above the original. It plays on `liveChanged` and loops when
`viewer.looping`. It stops on `indexChanged`, on `closeViewer()` and when
the effect changes. The Live button (L), its effects menu (⇧L) and
`liveNotice` (a chip under the button that shakes) are in the viewer's
`ActionBar`.

### How plugins load

`Plugins::scan` reads `builtInDir()` (`PINHOLE_BUILTIN_PLUGINS`, else
`../lib/pinhole/plugins` next to the binary, else `../../plugins` in a
checkout), then `personalDir()`. A personal plugin with a built-in's id
takes its place. `enabled()` reads `plugins.json`'s `enabled[id]`, and a
plugin with no entry there is on when built-in and off when personal.
Jacek's `plugins.json` has `settings` and `toured` and no `enabled`, so
every built-in is on. (Since B1b, `toured` and what was not a setting move
out of it: see B1b.) `Main.qml` loads each plugin that is on in a `Loader`
once `plugins.ready`. It gathers them once, on `allLoaded`
(`collectPlugins`), and follows `hidden` and `stacks` through a reload
with `plugins.hiddenOf` and `plugins.stacksOf`.

### Conversion

`Outgoing::place` runs `magick <full> -quality 92 jpg:<part>` into
`~/.cache/pinhole/compatible/<compatibleName>`. Send has its own copy of
this in `pinhole-send jpeg` (`plugins/send/sender/src/main.rs:718`), which
keeps its JPEGs in its cacheDir.

### What was measured

All of this was read only, from Jacek's library
(`~/.local/share/pinhole/library/icloud/<apple-id>`).

- 49,079 assets, 204,274 renditions, 192,648 `icloud_resources` rows, and
  13,987 Live Photos. `library.sqlite` is 200 MB. The thumbnails take
  3.1 GB, about 64 KB each. `PRAGMA user_version` is 1.
- Every asset as one JSON line, with its renditions and iCloud's own rows
  (master, change tag, resources), comes to **67 MB**. Python's `json`
  parses all 49,079 lines in **0.5 s**, and serde is several times faster.
  Listing the library from iCloud takes about 5 minutes, so sending a whole
  listing over a pipe costs about 0.1 % of it.
- Sending every thumbnail's bytes over a pipe would be 3.1 GB, about 2 s at
  pipe speed, against a thumbnail pass of about 6 minutes. With tickets
  that are URLs or paths, as below, no bytes cross a pipe at all.
- Code that would move: `core/src/icloud/` is 2,368 lines, plus the
  286-line provider. `folder.rs` is 924 lines. `service.rs` names iCloud or
  Apple 55 times.
- There is no `~/.config/pinhole/settings.json`. Jacek's iCloud source is
  *implied* by his session in `~/.local/state/pinhole/auth/`
  (`storage::resolve`). Any change to settings must keep that working
  exactly as it does now.
- The disk is btrfs, so `cp -a --reflink=always` copies the whole data
  folder at once, for tests on a copy.

## Decisions

### Sources: built-ins in process, owned by plugins; others out of process

**Decision: a mix.** iCloud and Folder stay Rust inside pinholed, behind
the `Source` trait. Each is *owned by a default-on plugin*, `pinhole.icloud`
or `pinhole.folder`. The plugin has the manifest, the switch, all of the
UI (sign-in, its rows in the side panel, what it offers when a source is
added), the translations, and in the end the Rust itself, which moves into
the plugin's folder as a crate linked into pinholed. A plugin of your own
can add a source of another kind. It does that through an out-of-process
*provider* that speaks a JSON-lines source protocol, as sign-in providers
already do.

Why the built-ins are not out of process:

1. **Jacek's library would have to move.** iCloud keeps 192,648 rows of
   its own (download links, checksums, change tags, masters) in
   `library.sqlite`, and `prepare` and `set_favorite` read them with joins
   on the same connection. Only pinholed writes the library. An outside
   iCloud provider would need those rows in a file of its own, or pinholed
   would have to write opaque rows for it. Either way that means a
   migration of a 200 MB index on his disk, and nobody would see a gain.
2. **Speed is not the reason either way.** The measurements above show
   that the pipe is not the cost: a listing is 0.1 % of the walk, and
   tickets that are URLs or paths keep every byte in pinholed. Speed is
   simply not a reason to pay for (1).
3. **The isolation that matters is already there.** Apple's sign-in, the
   one part that handles the password, already runs in its own process
   (`pinhole-auth`). The Rust walk, links, push and upload have not
   crashed pinholed. The one weak spot, a panic in `sync_round` silently
   ending the sync thread, is closed in process by `catch_unwind` (phase
   A2).
4. **Little code moves.** In process, the work is to take the iCloud and
   folder knowledge out of `service.rs`, `storage.rs`, `sync.rs` and
   `discover.rs` and put it behind a `Kind` trait. Out of process would
   mean a protocol, an adapter, a new provider binary, its own store, and
   the migration.

Why there is an out-of-process path anyway: a source of your own (Immich,
a NAS, a phone) must not need pinholed rebuilt. The same protocol makes
that possible, and keeps such a source out of pinholed's memory and out of
`library.sqlite`, which pinholed writes for it from what it says.

### What a built-in source plugin may do that yours may not

- Only kinds compiled into pinholed run in process: `icloud` and `folder`.
  They write the library's own tables of their kind and use pinhole-auth.
  pinholed ties each to a fixed plugin id (`pinhole.icloud`,
  `pinhole.folder`) and reads only that id's switch. No manifest can claim
  an in-process kind.
- A source of your own is a program in your plugin's folder
  (`"sources": [{"kind": "me.immich", "provider": "pinhole-immich"}]`).
  pinholed starts it only while the plugin is on, which you do yourself:
  personal plugins start off. It gets a state folder of its own,
  `~/.local/state/pinhole/sources/<plugin id>/`, mode 0700. It never
  writes the library. pinholed writes what it says, after checking it. Its
  kind must be its plugin id, or start with that id and a dot, so it can
  never take `icloud` or `folder`.
- In the window, a plugin's `pinhole.sources` acts only on the kinds its
  manifest declares. Any other kind is rejected with `not_yours`, so no
  plugin starts another's sign-in by mistake.
- Under the switch of a personal plugin that declares a source, the side
  panel says: "It reads your photos, and the accounts you sign in to with
  it. Turn it on only if you trust it." A personal copy of a built-in
  source plugin (the same id) replaces only its QML, never pinholed's
  Rust. Its row says: "Your own copy of iCloud: it sees the password you
  type."
- This is honest, not a sandbox. Every plugin runs as you, as `Trust` in
  `docs/plugins.md` says. The rules above keep plugins from stepping on
  each other's toes. They do not stop a plugin you chose to run.

### Live: the plugin shows it, pinholed makes the files

`pinhole.live` owns everything you see of Live Photos: the mark, the Live
button (L) and its effects (⇧L), playing the motion over the photo, the
long exposure, and its messages. The library still says what a photo is
(`is_live`, `live_effect`, a `live` rendition), because that is what the
source reports. pinholed still answers `loop`, `bounce` and
`long_exposure`, and still makes the ones a source has not got. Those are
*derived files*, kept in the cache under the one limit. Background already
fetches `loop` and `bounce`, and Frame and Send fetch `live`. If effects
were made by the Live plugin, those plugins would depend on it, and the
files would sit outside the limit.

### One conversion: `fetch(photo, "jpeg")`

pinholed makes a JPEG of a photo's `full` (ImageMagick, quality 92, which
keeps the date, place and camera details) when it is first asked for. It
keeps it in the cache under the limit, as it does the effects. A photo that
is a JPEG already is answered with its `full`. `Outgoing` (Ctrl+⇧E, drags,
Ctrl+C) and Send both ask for it. `pinhole-send jpeg` and
`~/.cache/pinhole/compatible` go away. Any plugin gets it with
`pinhole.fetch(photo, "jpeg")`.

### Marks: declared by plugins, drawn by the grid from bits

A plugin declares `marks`. Each is drawn from a *field* of the library
(`"live"`, `"favorite"`, `"video"`), which the grid reads per cell as it
does today, or from a list of positions, which C++ turns into a bit per
photo once, when it is given. The grid's per-frame work stays one 32-bit
read per visible cell. QML does nothing per frame. The video and stack
marks stay in the core.

### Favourites: a plugin over a core API

The library keeps `favorite` as a field the source reports, and the core
keeps changing it: `pinhole.setFavorite(photos, on)` sets it in memory at
once, asks pinholed (`set_favorite`) and puts it back if that fails.
Everything you see moves to `pinhole.favourites`: F, the heart in the
viewer and in select mode's bar, the mark, the undo step, and the
complaint when it fails.

## The core after the change

| Feature | Where | Why |
|---|---|---|
| The grid: levels, scrolling, dates, cursor, select mode, neon | core | It is the browser itself, and it is C++ for 120 Hz |
| Reading and reloading the library; thumbnails | core | Everything reads it, and plugins' positions follow its reloads |
| Stacks and hidden (the API, and the stack mark) | core | The grid's layout, which plugins feed |
| Video mark, playing a video, its seek bar, M | core | A browser that cannot play a video is not one. The viewer's player is in its frame |
| Viewer: photos sharpening, zoom, pan, carousel | core | C++ motion (`viewermotion.cpp`) |
| Live mark, Live button, effects menu, motion, long exposure | **new `pinhole.live`** | Shows what a source reports; needs only the viewer hooks below |
| Favourite mark, F, hearts, undo | **new `pinhole.favourites`** | UI over `pinhole.setFavorite` |
| Copy, Export (Ctrl+E, ⇧E), dragging out | core | A file promise drag and the desktop's picker are C++. Every source has them |
| HEIC to JPEG | core, in pinholed (`jpeg`) | One conversion, cached under the limit, for the window and plugins |
| Effects made from the motion | core, in pinholed | Derived files that Background and Live share, under the limit |
| Dropping files to add them (`DropTarget`, `Incoming`) | core | The window's one way in for files, for whatever source is shown. A source says only `accepts` |
| Storage: folder, limit, move | core | pinholed's cache and libraries |
| Sources: the list, switching (1–9), Remove, progress, rounds | core | Generic over kinds |
| A source's row details and buttons (Sign out, Sign in again, disk missing) | the source's plugin (`PluginSource.note`, `.actions`) | Kind-specific |
| Adding a source: what was found, and the ways in (icloudpd, folders, Choose a folder, Sign in) | the source's plugin (`PluginSource.offers`, `.ways`) | Kind-specific |
| Signing in to iCloud (the panel, the code boxes) | **new `pinhole.icloud`** | Kind-specific |
| "Not signed in" card in the middle | core card, calling `PluginSource.signIn` | Generic trouble, kind-specific action |
| iCloud: CloudKit, walk, change feed, links, push, upload, favourites, pinhole-auth, icloudpd import, pinhole-probe | **`pinhole.icloud`**, Rust in pinholed (crate in `plugins/icloud/source`) | See above |
| Folder: walk, HEIC+MOV pairing, EXIF, ffprobe, thumbnails, folder favourites, copying in | **new `pinhole.folder`**, Rust in pinholed (crate in `plugins/folder/source`) | See above |
| Sources of your own | your plugin, out of process | The source protocol |
| `pinhole <command>` (status, sources, show, remove, sync, add, storage) | core | Generic |
| `pinhole sign-in` / `sign-out` | core CLI, sending `kind: "icloud"` | A terminal has no plugin QML. Says "iCloud is turned off" when it is |
| Help, tours, the new-plugins card, undo/redo, keys, theme, translations, bench, self-tests | core | Pinhole's frame for everything else |
| Series, Screenshots, On this day, Calendar, Sheet, Send, Info, Colours, Theme, Frame, Background | plugins, as now | Send moves to `fetch "jpeg"` |
| pinhole-fake | core | A library no source fills, for the grid |

## The plugin API, version 3

`Plugins::api` goes from 2 to 3. Nothing in API 2 changes or goes away.
`pinhole plugin check` warns when a plugin uses any of the following but
its manifest asks for `api` below 3. `Plugins::apiTwoUses` becomes
`Plugins::apiUses(dir) -> QMap<int, QStringList>`.

### Manifest

```json
{
  "schemaVersion": 1,
  "api": 3,
  "id": "pinhole.icloud",
  "sources": [{ "kind": "icloud" }]
}
```

- `sources`: the source kinds this plugin owns. In a built-in plugin, a
  `kind` with no `provider` names a kind compiled into pinholed (`icloud`,
  `folder`). Anywhere, `{"kind": "<id>[.<name>]", "provider": "<program in
  its folder>"}` is an out-of-process source (phase A4). `manifestProblem`
  checks the following: an in-process kind is allowed only in
  `builtInDir()`; a provider path is relative, has no `..`, and is
  executable; a provider's kind is the id or starts with the id and a dot.
- `Plugins::at(id)` adds `sources` (the kinds), so the window knows which
  plugin owns which kind even while it is off.

### Marks: `PluginMark`

A C++ type, `PluginMark : QObject` in `plugins.h`, registered in
`Pinhole.Plugin`. `PinholePlugin` gets `property list<QtObject> marks`.

```qml
marks: [
    PluginMark {
        name: "live"              // unique in the plugin; its id is "<plugin id>/<name>"
        text: qsTr("Live Photos") // its switch in the side panel's "In the grid"
        icon: "live"              // a Pinhole shape (video, live, heart, stack), or one Nerd Font glyph
        colour: "yellow"          // a theme colour's name (red, green, yellow, blue, magenta, cyan, accent) or "#rrggbb"
        order: 30                 // left to right in the corner; favourite 10, video 20, live 30, stack 90
        field: "live"             // the photos whose library field is true: "live", "favorite", "video"
        // or:
        photos: []                // library positions, followed through a reload as `hidden` is
        defaultShown: false       // its switch until it is turned (Live: off, as today)
    }
]
```

- `field` and `photos` are alternatives (`field: ""` means `photos`). A
  mark with `field` costs nothing when the library changes. With
  `photos` (positions, or ids, resolved as given), `Plugins` follows the
  positions through `libraryReloaded` until the plugin gives them again, as
  `hiddenOf` does; `Plugins::marksOf(id)` says where they are now, by name.
  As built (B1), each `PluginMark`'s own `photosChanged` is "given again",
  so nothing is gathered in QML.
- Left out: `order` 50, `colour` "accent", `defaultShown` true.
- At most 32 marks in all (`quint32` per photo), and at most 4 per plugin.
  Beyond that, `pinhole.warn` says so and the extra marks are left out. So
  is a mark that is not right (its name, field, icon or colour), and the
  problem is unsaid once its marks are right.
- The core's own: `pinhole/video` (shape video, cyan, order 20, field video)
  and `pinhole/stack` (shape stack, magenta, order 90; set from `m_stackOf`
  as today, field `"stack"`, which only the core's has).
- The theme gains `green` and `blue` (Tokyo Night's when the theme has
  none), for the colour names.

Wiring (no QML per frame, no QML gathering per change):

- `Main.qml` calls `plugins.setMarks(id, item.marks)` as a plugin loads and
  on its `marksChanged`, and `plugins.setMarks(id, [])` as it unloads.
  `Plugins` connects to each `PluginMark`'s change signals itself.
- `Plugins` exposes `Q_PROPERTY(QVariantList markKinds)`, sorted by order:
  `{id, plugin, text, icon, colour, order, field, defaultShown}`.
  `SidePanel.qml` lists those for the switches. `Main.qml` binds
  `grid.markKinds` to them, with `colour` resolved through `theme`.
- `Plugins` calls `PhotoGrid::setMarkBits(std::vector<quint32>)` with the
  bits from `photos` marks, one word per library position. It rebuilds
  them only when a list or the set of kinds changes, or after a reload.
- `PhotoGrid` loses `marks` and `markColours`. It gains `Q_PROPERTY(
  QVariantList markKinds)` and `Q_PROPERTY(QStringList marksShown)`, and
  works out `m_videoMask`, `m_liveMask`, `m_favoriteMask` and `m_stackMask`
  from the kinds. Step 3 becomes `bits = (item.video ? m_videoMask : 0) |
  (item.live ? m_liveMask : 0) | (item.favorite ? m_favoriteMask : 0) |
  (stacked ? m_stackMask : 0) | m_markBits[i]`, then `bits & m_shownMask`.
  The atlas strips are built for `m_markKinds.size()` slots and rebuilt
  only when the kinds or colours change (`m_markKindsVersion`).
  `PhotoGrid::marksOn(index)` gives the ids a photo's cell carries, as step
  3 works them out, for tests and self-tests.
- `marks.h`: `enum Mark` gives way to `struct MarkShape { QString builtin;
  QString glyph; }` and `MarkShape markShape(const QString &icon)`.
  `shape()` keeps the four drawn shapes. It draws a glyph with the theme's
  font as a `QPainterPath::addText`, filled for plain and stroked as the
  tube. `plainMark`, `neonMark` and `markImage` take a `MarkShape`.
  `MarkProvider` serves `image://mark/<icon>` (a glyph percent-encoded).
  "One glyph" is one grapheme: a glyph past the first plane, or a flag of
  two code points, is one. `"favorite"` names the heart too, as the side
  panel's images did.
- The switches move out of QML `Settings` into a C++ `MarkChoices`
  (`app/src/markchoices.{h,cpp}`, a context property `markChoices`). It
  owns `[grid]` in `window.conf`: `Q_INVOKABLE bool shown(QString id, bool
  byDefault)`, `Q_INVOKABLE void setShown(QString id, bool on)`, and
  `Q_PROPERTY(int revision)`. Its migration is below.

### The viewer: `viewerLayer`, and actions that can say more

`PinholePlugin` gets `property Component viewerLayer`. `PhotoViewer.qml`
has a `Repeater` over the plugins with one. Each makes its layer inside
`frame`, above `longExposureImage`'s place and the `VideoOutput`, so the
layer is zoomed, panned, grown and clipped with the photo. It is made
afresh for each photo shown (a `Loader` whose `sourceComponent` is set to
null on `viewer.indexChanged` and back on `Qt.callLater`). It is made with
`{ photo: viewer.index }`, and Pinhole sets its `shown` property: true
while that photo is shown, false the moment `closeViewer()` starts
shrinking or the photo changes. A layer stops its motion on `shown` false.
Layers stack in plugin order.

As built (B2): not a `Loader` but `Component.createObject(slot, { photo,
shown: true })` in a `Repeater` slot per plugin, as a Loader cannot give
a component initial properties, and `photo` must be there before the
layer's `Component.onCompleted`. The root declares `photo` and `shown`.
Photos passing quicker than 150 ms apart (an arrow held, the strip
scrubbed) get no layer until it stops on one, as `Viewer::show` waits to
fetch (`kPassing`): a layer that fetches as it is made would otherwise
queue a fetch per photo passed. A layer that cannot be made is said under
its plugin (`Plugins::layerFailed`). The layers are gathered with the
actions in `collectPlugins` (`pluginLayers`), and made again only when
one comes or goes.

`PluginAction` gets:

- `order` (int, 0): actions in one place are sorted by it, then by plugin.
  In select mode's bar, below 0 comes before Pinhole's own Copy, 0 and up
  after. In the viewer, every plugin action is before Copy already.
  Favourites uses -20 and Live -10, which keeps today's heart, Live, …,
  Copy.
- `hint` (string): in a group's menu, a word at the right of its row in
  place of its key ("iPhone").
- `complain(text)`: a few words under its button for four seconds, the
  button shaking, as `liveNotice` and Copy do now. `PluginGroup.qml` and
  `MoreGroup.qml` draw it, the code moved from `PhotoViewer.qml`'s
  `liveNotice`. Folded into More ▾, it shows under More.

As built (B2): `complain` emits `complained(text)`. The chip is
`Complaint.qml`, made by a `MenuGroup` the first time one of its buttons
complains (`MenuGroup.complain`, `hush`), under the bar, or over it for a
bar at the bottom (`upward`, select mode's); its button shakes by a
`Translate` on the group's button slot. The viewer's heart and Live
button use the same, so `liveNotice` is gone already in B2. A grid
action's complaint shows under its own button. `ActionBar.hush()` takes
them away when another photo shows. The sort by `order` is by position
too: V4's `Array.prototype.sort` is not stable, and an unstable sort
reordered the plugins' buttons and their groups' keys.

`pinhole.fetch(photo, kind, options)`: a third argument that is an object,
not a function, is `options`. `{ stream: true }` gives a URL for a
`MediaPlayer`: `http://127.0.0.1…` while it downloads (the service's
`stream`), or `file://…` once cached. `kind` may now be `jpeg` (above) and
the effects (`loop`, `bounce`, `long_exposure`).

`library.item()` gains `effect` (`"loop"`, `"bounce"`, `"long_exposure"`,
`""`), and `library.field("effect")` works.

### State and cache: `pinhole.state`, `pinhole.cache` (B1b)

Not in the design as first written; added once it showed that plugins kept
in `plugins.json` what is not a setting. That file is the user's choices,
and pinholed reads it again whenever it changes (A2), yet Screenshots
wrote its 30 KB `found:` list there on a launch on a real library, Series
its edits, Info its panel left open, Send its devices, and the window
every tour seen.

- `pinhole.state` and `pinhole.cache` are `QQmlPropertyMap`s, as
  `pinhole.settings` is: read, bind, assign, `null` takes a key out. The
  smallest shape that fits: no new idiom for plugin authors, bindable,
  and `keys()` for pruning. No promise: nothing is waited for, as the
  value is the plugin's at once; a failed write is said under the plugin
  (`PluginHost::report`), as a setting's is.
- `PluginStore` (`app/src/pluginstore.{h,cpp}`): one JSON object per
  plugin, `~/.local/state/pinhole/plugins/<id>/state.json` and
  `~/.cache/pinhole/plugins/<id>/cache.json` (beside the `cacheDir`s,
  which stay for files). Read when the plugin first asks, so before the
  first frame for one that reads it as it loads, as Screenshots must.
  Writes: an unchanged value is no write; changes in one turn of the event
  loop are one write, of the whole file, through `KeptFiles`: a
  one-thread `QThreadPool`, in order, each a `QSaveFile` (beside it, then
  renamed over it). Nothing is written on the window's thread, so no
  write holds up a frame. `Plugins::flush()` waits for all of it, and
  `~Plugins` calls it.
- A file per plugin, not per key: one read for all a plugin keeps, and a
  key may hold any characters (`found:folder:/home/me/Pictures`).
- The tours seen are the window's state, not a choice:
  `~/.local/state/pinhole/toured.json`, written the same way.
- `apiUses`: `pinhole.state` and `pinhole.cache` are API 3's.
  Screenshots, Series, Info and Send ask for `api: 3`.

### Favourites: `pinhole.setFavorite`

`pinhole.setFavorite(photos, favorite)` returns a promise of
`{ changed }`. `photos` are positions or ids. It is `Favourites::set`
with no history step: the photos change in memory at once (grid, viewer,
`library.item().favorite`), then pinholed's `set_favorite` runs with
`source: Library::source()`. If that fails, they are put back as they were,
and the promise is rejected with the service's `code` (`unsupported`,
`other_source`, `needs_sign_in`, `unavailable`, `not_found`). The plugin
records the undo step with `pinhole.did` itself.

### Sources: `PluginSource` and `pinhole.sources`

`PinholePlugin` gets `property list<QtObject> sources`, each a
`PluginSource` (QML, `app/src/PluginSource.qml`):

```qml
PluginSource {
    kind: "icloud"                     // one of its manifest's
    // What the window calls the photos' place in a sentence:
    // "Nothing in iCloud yet", "Adding 3 files to Pictures…".
    place: source => qsTr("iCloud")
    // Adding a source: what was found here, best first, ready on 1–5.
    offers: [ { title, detail, triggered: () => … } ]
    // …and the ways in that are always there ("Sign in to iCloud…").
    ways: [ { title, triggered: () => … } ]
    // A source's row in the side panel: a line when it needs something,
    // and its buttons beside Remove (Sign out; Sign in again).
    note: source => ""
    actions: source => []              // [{ text, key, triggered }]
    // What "Sign in" on the window's card does; null for a kind that never needs it.
    signIn: null                       // source => { … }
}
```

`source` is a `sources.list` entry (id, kind, plugin, name, count, thumbs,
and the kind's own fields such as `account` or `path`), with `shown` and
`health` added for the one shown.

`pinhole.sources` is a new `PluginSources` object per host. The list and
state are read-only mirrors of `Sources`. Every method naming a kind
rejects `not_yours` for a kind not in this plugin's manifest.

| | |
|---|---|
| `list`, `current`, `health`, `progress`, `rounds` | as `Sources` has them |
| `discovered(kind)` | what `discover` found for that kind (`{icloudpd: […]}`, `{folders: […]}`); `discoveredChanged()` |
| `discover()` | look again |
| `add(kind, entry)` | a promise of the source as shown: `set_source` with `kind` |
| `show(id)` | show it |
| `signIn(kind, params)` | a promise of `{ state: "signed_in" \| "needs_code" }`; rejected with the provider's codes |
| `submitCode(kind, params)` | a promise of `{ state }` |
| `signOut(id)` | a promise of the sources left |

`pinhole.chooseFolder(title)` is a promise of a folder chosen in the
desktop's own picker (`PortalFolderPicker`), rejected `cancelled`.

Where the window uses them:

- `Main.qml` gathers `pluginSources` in `collectPlugins` (by kind).
  `win.placeName` becomes the shown source's `place(source)`, or its
  `name`.
- `SourcePicker.qml` lists every `offers` entry from the plugins that are
  on (at most five, on 1–5), then every `ways` entry. The first run and
  Add a source both use it. When an entry is triggered, the picker closes.
  The plugin may then cover the window with a panel of its own
  (iCloud's sign-in). The first-run picker hides while a plugin covers
  (`shown: win.firstRun && !win.pluginCovering`), so the `"plugin"` key
  layer is the plugin's.
- `SidePanel.qml`'s source rows keep name, detail, rounds and Remove, and
  add the `note` line and the `actions` buttons (keys in the `"side"`
  layer, as Sign out's are now).
- The trouble card in `Main.qml` shows when `service.health ===
  "needs_sign_in"` and the shown kind's `signIn` is set. Its title is
  `"Not signed in to %1"` with the source's `account` or `name`, and its
  ⏎ calls `signIn(source)`.

## pinholed

### Source kinds (`core/src/kinds.rs`)

```rust
pub trait Kind: Send + Sync {
    /// As settings.json names it: "icloud", "folder", "me.immich".
    fn kind(&self) -> &str;
    /// The plugin whose switch it follows: "pinhole.icloud".
    fn plugin(&self) -> &str;
    /// An entry of this kind in its usual form, or None.
    fn normal(&self, entry: &Value) -> Option<Value>;
    /// The entry's source id: "icloud:<apple id>", "folder:<path>".
    fn id_of(&self, entry: &Value) -> String;
    /// Entries known without the settings naming them: the Apple IDs signed in to here.
    fn implicit(&self) -> Vec<Value> { Vec::new() }
    /// Without a list, its sources are only its implicit ones: one shown
    /// that is not among them is not added (iCloud true; a folder's is added).
    fn implicit_only(&self) -> bool { false }
    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>>;
    /// The `sources` event's own fields for it: name, and account or path.
    fn describe(&self, entry: &Value) -> Value;
    /// `set_source` with this kind: the entry for these params, checked.
    fn entry_for(&self, params: &Value) -> Result<Value, Failure>;
    /// What `discover` found that could be added.
    fn discover(&self, added: &[Value]) -> Value { Value::Null }
    fn needs_sign_in(&self) -> bool { false }
    /// The extensions `add` takes, for the `kinds` event.
    fn accepts(&self) -> &'static [&'static str] { &[] }
    fn sign_in(&self, params: &Value) -> Result<SignedIn, Failure> { Err(Failure::unsupported(…)) }
    fn submit_code(&self, params: &Value) -> Result<SignedIn, Failure> { Err(Failure::unsupported(…)) }
    /// bad_request, as `sign_out` of a folder was before kinds.
    fn sign_out(&self, entry: &Value) -> Result<(), Failure> { Err(Failure::bad_request(…)) }
    /// Taken out of the sources: what it kept apart goes (an Apple ID's session).
    fn removed(&self, entry: &Value) {}
}
pub enum SignedIn { Entry(Value), NeedsCode }
pub struct Failure { pub code: String, pub message: String }

pub struct Kinds { all: Vec<Arc<dyn Kind>>, on: RwLock<HashSet<String>> }
impl Kinds {
    pub fn new(all: Vec<Arc<dyn Kind>>) -> Kinds;      // all on until refreshed
    pub fn refresh(&self, switches: &HashMap<String, bool>) -> bool; // changed?
    pub fn of(&self, kind: &str) -> Option<Arc<dyn Kind>>; // on or not
    pub fn of_entry(&self, entry: &Value) -> Option<Arc<dyn Kind>>;
    pub fn on(&self, kind: &str) -> bool;
    pub fn all(&self) -> &[Arc<dyn Kind>];
    pub fn id_of(&self, entry: &Value) -> String;      // "none" for no kind known
    pub fn report(&self) -> Value;                     // the `kinds` event
    pub fn shown(&self, settings: &Settings) -> Option<Value>;
    pub fn source_for(&self, entry: Option<&Value>) -> Result<Arc<dyn Source>>;
}
```

- The built-in kinds are put together by pinholed, not the core (since
  A3): `pinholed::builtin_kinds(auth) -> Kinds` (`core/daemon/src/lib.rs`)
  is `Kinds::new(vec![IcloudKind, FolderKind])`. Until A3 it was
  `Kinds::builtin(auth)`.
- `IcloudKind` (`icloud/kind.rs`, since A3 pinhole-icloud's `kind.rs`)
  holds what is now in `service.rs`: the
  `signing_in` map of `ProcessProvider`s, sign-in through `import_icloudpd`
  or a password, `submit_code`, `sign_out` (cookies removed), `removed`
  (`state_dir` removed), `implicit` (`auth::accounts()`), the
  `needs_sign_in` check in `entry_for`, `describe` (with `account_name`),
  and `discover` (icloudpd's stems).
- `FolderKind` (`folder.rs`, since A3 pinhole-folder's `lib.rs`) has
  `entry_for` (absolute, a folder,
  trailing `/` trimmed), `describe` (`display_name`), and `discover`
  (`discover::candidates`).
- `Source` gains `fn details(&self) -> Value` (`{"account": …}` or
  `{"path": …}`). `source_info` uses it in place of the
  `strip_prefix("icloud:")` and `strip_prefix("folder:")` tests.
- `storage::read_settings(path, &Kinds)`. `resolve` normalises entries
  through `Kinds::of(kind).normal()`, and fills a missing `sources` from
  every kind's `implicit()`, in registry order (iCloud first, as now),
  then the source shown if its kind is not `implicit_only` (the folder
  shown, as before). It keeps entries of kinds that are off.
  `source_entry`, `show_source` and `remove_source` take the `Kinds` too,
  for `id_of`.
- **Which source is shown**: `Kinds::shown(&Settings) -> Option<Value>`.
  That is `settings.source` if its kind is on, else the first source whose
  kind is on, else none. `"source": null` stays none, as before kinds.
  settings.json is never rewritten for this, so turning a plugin back on
  shows what was shown before.
- `core/src/plugins.rs`: `plugins_path()` (`$XDG_CONFIG_HOME/pinhole/plugins.json`)
  and `switches() -> HashMap<String, bool>` read `enabled`. A built-in
  kind is on unless its plugin's switch is `false`. A provider kind is on
  as its plugin is in the window (phase A4, as built): a plugin of your
  own only when its switch is `true`, a built-in one (there is none yet)
  unless it is `false`.
- `sync::source_for` becomes `Kinds::source_for(entry)`. `service.rs`
  keeps no `icloud` or `folder` names: `sign_in` without a `kind` is the
  first kind that `needs_sign_in`.
- `service::Config` loses `auth` (the provider is `IcloudKind`'s) and
  gains `kinds: Kinds` and `plugins: PathBuf` (plugins.json, as `settings`
  is a path given, so a test has its own). `Shared` reads both from its
  config, and keeps `plugins::stamp` (mtime and size) of the file last
  read.
- `sync_round` runs under `std::panic::catch_unwind`. A panic is logged,
  the health becomes `failing`, and the loop goes on. The round held
  `library_lock`, which the panic poisons, so it is cleared
  (`clear_poison`; the index's own transactions roll back what was half
  written). A `progress` under way says `done`. The source, whose own
  locks a panic may have poisoned too (iCloud's clients), is made afresh
  from its settings entry and put in place quietly (the same id: no
  `source` event). `core/tests/panic.rs` proves it with a source whose
  first listing panics: `failing`, then `sync`, and the next round lists
  and makes the thumbnail.

### Protocol (still `protocol: 1`; additive)

| Change | |
|---|---|
| event `kinds` | `{"event": "kinds", "kinds": [{"kind": "icloud", "plugin": "pinhole.icloud", "on": true, "needs_sign_in": true, "accepts": […]}, …]}`, sent to a window as it connects (after `sources`) and when a switch changes |
| method `kinds` | the same, as a result |
| method `plugins_changed` | `{}`. Reads plugins.json again; if a kind went off, shows `Kinds::shown`; broadcasts `kinds`, then `source`, `library_moved`, `sources` if they changed. The window sends it after `Plugins::setEnabled`. Each sync round also checks the file's mtime, for edits by hand |
| `sources` entries | add `plugin`. Only sources of kinds that are on are listed. `current` is the source shown, which is `Kinds::shown` once the switches are followed |
| `source` event | adds `plugin`. The rest comes from `details()` |
| `set_source` | `kind` may be any kind that is on; else `turned_off`. So too `set_source`, `remove_source` and `sign_out` by the `id` of a source whose kind is off: nothing of a kind that is off is changed |
| `sign_in`, `submit_code` | take `kind`; none means `icloud`, so the CLI and older windows keep working. A kind that is off: `turned_off` |
| `discover` | adds `kinds: {"icloud": {"icloudpd": […]}, "folder": {"folders": […]}}` for kinds that are on; keeps `icloudpd` and `folders` at the top, as each kind's fields merged there (a kind that is off has none: a window of today reads a missing one as empty) |
| `fetch` `kind: "jpeg"` | see Conversion |
| error `turned_off` | a source kind whose plugin is off |

## The source protocol (phase A4, `docs/source-providers.md`)

A provider is a program in a plugin's folder. It talks JSON lines over
stdio, as `auth/PROTOCOL.md` does. It is started as `<provider>
--state-dir <dir>`, one process per source in use (as built: the one
shown, started by its first request and stopped when it is no longer
shown, removed or turned off), and one of its own for each of
`add_entry`, `discover`, `sign_out` and `removed`, and for a sign-in until
it is done. `ProcessKind` and `ProcessSource` (`core/src/process.rs`)
implement `Kind` and `Source` with it. `docs/source-providers.md` is the
protocol as built, for plugin authors; this section is the design, with
what changed as it was built:

- **No `describe`.** What it was to say is static, and pinholed needs it
  without starting anything (the `kinds` event goes to every window as it
  connects; `Source::name` is asked everywhere). So the manifest's
  `sources` entry says `needs_sign_in`, `accepts` and `rounds_every_secs`,
  the entry's own `name` names the source, and push is a `watch` that is
  not answered `unsupported`.
- **`unsupported` is an answer** for every optional method: `start` so is
  ready, `changes` so lists again, `watch` so has no push. The least
  provider is `add_entry`, `list` and `prepare` (the doc has one in Python
  and one in sh with jq, both run through pinholed as written).
- **Keys** are prefixed by pinholed with a hash of the source id and `~`
  (`f783eec2~<key>`, `f783eec2m~<key>` for one to make), so a provider's
  keys need only be its own and never meet another source's in the shared
  cache, and a made rendition is known by its key alone.
- **What is not the protocol stops it:** a stdout line that is not a JSON
  object with an `id` or a `note`, an answer with neither `"ok": true` nor
  `false`, a line over 4 MB. A line pinholed cannot attribute could have
  been a `put` of a listing, after which `remove_unseen` would take those
  assets away; so the request fails instead and nothing is removed. A put
  over 1,000 assets fails its request. An asset that is not right is left
  out and logged, and its id, if that much is right, counts as seen: what
  the library had of it stays.
- **Silence** is measured from the request or the provider's last line,
  whichever is later (an idle provider is not silent). Its stderr is at
  most 200 lines in 10 s in the log. `PINHOLE_PROVIDER_SILENCE_SECS`
  shortens the two minutes for the tests.
- **A plugin gone** (its folder moved away): its kind goes, with its
  processes, and its entries stay in the settings (`kinds::kept`: a kind
  and a key), as a kind turned off does, so it comes back with them.
- **A plugin whose manifest changed** gets a new `ProcessKind`; the source
  shown of it is made afresh, quietly (`follow`).

- Requests carry `id`. Lines with that `id` and `note` come before the
  answer. Lines with `note` and no `id` are the provider's own news.
- A settings entry of such a kind is `{"kind": "<kind>", "key": "<unique
  in the kind>", …its own fields}`. The source id is `<kind>:<key>`. The
  library is `library/<kind>/<key, made safe>`.

| Method | Params | Result, and notes before it |
|---|---|---|
| ~~`describe`~~ | | in the manifest instead (above) |
| `start` | `entry` | `{}` |
| `list` | `entry` | notes `{"note": "put", "assets": [Asset…]}`, `{"note": "progress", "done", "total"}`; then `{token, summary}` |
| `changes` | `entry`, `token` | notes `put` and `{"note": "removed", "ids": […]}`; then `{token}`, or error `token_expired` |
| `prepare` | `entry`, `wanted: [{asset, kind, name, key}]` | `{tickets: [{"path"} \| {"url", "headers"} \| {"error": {code, message}}]}` |
| `add` | `entry`, `file`, `files`, `motion` | notes `{"note": "sent", "sent", "bytes"}`; then `{id, duplicate, note}`, or an `AddError` code |
| `set_favorite` | `entry`, `ids`, `favorite` | `{}`, or `unsupported` |
| `watch` | `entry` | `{}`, then news `{"note": "changed"}` and `{"note": "listening", "on"}` |
| `discover` | `added` | anything; given to the window as `kinds[<kind>]` |
| `add_entry` | the window's params | `{entry}`: what `set_source` adds |
| `sign_in`, `submit_code`, `sign_out` | the window's params, or `entry` | `{state, entry}` |
| `removed` | `entry` | `{}`: forget what it kept for it |

- An `Asset` is `source::Asset` as JSON: `id`, `filename`, `item_type`,
  `taken`, `added`, `kind` (`photo` \| `video` \| `live`), `favorite`,
  `duration`, `orientation`, `has_location`, `size`, `effect`,
  `renditions: [{kind, name, key, width, height, size, file_type}]`.
  pinholed checks each before it calls `Index::put`. The `id` must be 1–512
  bytes. Kinds must be known. Each `key` must be 1–256 bytes, from
  `[A-Za-z0-9._~+/=-]`. A bad asset is skipped and logged, never written.
- A `thumb` or `medium` rendition may say `"make": true`. pinholed then
  makes it from the `original` ticket's file with the folder's renderer.
  That renderer moves to `core/src/render.rs` in this phase.
- Tickets: `path` is opened as a file, with byte ranges by seek. `url` is
  fetched with `reqwest` and the given headers, with byte ranges by
  `Range`. 401, 403, 404 and 410 mean `Expired`, as for iCloud. pinholed
  never passes bytes through the provider.
- Error codes map to health: `needs_sign_in`, `offline`, `failing`. Every
  other code means `failing`.
- A provider that is silent for two minutes during a request, or that
  dies, is killed. Its requests fail `unavailable`, and the next request
  starts a new one, at most once in 10 s. Its stderr goes to pinholed's
  log, each line prefixed with the plugin id. pinholed never logs the
  params of `sign_in` or `submit_code`.

## Migration

| Step | On disk | Migration | Tested on a copy |
|---|---|---|---|
| B1 marks | `~/.config/pinhole/window.conf` `[grid]` | `MarkChoices` reads `marks` once, when `markChoices` (JSON) is missing, and writes `{"pinhole/video": bit 1, "pinhole.live/live": bit 2, "pinhole.favourites/favorite": bit 4, "pinhole/stack": bit 8}`. `marks` is left as it is. Ids not in `markChoices` take `defaultShown`. With no `marks` either (a first run), nothing is written until a switch is turned, so every kind follows its `defaultShown` (as built: writing 13's switches then would freeze today's defaults for someone who never chose) | unit test with a temporary `window.conf` holding `marks=13`, `marks=15`, `marks=0` and no `marks`; then the app run with `XDG_CONFIG_HOME` on a copy of `~/.config` |
| A1 / B5 jpeg | `~/.cache/pinhole/compatible/` | made again on demand. Until B5 the window still writes it, so pinholed removes only what is unused for a day, as it starts, and logs it; B5 removes the folder with its writer. New JPEGs go to `cache/full/…/<key>-jpeg.jpg` (`cached_path(Want::Jpeg, full_key, "jpg", true)`). They count as `full` in the storage panel, with no new `KINDS` entry | pinholed run with `XDG_CACHE_HOME` and `XDG_DATA_HOME` on reflink copies |
| A2 kinds | `settings.json` (absent for Jacek), `plugins.json` (read only by pinholed) | none. `resolve` must give the same `Settings` as now for every input. That is a table test over today's cases, plus "no file at all, one Apple ID signed in". Nothing writes settings.json because a plugin was turned off | pinholed with `XDG_DATA_HOME`, `XDG_CONFIG_HOME` and `XDG_STATE_HOME` on reflink copies and `PINHOLE_SOCKET` of its own. `sources` lists his Apple ID with `count: 49079`. After a round, `meta.listed_at` and `change_token` are unchanged (no new listing). `enabled["pinhole.icloud"] = false` in the copy's plugins.json lists nothing, leaves the library's mtime as it was, and turning it back on shows it again with no listing |
| A3 crates | none | `pinhole-auth` keeps its state folder `~/.local/state/pinhole/auth/<apple id>/`. `auth::default_provider` looks in the checkout at `plugins/icloud/auth/pyicloud/pinhole-auth`; installed, it is still beside pinholed | as A2, and `pinhole sign-in` on a scratch state folder |
| A4 providers | new `~/.local/state/pinhole/sources/` | new, nothing to migrate | the test provider |
| B3, B4, B6, B7 new plugins | `plugins.json` | **none written.** The new built-ins have no `enabled` entry and so are on. They have no `tour`, so no new-plugins card shows. A test loads a copy of Jacek's plugins.json, runs the window, and checks that the file is byte for byte the same afterwards | `XDG_CONFIG_HOME` on a copy |
| B3 Live effect chosen | memory only (`m_chosen`) | none | |
| B1b state and cache | `plugins.json`; new `~/.local/state/pinhole/plugins/<id>/state.json`, `~/.cache/pinhole/plugins/<id>/cache.json`, `~/.local/state/pinhole/toured.json` | `Plugins::moveOutOfSettings`, as the window starts, before any plugin loads: `pinhole.screenshots` `found:*` to its cache; `pinhole.series` `edits:*` and `edits`, `pinhole.info` `shown`, `pinhole.send` `devices` to their state; `toured` to toured.json (merged with what is there). Only those plugins' keys (another plugin's `edits` is its own). The settings are read without them at once. One job on `KeptFiles`: the new files first, then `plugins.json` without them, only if those were written. If they could not be, the keys stay in `plugins.json`, and a setting changed meanwhile writes them back with it (`unmoved`). An emptied plugin's entry goes. With nothing to move, nothing is written | unit tests (the move and the next start writing nothing; a state folder that cannot be made); a window on a copy of Jacek's live `plugins.json` and real library |

Rules for every test on a copy:

- Copy with `cp -a --reflink=always`.
- Set `PINHOLE_SOCKET` to a socket of the test's own, never the default.
- Run windows on workspace 5, as CLAUDE.md says.
- Never let a scratch `HOME` reach the default socket. That once broke
  Jacek's window.

## Phases

There are two tracks, which touch different files: **A** (Rust, `core/`
and the Rust under `plugins/`) and **B** (the window, `app/` and plugin
QML). One A phase and one B phase can run at once. That is two builds at a
time: cargo for A, qmake for B. Within a track, the phases go in order.
Each phase:

1. lands on `main` alone and is installed with `bin/install-local`;
2. keeps `docs/` true (`plugins.md`, `sources.md`, `service.md`, README);
3. adds every new string to `pl.json` (Pinhole's, or the plugin's
   `i18n/pl.json`), and removes the ones it moves out of Pinhole's;
4. passes `cargo test --release --workspace` (A) or `app/bin/test` (B) and
   `pinhole plugin check` on each plugin it touches;
5. is measured against the baseline below. On a regression of more than
   5 %, the phase does not land until it is fixed.

### Phase 0: the yardsticks (no code)

On today's `main`, record:

- `PINHOLE_SELFTEST=hidden:<dir>`'s "first frame at N ms", five runs, on
  the real library;
- `app/build/pinhole --bench --library ~/.local/share/pinhole/fake`, the
  frame times per phase;
- `PINHOLE_SELFTEST=viewer`'s stage times;
- a reflink copy of `~/.config/pinhole`, `~/.local/state/pinhole` and
  `~/.local/share/pinhole/library/icloud/<apple-id>`, kept
  under `/tmp/claude-1000/baseline/` for the migration tests.

Every run goes through a script handed to Hyprland's workspace 5, as
CLAUDE.md says. Save the numbers in `/tmp/claude-1000/baseline/numbers.txt`.

### A1: the one JPEG, in pinholed

- `core/src/source.rs`: `Want::Jpeg` (`"jpeg"`). Not streamable, and not a
  source rendition.
- `core/src/service.rs`: in `fetch`, `jpeg` finds `full`. If its
  `file_type` is `public.jpeg`, it answers that as is (fetched as `full`).
  A video's is `not_found`. Otherwise `make()` grows a `Want::Jpeg`
  branch. It fetches `full` through `Out::Service`, then runs
  `core/src/jpeg.rs` at normal priority, renames the part, and answers
  `{path, type: "public.jpeg", rendition: "made"}`. `cached_path` maps
  `Jpeg` to the `full` folder with the `-jpeg` suffix.
- `core/src/jpeg.rs`: `magick -[0] -auto-orient -background white -alpha
  remove -alpha off -quality 92 jpg:<part>`, the `full` on stdin
  (`-auto-orient`: a TIFF, PNG or WebP with an orientation comes out
  upright; a HEIC comes upright from libheif already, and says so). Stdin, because a
  folder's file name can hold what ImageMagick reads as its own syntax
  (`%d` and `%o` are a frame's number: "50%off.jpg" is "500ff.jpg" to
  it; ImageMagick 7.1 reads `[1]` and `*` in the name of a file that is
  there as they are); `[0]` so an animated GIF makes one file, not
  `part-0`, `part-1`…; white so a see-through PNG is not black. Two at
  most at once: one ImageMagick of a 24-megapixel HEIC takes every core
  for 0.6–0.9 s, and a window may ask for fifty.
- `core/src/storage.rs`: `forget_compatible()`, as the shared service
  starts, removes what in `old_cache()/compatible` has not been used for a
  day, never the folder: until B5 the window still makes its JPEGs there
  (`outgoing.cpp`), and one made a moment ago may be on its way somewhere.
  B5 removes the writer, and with it the folder.
- `core/tests/service.rs`: a folder with a HEIC (made with `magick` in the
  test, skipped without it) and a JPEG. `jpeg` of the HEIC is a JPEG with
  the EXIF date kept (`magick identify -format %[EXIF:DateTimeOriginal]`).
  `jpeg` of the JPEG is the file itself. Three asks at once run one
  `magick` (a logging `magick` first on `PATH`) and all say `cached:
  false`; a fourth is `cached: true`; `storage` counts it in `full`; of
  the old `compatible` folder, a JPEG unused for two days is gone and one
  just made stays. `jpeg.rs` has a unit test: a name with `[1]*`, an
  animated GIF, a see-through PNG, a TIFF of orientation 6, a HEIC turned
  by its own transform, a file that is no photo.
- `docs/service.md`: the kind.
- Works alone: nothing asks for it yet.

### A2: source kinds, and the plugins' switches

- New: `core/src/kinds.rs`, `core/src/plugins.rs`, `core/src/icloud/kind.rs`.
- Changed: `core/src/service.rs` (above: `signing_in`, `sign_in`,
  `submit_code`, `sign_out`, `set_source`, `remove_source`,
  `sources_report`, `source_info`, `discover`, and `follow_settings`
  reading plugins.json; `Shared` gains `kinds: Kinds`), `storage.rs`,
  `sync.rs`, `discover.rs`, `folder.rs`, `source.rs` (`details`),
  `icloud/mod.rs`, `bin/daemon.rs`, `core/tests/service.rs`, `docs/service.md`,
  `docs/sources.md`.
- Tests:
  - unit tests for `resolve` (every case of today, unchanged answers, plus
    entries of a kind that is off);
  - `Kinds::shown`;
  - `plugins::switches` (missing file, bad JSON, `false`, `true`);
  - in `core/tests/kinds.rs` (a test binary of its own, as it sets HOME
    and the XDG folders), a folder source with
    `enabled["pinhole.folder"] = false` written to the test's
    `XDG_CONFIG_HOME`. After `plugins_changed`: `sources` is empty, a
    `kinds` event has `on: false`, `current` is null, and the library
    folder and its `change_token` are untouched. Turned on again: shown,
    with no listing (`listed_at` unchanged), turned on by a hand edit that
    the next round finds; and `turned_off` for whatever is asked of it;
  - the real-library check on a copy (Migration, A2);
  - `core/tests/panic.rs`: a panicking round, then the next one (above).
- Also in this phase, a bug A1 found: the folder source gave ImageMagick
  `"<path>[0]"`, so a file or folder named with a `%` (`50%off.jpg`,
  `Sale 50%d`) got no thumbnail. Every place a file path reaches
  ImageMagick or ffmpeg now passes it so its syntax cannot misread it:
  - stdin (and stdout for what is made), as `jpeg.rs`: `folder.rs`'s
    thumbnails and mediums, `plugins/info/pinhole-info`,
    `plugins/theme/pinhole-theme`, `plugins/background/background`,
    `pinhole-send jpeg`, and `Outgoing::place` (B5 replaces the last two
    with `fetch "jpeg"`);
  - with every `%` doubled where magick is given several files on its
    command line through `pinhole.run`: Sheet (`Layout.magickFile`, its
    `identify` and its sheet) and Theme's palette (`Palette.magickArgs`);
  - ffmpeg: the long exposure's vidstab transforms are named in the
    filter graph by their file name alone, ffmpeg running in their folder,
    as a cache folder's path (the person's to choose) may hold `,` `;`
    `[`; and the long exposure is written with `-update 1`, so a `%d` in
    its path is no frame number. ffmpeg and ffprobe otherwise get absolute
    paths of files (never a protocol, and the `mov` demuxer reads no
    patterns), checked with such names.
  - Tests: `folder::tests::thumbnails_are_made_whatever_the_files_are_called`
    (`50%off.jpg`, `IMG [1]*.png`, `x%03d.JPG`, `heic:odd.jpg`,
    `@list.jpg` and a video, in a folder `Sale 50%d [1]` beside a
    `500ff.jpg` that the old call read instead; it fails on the old code),
    `effects_are_made_from_the_motion` in a folder
    `Photos, 50%d [old]; 'x'`, and the theme, background and info checks.
- Works alone: the window ignores the new fields, and `sign_in` without
  `kind` is iCloud.

### A3: the built-in sources' Rust moves into their plugins

- `core/Cargo.toml` becomes a workspace root with
  `members = ["daemon", "../plugins/icloud/source", "../plugins/folder/source"]`
  and the same `default-members` (with `.`, the core itself). Each member
  outside `core/` sets `package.workspace = "../../../core"`. The versions
  are the workspace's (`[workspace.dependencies]`), each crate taking what
  it uses. There is one `core/Cargo.lock`, regenerated offline (only the
  new packages added; no version moved) and committed. The target folder
  stays `core/target`, so `core/target/release/pinholed` stays where
  `serviceclient.cpp` and `bin/install-files` look for it.
- **The layout, decided:** the workspace's root stays `core/`, with
  members outside it. Cargo took it as it is, lock file and all, so the
  fallback (a root at the top of the checkout with `target-dir =
  "core/target"`) was not needed, and would have been worse: a
  `Cargo.toml` at the top would make the series finder and the send
  sender (their own crates, built with `--manifest-path`) find a workspace
  they are not in, so each would need an `exclude`, and `Cargo.lock` would
  move. `cargo` run inside a plugin's `source/` finds the workspace through
  `package.workspace` and builds into `core/target`.
- `git mv core/src/icloud plugins/icloud/source/src` (`mod.rs` becomes
  `lib.rs`). This makes crate `pinhole-icloud`, a lib, with `pinhole-probe`
  as its bin (`src/bin/probe.rs`). `git mv core/src/folder.rs
  plugins/folder/source/src/lib.rs` makes crate `pinhole-folder`, and
  `add::copy_into` moves into it (`copy.rs`). `core/src/discover.rs` is
  split between them: icloudpd's stems go to pinhole-icloud's
  `discover.rs`, the folders to offer to pinhole-folder's. The core has no
  `discover.rs`, `folder.rs` or `icloud/`.
- `git mv auth plugins/icloud/auth`. `auth::default_provider` looks in the
  checkout at `plugins/icloud/auth/pyicloud/pinhole-auth`.
  `bin/install-files` still installs it as `$PREFIX/bin/pinhole-auth`,
  beside pinholed, where the installed pinholed finds it (`--auth`, else
  `$PINHOLE_AUTH`, else the checkout's, else beside it, else `PATH`).
- `pinholed` moves to package `core/daemon` (package and bin `pinholed`),
  which depends on `pinhole-core`, `pinhole-icloud` and `pinhole-folder`.
  It has a lib too, `pinholed::builtin_kinds(auth)`, which builds
  `Kinds::new(vec![IcloudKind, FolderKind])`, so its `main.rs` and its
  tests make the same kinds. `pinhole-core` no longer depends on `p256`,
  `tungstenite`, `base64`, `url`, `kamadak-exif`, `chrono` or `serde`. It
  keeps `reqwest`, which A4's URL tickets use. `pinhole-fake` stays the
  core's.
- Tests that use `FolderSource` or the built-in kinds move out of the
  core, unchanged but for their paths: `core/tests/*.rs` to
  `core/daemon/tests/`; `sync.rs`'s three over a folder to
  `plugins/folder/source/tests/sync.rs`; `storage.rs`'s folder library
  names to `plugins/folder/source/tests/libraries.rs`; the `Kinds` and
  `resolve` table tests over the real kinds to
  `core/daemon/tests/builtin_kinds.rs` (`storage::resolve` is `pub` for
  them). In the core, the storage tests' second library is `library/cloud`,
  a read of only the location uses `Kinds::new(vec![])`, and
  `rounds_event`'s test has a stub source. 74 tests before, 74 after.
- `bin/install-local`: `cargo build --release --locked --workspace
  --manifest-path core/Cargo.toml`. `pkgbuild/PKGBUILD`: the same. README:
  the layout. `docs/sources.md` and `docs/service.md`: where the crates
  are and where pinhole-auth is found.
- Until B6 gives them their QML, `plugins/icloud` and `plugins/folder` hold
  only Rust and no `manifest.json`: no plugin, as `Plugins::scan` already
  has it (it skips such a folder), so the window loads nothing new and
  `bin/install-files` copies nothing of them. The app's tests that walk
  `plugins/` (the translations, API 2's uses) skip such a folder too.
- Tests: `cargo test --release --workspace`. `makepkg -f` in a scratch
  copy of the checkout builds, as an ordinary user, with no install, and
  its package has `usr/bin/pinholed` and `usr/bin/pinhole-auth`. `cargo
  build --locked --workspace` from a fresh clone of the branch.
  `bin/install-local`, then the installed pinholed serves the real library
  as before (49,079, no listing), and `pinhole sign-in` on a scratch state
  folder reaches pinhole-auth (its "no icloudpd session" answer, no
  password given). `grep -rn "icloud\|Apple" core/src` finds nothing
  outside comments that name the plugins (`pinhole.icloud`,
  `plugins/icloud/source`).

### A4: sources of your own, out of process

- New: `core/src/process.rs` (`ProcessKind`, `ProcessSource`),
  `core/src/render.rs` (moved from the folder crate), `docs/source-providers.md`,
  `core/daemon/tests/provider.rs`, and
  `core/daemon/tests/provider/pinhole-test-source`. The test source is a
  `python3 -I` script that serves a temporary folder of JPEGs with `path`
  tickets, needs a "sign-in" (any password `"ok"`), and pushes `changed`
  when a file is added.
- `core/src/plugins.rs`: scans `builtInDir` (as `Plugins::builtInDir`
  finds it, from the exe) and `personalDir` for manifests with `sources`
  and `provider`. Personal plugins override built-ins by id.
- `kinds.rs`: `Kinds::refresh` adds and drops provider kinds as plugins
  come, go and are switched.
- Tests, in `provider.rs`:
  - list, then thumbnails made, then fetch of `original` by path;
  - a bad asset skipped;
  - sign-in and its error codes;
  - the provider killed mid-list (`unavailable`, then a fresh one);
  - push starting a round;
  - a kind not namespaced by its id refused;
  - turned off: no process left running.
- Works alone: no built-in uses it.
- As built:
  - The test source is a plugin folder, `core/daemon/tests/provider/`
    (manifest, an empty `Main.qml`, the script), so B7's `pinhole plugin
    check` has a folder to check. Its thumbnails are `url` tickets from a
    web server of its own that wants the header it gives (or `path`, by
    the entry), its mediums `make: true`, its originals `path`. It asks a
    code for the password "two-factor", keeps favourites, takes files
    added, and misbehaves on request (a `.misbehave` file in its folder).
  - `provider.rs` runs pinholed itself (`CARGO_BIN_EXE_pinholed`) with
    every folder in a scratch one and a socket of its own, and drives it
    as a window: 300 photos listed with thumbnails by `url` and `path`,
    mediums made, originals and fulls fetched, changes pushed and followed
    (5 new, 3 gone, 1 edited, with no listing), a file added, a favourite,
    sign-in with `wrong_password`, `needs_code`, `wrong_code` and the
    code, nothing of it in the log, a second source added and removed
    (its library gone, the provider told), sign-out; the provider hanging
    (stopped after the silence), crashing (started at most once in 10 s,
    health `failing`), sending a line that is not JSON, one over 4 MB, a
    put of 1,001, lines for other requests, a removal in a listing, and
    ten assets that are not right: the library byte for byte as it was
    (`PRAGMA integrity_check` too) where nothing could be written, and
    only what is right where it could; its stderr flooding the log; kinds
    outside their plugins' names refused; the plugin turned off (no
    process left, library and state untouched, `turned_off`), on with no
    listing, taken away and back; and 50,000 assets through the protocol
    (0.6 s), the provider never holding a file of the library open.
  - `render.rs` takes the file and whether it is a video; the folder
    crate's `render` calls it.

### B1: marks from plugins (API 3 begins)

- Changed: `app/src/marks.{h,cpp}`, `photogrid.{h,cpp}`, `plugins.{h,cpp}`
  (`PluginMark`, `setMarks`, `markKinds`, `marksOf` following through
  `libraryReloaded`, `api = 3`, `apiUses`), `PinholePlugin.qml` (`marks`),
  `Main.qml`, `SidePanel.qml` (the "In the grid" switches from
  `plugins.markKinds`, with `markChoices`), `main.cpp` (`markChoices`),
  `sources.pri`, `resources.qrc`, `app/tests/pinhole_tests.cpp`,
  `selftestrun.cpp` (`neon:`), `docs/plugins.md`, `app/src/i18n/pl.json`.
  As built: `theme.{h,cpp}` (green, blue), `selftest.cpp` (`reload` also
  checks that every plugin mark stayed on its photo); `resources.qrc`,
  `neon:` and `pl.json` needed nothing (the side panel's four words were
  there already, and the warnings are English, as `warn`'s are).
- New: `app/src/markchoices.{h,cpp}`.
- Until B3 and B4, the core itself provides `pinhole.live/live` and
  `pinhole.favourites/favorite` (field marks, as today). That way the
  migration happens once, with the final ids. A plugin mark of the same id
  takes the stand-in's place, so B3 and B4 only delete them.
- Proof as built: a window offscreen (`QT_QPA_PLATFORM=offscreen`) does
  not draw the grid's photos, so `neon:` ran as a Wayland window on
  workspace 5, whose grabs do draw it (it steps its own frames). Frames of
  main and B1 differ only where a medium rendition arrived at another
  moment, as two runs of main do.
- Tests:
  - `MarkChoices` migration (above);
  - bits from `photos` followed through a reload;
  - 32 slots at most;
  - `--bench` on the fake 100k against the baseline (frame time p99
    within 5 %);
  - `PINHOLE_SELFTEST=neon:<dir>` images look the same as on main;
  - a test plugin in a scratch `PINHOLE_BUILTIN_PLUGINS` with a glyph mark
    on every tenth photo, drawn plain and neon;
  - first frame within 5 %.

### B1b: a plugin's state and cache, out of plugins.json

Added after the design, on the window track (see "State and cache" in the
API above).

- Changed: `plugins.{h,cpp}` (`PluginHost::state`, `cache`,
  `Plugins::moveOutOfSettings`, `unmoved`, `flush`, `stateRoot`,
  `cacheRoot`, `touredPath`, toured out of `m_config`, `writeConfig`
  waiting for the move), `sources.pri`, `selftest.cpp` (`otherseries`
  reads Series' state), `app/tests/pinhole_tests.cpp`,
  `app/src/i18n/pl.json` (two warnings), `docs/plugins.md`; the plugins
  Screenshots (`found:` in its cache; its old `cacheDir`s are no longer
  removed, as the cache file now lives in that folder and they were gone
  on this computer already), Series (`edits` in its state), Info (`shown`),
  Send (`devices`), each `api: 3`.
- New: `app/src/pluginstore.{h,cpp}`.
- Stay settings: Screenshots' `hide`, Series' and Sheet's `recipe` (the
  user's choice of look), Send's `compatible`, On this day's, Frame's and
  Theme's.
- Tests: the store (at once, written after the turn, one write for many
  changes, none for the same value, read by the next start, a failed write
  said); the move (every key where it belongs, `plugins.json` exactly the
  switches and settings, a second start writing nothing, a tour seen not
  touching `plugins.json`); a move that cannot be written; Screenshots
  with its cache; `apiUses`. Then a window on a copy of Jacek's live
  `plugins.json` with his library: the file afterwards, and a second
  launch leaving its mtime as it was.

### B2: hooks in the viewer and on actions

- Changed: `plugins.{h,cpp}` (fetch `options`, `item.effect`,
  `field("effect")`), `PinholePlugin.qml` (`viewerLayer`),
  `PluginAction.qml` (`order`, `hint`, `complain`), `PluginGroup.qml`,
  `MoreGroup.qml`, `MenuGroup.qml` (the complaint chip, moved from
  `PhotoViewer.qml`), `PhotoViewer.qml` (the layers' `Repeater`; plugin
  groups sorted by `order`), `Main.qml` (`pluginActions` sorted by
  `order`; select mode's bar with plugin actions below 0 before Copy),
  `docs/plugins.md`, `app/tests/pinhole_tests.cpp`.
- Tests:
  - a test plugin with a layer that counts the times it is made and sees
    `shown` go false on Esc and on →, and that zooms with the photo (the
    layer's mapped rect equals `frame`'s);
  - a fetch of a video with `{stream: true}` gives an `http://` URL before
    the download ends;
  - `complain` shows and fades;
  - `order` places a test action before Copy in select mode;
  - `PINHOLE_SELFTEST=keys` and `actionbar:` pass as before.
- As built: the layer, `complain` and `order` need the whole window, so
  they are a self-test, `PINHOLE_SELFTEST=layers:<dir>`
  (`selftestlayers.cpp`), run offscreen with the test plugin
  `app/tests/layers` among the plugins: the layer made once per photo,
  on the photo opened, zoomed and panned, `shown` false at once on ← and
  on Esc and gone once closed, none for three photos passed quickly; the
  complaint under the viewer's button, gone after four seconds; an order
  -5 action before Copy, its complaint over the bar. The fetch options,
  `effect` and `apiUses` are unit tests. `keys` gives the same commands
  and results as main's binary, and `actionbar:`'s frames are the same
  pixels.

### B3: the Live Photos plugin

- New `plugins/live/`:
  - `manifest.json`: `pinhole.live`, "Live Photos", api 3.
  - `Main.qml`: the mark (`field: "live"`, shape live, yellow, order 30,
    `defaultShown: false`); a `viewer` group, `order: -10`, made of the
    play action (text: the effect's name; key L; `busy` while the motion
    comes; `active` while it plays) and the four effects (`hint:
    "iPhone"` on the one saved; `active` on the one chosen); a map of the
    effects chosen in this run, by id; `viewerLayer: LiveLayer {}`.
  - `LiveLayer.qml`: a `MediaPlayer` and `VideoOutput` that loop for loop
    and bounce. An `Image` of the long exposure. An `Image` of `original`
    over Pinhole's `full` when a photo saved as a long exposure is shown
    as anything else. It fetches with `{stream: true}` for live, loop and
    bounce. A loop or bounce plays as the photo shows, and L plays again.
    It stops when `shown` turns false.
  - `i18n/en.json` and `i18n/pl.json`: the strings, moved from Pinhole's
    ("Live", "Loop", "Bounce", "Long exposure", "Live effects", "Play the
    Live Photo", "This Live Photo has no motion", "Could not play the
    motion", "Could not show this effect").
- Removed from the core: from `viewer.{h,cpp}`, `isLive`, `liveSource`,
  `liveLoading`, `savedEffect`, `effect`, `looping`, `longExposure`,
  `playLive`, `setEffect`, `showEffect`, `motionKind`, `stillKind` (always
  `full` now), `m_chosen` and `liveFailed`; from `PhotoViewer.qml`, the
  Live player, the long exposure, the Live `MenuGroup` and `liveNotice`;
  the core's stand-in `pinhole.live/live` mark (B1); and `LiveEffect` stays
  in `library.h` (data).
- Also: `bin/install-files` (the plugin), README (the plugin list),
  `selftest.cpp` (`live:` and `actionbar:` drive the plugin's button by
  its key and read its layer's player; `viewer` counts the motion through
  the layer), and `app/tests/pinhole_tests.cpp` (the `playLive` test goes;
  a test that loads `plugins/live` against a test library and plays a
  `live` rendition from a stub service).
- Tests: the above. `PINHOLE_SELFTEST=live:<dir>` on the real library: L
  plays, ⇧L opens the four, Bounce plays round and round, Long exposure
  shows. First frame within 5 % of the baseline with 12 plugins.

### B4: the Favourites plugin

- New `plugins/favourites/`: `manifest.json` (`pinhole.favourites`,
  "Favourites"), `Main.qml`, `i18n/`.
  - Mark: field favorite, heart, red, order 10, shown.
  - A `viewer` action: text `""` or `""`, key F, `active` when
    it is a favourite, `order: -20`.
  - A `selection` action: the same, over what is ticked, `order: -20`.
  - Toggling: all favourites become none, else all become favourites.
    Then `pinhole.setFavorite(…)` and `pinhole.did(qsTr("favourite"), …)`.
    A failure calls `complain` with the message.
- Changed: `favourites.{h,cpp}` (`set(QList<int>, bool) -> QJSValue`
  promise via `PluginHost`, no `History`), `plugins.{h,cpp}`
  (`setFavorite`), `Main.qml` (select mode's heart and its width in
  `barMetrics` removed), `PhotoViewer.qml` (the heart removed),
  `selftest.cpp` (`favourite:` presses F through the plugin), and
  `pinhole_tests.cpp` (the `Favourites` tests become `setFavorite` tests:
  set at once, put back on failure, `other_source` after a switch). The
  core's stand-in mark is removed. Also `bin/install-files`, README and
  `docs/plugins.md`.
- Tests: the above. Undo and redo of a favourite. First frame within 5 %.

### B5: one JPEG, used (after A1)

- `app/src/outgoing.{h,cpp}`: `place()` asks for `fetch(id, "jpeg")` in
  place of `magick`. `compatibleName` goes, along with its test.
- `core/src/storage.rs`: `forget_compatible()` removes the whole
  `compatible` folder, as nothing writes it any more.
- `plugins/send/SendPanel.qml`: HEIC as JPEG through `pinhole.fetch(id,
  "jpeg")`.
- `plugins/send/sender/src/main.rs`: `jpeg` and its tidying of `jpeg/` go.
- `selftest.cpp` (`outgoing`), `docs/plugins.md` (`fetch` kinds).
- Tests: `PINHOLE_SELFTEST=outgoing` (a HEIC becomes a JPEG with the date
  kept); `PINHOLE_SELFTEST=send:<dir>`; `cargo test` in the sender.

### B6: sources from plugins (after A2)

This is the largest phase, and it lands whole, because a halfway state
would mean writing throwaway code.

- `app/src/sources.{h,cpp}`:
  - keeps `list`, `current`, `known`, `progress`, `rounds`, `show`,
    `showAt`, `remove`;
  - gains `kinds` (from the event), `discovered`, and `add(kind, entry)`;
  - `signIn`, `submitCode`, `cancelSignIn`, `icloudpdHas`, `addFolder`
    and `chooseFolder` go into `PluginSources` and plugin QML;
  - `Plugins::setEnabled` sends `plugins_changed`.
- `app/src/plugins.{h,cpp}`: `PluginSources` (scoped by the manifest's
  `sources`); `chooseFolder`; manifest `sources` checked;
  `Plugins::at().sources`.
- New: `app/src/PluginSource.qml`. Changed: `PinholePlugin.qml`
  (`sources`), `Main.qml` (`pluginSources`, `placeName`, the trouble
  card, the first-run picker), `SidePanel.qml` (rows), `SourcePicker.qml`
  (offers and ways), `resources.qrc`, `DropTarget.qml` (place).
- Removed: `app/src/SignInPanel.qml`. Its code moves to
  `plugins/icloud/ICloudSignIn.qml`, a name that does not clash with
  anything in Pinhole's translations.
- New `plugins/icloud/`:
  - `manifest.json`: `pinhole.icloud`, "iCloud",
    `sources: [{kind: "icloud"}]`, api 3.
  - `Main.qml`: a `PluginSource` with `place` "iCloud"; `offers` (icloudpd
    signed in here); `ways` ("Sign in to iCloud…"); `note` ("Not signed
    in", when `needs_sign_in`); `actions` (Sign out with its key, or Sign
    in); `signIn` opening `ICloudSignIn` as a cover.
  - `ICloudSignIn.qml` and `i18n/`.
- New `plugins/folder/`:
  - `manifest.json`: `pinhole.folder`, "Folders", `sources: [{kind:
    "folder"}]`.
  - `Main.qml`: a `PluginSource` with `place` the folder's name; `offers`
    (the discovered folders, as `SourcePicker` picks them now: three at
    most, those with 20 or more first); `ways` ("Choose a folder…",
    through `pinhole.chooseFolder`); `note` ("Its disk is not connected",
    when `offline`).
  - `i18n/`.
- Also: `bin/install-files`, README, `docs/plugins.md`,
  `docs/sources.md`, and `app/src/i18n/pl.json` (strings moved out).
- Tests:
  - `pinhole_tests.cpp`: `PluginSources` rejects `not_yours`;
    `Sources` parses `kinds`;
  - with a scratch `HOME` and its own `PINHOLE_SOCKET`, the first run
    with a folder of photos in `Pictures` shows it offered on key 1, which
    adds it;
  - `PINHOLE_SELFTEST=sourcesbar:` and `settings:` images look as before;
  - signing in with icloudpd's session on a scratch state folder, by hand
    once, with Jacek's icloudpd session copied;
  - the real window on the real library: unchanged, the source row as
    before, Sign out present;
  - first frame within 5 % with 14 plugins.

### B7: switched off, and the trust lines (after B6)

- `Main.qml`: the empty states, in the middle of the window, each with
  visible buttons.
  - No source plugin is on: "No source of photos is on", then "Where the
    photos come from is a plugin's: iCloud, Folders." Under that, one
    button for each built-in source plugin that is off, "Turn on iCloud"
    and "Turn on Folders" (keys 1 and 2), which call
    `plugins.setEnabled(id, true)`.
  - Sources added, but their plugins all off: "Your sources' plugins are
    off: %1", with the same buttons.
  - Plugins on, nothing added: the first-run picker, as before.
- `SidePanel.qml`, under the switch of a plugin with `sources`:
  - when it is off: "Its sources and their photos stay on this computer,
    out of sight, until it is on again.";
  - for a personal source plugin: the trust line above;
  - for a personal copy of a built-in source plugin: "Your own copy of
    %1: it sees the password you type."
- `plugins.cpp`: a personal plugin's `provider` kinds checked (A4's
  rules) in `manifestProblem` and in `pinhole plugin check`.
- Tests:
  - with both source plugins off on a copy of the config, the empty state
    shows. "Turn on iCloud" brings back the grid with the same 49,079
    photos and no listing (pinholed's log says no "listing the whole
    library");
  - the window opened with `plugins.json` from the baseline copy leaves it
    byte for byte the same;
  - `pinhole plugin check` on the A4 test source's plugin folder.

### What can run side by side

| A track (cargo build) | B track (qmake build) |
|---|---|
| A1 | B1 |
| A2 | B2 |
| A3 | B3 |
| A4 | B4 |
| | B5 (needs A1) |
| | B6 (needs A2) |
| | B7 (needs B6; its plugin-check part needs A4's rules, written down above, not A4's code) |

The tracks share almost no files. The exceptions:

- `bin/install-files`: A3, B3, B4 and B6 each add or move one block. A
  conflict there is trivial; rebase and keep both.
- `README.md` and `docs/sources.md`: A2, A3 and B6. Edit separate
  sections.
- `plugins/icloud/`: A3 writes `source/` and `auth/`, and B6 writes the
  QML. No file is in both.

B5 touches `plugins/send/sender` (cargo) and the window. It is a B phase
and holds the B track's build.

## Risks, and the answers

1. **The first frame gets slower with four more plugins** (12, then 14).
   *Answer:* keep each new `Main.qml` small. The sign-in panel and the
   Live layer are `Component`s made on use. Marks use `field`, so there is
   no JS over 49k photos. No new plugin has a tour. Measure in every B
   phase against Phase 0. The budget is 5 %. Over it, look at
   `collectPlugins` first (it now also runs `setMarks`).
2. **A per-frame cost in the grid from generic marks.** *Answer:* one more
   `quint32` read per visible cell (`m_markBits`, 400 KB for 100k), and
   masks worked out when the kinds change. The atlas is rebuilt only on a
   kinds or colour change, never on a `photos` change. Checked with
   `--bench` on 100k in B1.
3. **Turning off iCloud looks as if 49,079 photos are gone.** *Answer:*
   nothing is deleted, listed again or written to settings. The switch
   says so, the middle of the window says which plugin to turn on and
   gives the button, and B7's test proves the way back is instant.
4. **The window and pinholed disagree on which plugins are on.**
   *Answer:* plugins.json is the one truth. The window writes it, then
   sends `plugins_changed`. pinholed decides what is listed and shown, and
   the window follows the `kinds` and `sources` events. Each round
   rechecks the file's mtime, for edits by hand and for another window.
5. **A personal copy of `pinhole.icloud` sees the password.** *Answer:*
   that is true of any plugin today, so say so on its row (B7). Signing in
   is scoped by kind. pinholed never logs the params.
6. **Live motion now in plugin QML: timing and zoom.** *Answer:* the layer
   lives inside `frame`, so zoom, pan and the grow from the cell apply as
   they do to the player now. `shown` turns false the moment shrinking
   starts, as `closeViewer()` stops the player now. B2's test checks the
   layer's rect, and B3 runs `live:` and `viewer` on the real library.
7. **The crate split breaks the package build for others.** *Answer:*
   A3's test runs `makepkg -f` as an ordinary user, and `cargo build
   --locked --workspace` from a fresh clone of the branch.
8. **Members outside the workspace root.** *Answer:* Cargo supports this
   with `package.workspace`. If it fights the lock file, put the
   workspace root at the repository's top level with `target-dir =
   "core/target"` in `.cargo/config.toml`, so the paths stay the same.
   *Decided in A3:* it did not fight; the root stays `core/` (see A3).
9. **A provider that hangs or floods.** *Answer:* two minutes of silence
   means it is killed. A `put` line is limited to 4 MB and 1,000 assets.
   Assets are checked before they are written. It restarts at most once
   in 10 s.
10. **Strings move between catalogs and lose their Polish.** *Answer:*
    each B phase moves the `pl.json` entries with the strings, and
    `pinhole plugin check` plus the translation tests in `app/bin/test`
    must pass. A Polish run (`LANG=pl_PL.UTF-8`) of `settings:` and
    `sourcesbar:` is looked at.
11. **`pinhole.viewer` was the whole `Viewer` object, and B3 removes
    properties from it.** *Answer:* only `open`, `index` and `close` are
    documented, and they are the only ones any plugin uses (checked:
    `git grep pinhole.viewer plugins`). The rest was never API.
12. **iCloud's `full` of a long exposure.** *Answer:* the core always
    shows `full`. The Live layer puts `original` over it when another
    effect is chosen, as `stillKind()` did.

## Open questions, decided

- *Should favourites be a plugin?* Yes, as above. The field and
  `setFavorite` stay in the core, because the source keeps them.
- *Should drop-to-add, Export, Copy and dragging out be plugins?* No.
  They are how files go in and out for any source, they are C++
  (portal, file promises), and they have no kind-specific part.
- *Should video playback be a plugin?* No. A photo browser plays videos.
- *Should effects be made by the Live plugin?* No. pinholed makes them, as
  derived files under the limit, shared with Background.
- *Should iCloud move out of process later?* No, for the reasons above.
  The protocol is there for sources of your own.
- *Plugin names and Polish:* "iCloud" (iCloud), "Folders" (Foldery),
  "Live Photos" (Zdjęcia Live), "Favourites" (Ulubione).
- *Do the new built-ins get tours?* No. They are what Pinhole always did.
- *Does `pinhole sign-in` stay in the CLI?* Yes, sending `kind: "icloud"`.
  It is the one place in the core that still says "Apple ID", because a
  terminal has no plugin to ask.
- *Where does this document live?* In `docs/core-and-plugins.md`, kept
  true as each phase lands.
