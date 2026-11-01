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
every built-in is on. `Main.qml` loads each plugin that is on in a `Loader`
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

- `field` and `photos` are alternatives. A mark with `field` costs nothing
  when the library changes. With `photos`, `Plugins::marksOf` follows the
  positions through `libraryReloaded` until the plugin gives them again, as
  `hiddenOf` does.
- At most 32 marks in all (`quint32` per photo), and at most 4 per plugin.
  Beyond that, `pinhole.warn` says so and the extra marks are left out.
- The core's own: `pinhole/video` (shape video, cyan, order 20, field video)
  and `pinhole/stack` (shape stack, magenta, order 90; set from `m_stackOf`
  as today).

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
- `marks.h`: `enum Mark` gives way to `struct MarkShape { QString builtin;
  QString glyph; }` and `MarkShape markShape(const QString &icon)`.
  `shape()` keeps the four drawn shapes. It draws a glyph with the theme's
  font as a `QPainterPath::addText`, filled for plain and stroked as the
  tube. `plainMark`, `neonMark` and `markImage` take a `MarkShape`.
  `MarkProvider` serves `image://mark/<icon>`.
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

`pinhole.fetch(photo, kind, options)`: a third argument that is an object,
not a function, is `options`. `{ stream: true }` gives a URL for a
`MediaPlayer`: `http://127.0.0.1…` while it downloads (the service's
`stream`), or `file://…` once cached. `kind` may now be `jpeg` (above) and
the effects (`loop`, `bounce`, `long_exposure`).

`library.item()` gains `effect` (`"loop"`, `"bounce"`, `"long_exposure"`,
`""`), and `library.field("effect")` works.

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
    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>>;
    /// The `sources` event's own fields for it: name, and account or path.
    fn describe(&self, entry: &Value) -> Value;
    /// `set_source` with this kind: the entry for these params, checked.
    fn entry_for(&self, params: &Value) -> Result<Value, Failure>;
    /// What `discover` found that could be added.
    fn discover(&self, added: &[Value]) -> Value { Value::Null }
    fn needs_sign_in(&self) -> bool { false }
    fn sign_in(&self, params: &Value) -> Result<SignedIn, Failure> { Err(Failure::unsupported()) }
    fn submit_code(&self, params: &Value) -> Result<SignedIn, Failure> { Err(Failure::unsupported()) }
    fn sign_out(&self, entry: &Value) -> Result<(), Failure> { Err(Failure::unsupported()) }
    /// Taken out of the sources: what it kept apart goes (an Apple ID's session).
    fn removed(&self, entry: &Value) {}
}
pub enum SignedIn { Entry(Value), NeedsCode }
pub struct Failure { pub code: String, pub message: String }

pub struct Kinds { all: Vec<Arc<dyn Kind>>, on: RwLock<HashSet<String>> }
impl Kinds {
    pub fn builtin(auth: PathBuf) -> Kinds;            // IcloudKind, FolderKind
    pub fn refresh(&self, switches: &HashMap<String, bool>) -> bool; // changed?
    pub fn of(&self, kind: &str) -> Option<Arc<dyn Kind>>; // on or not
    pub fn on(&self, kind: &str) -> bool;
    pub fn report(&self) -> Value;                     // the `kinds` event
}
```

- `IcloudKind` (`icloud/kind.rs`) holds what is now in `service.rs`: the
  `signing_in` map of `ProcessProvider`s, sign-in through `import_icloudpd`
  or a password, `submit_code`, `sign_out` (cookies removed), `removed`
  (`state_dir` removed), `implicit` (`auth::accounts()`), the
  `needs_sign_in` check in `entry_for`, `describe` (with `account_name`),
  and `discover` (icloudpd's stems).
- `FolderKind` (`folder.rs`) has `entry_for` (absolute, a folder,
  trailing `/` trimmed), `describe` (`display_name`), and `discover`
  (`discover::candidates`).
- `Source` gains `fn details(&self) -> Value` (`{"account": …}` or
  `{"path": …}`). `source_info` uses it in place of the
  `strip_prefix("icloud:")` and `strip_prefix("folder:")` tests.
- `storage::read_settings(path, &Kinds)`. `resolve` normalises entries
  through `Kinds::of(kind).normal()`, and fills a missing `sources` from
  every kind's `implicit()`, in registry order (iCloud first, as now),
  then the folder shown. It keeps entries of kinds that are off.
- **Which source is shown**: `Kinds::shown(&Settings) -> Option<Value>`.
  That is `settings.source` if its kind is on, else the first source whose
  kind is on, else none. settings.json is never rewritten for this, so
  turning a plugin back on shows what was shown before.
- `core/src/plugins.rs`: `plugins_path()` (`$XDG_CONFIG_HOME/pinhole/plugins.json`)
  and `switches() -> HashMap<String, bool>` read `enabled`. A built-in
  kind is on unless its plugin's switch is `false`. A provider kind is on
  only when its switch is `true` (phase A4).
- `sync::source_for` becomes `Kinds::source_for(entry)`. `service.rs`
  keeps no `icloud` or `folder` names.
- `sync_round` runs under `std::panic::catch_unwind`. A panic is logged,
  the health becomes `failing`, and the loop goes on.

### Protocol (still `protocol: 1`; additive)

| Change | |
|---|---|
| event `kinds` | `{"event": "kinds", "kinds": [{"kind": "icloud", "plugin": "pinhole.icloud", "on": true, "needs_sign_in": true, "accepts": […]}, …]}`, sent to a window as it connects (after `sources`) and when a switch changes |
| method `kinds` | the same, as a result |
| method `plugins_changed` | `{}`. Reads plugins.json again; if a kind went off, shows `Kinds::shown`; broadcasts `kinds`, then `source`, `library_moved`, `sources` if they changed. The window sends it after `Plugins::setEnabled`. Each sync round also checks the file's mtime, for edits by hand |
| `sources` entries | add `plugin`. Only sources of kinds that are on are listed. `current` is `Kinds::shown` |
| `source` event | adds `plugin`. The rest comes from `details()` |
| `set_source` | `kind` may be any kind that is on; else `turned_off` |
| `sign_in`, `submit_code` | take `kind`; none means `icloud`, so the CLI and older windows keep working. A kind that is off: `turned_off` |
| `discover` | adds `kinds: {"icloud": {"icloudpd": […]}, "folder": {"folders": […]}}` for kinds that are on; keeps `icloudpd` and `folders` at the top |
| `fetch` `kind: "jpeg"` | see Conversion |
| error `turned_off` | a source kind whose plugin is off |

## The source protocol (phase A4, `docs/source-providers.md`)

A provider is a program in a plugin's folder. It talks JSON lines over
stdio, as `auth/PROTOCOL.md` does. It is started as `<provider>
--state-dir <dir>`, one process per source added (and one more for
`discover` and signing in). `ProcessKind` and `ProcessSource`
(`core/src/process.rs`) implement `Kind` and `Source` with it.

- Requests carry `id`. Lines with that `id` and `note` come before the
  answer. Lines with `note` and no `id` are the provider's own news.
- A settings entry of such a kind is `{"kind": "<kind>", "key": "<unique
  in the kind>", …its own fields}`. The source id is `<kind>:<key>`. The
  library is `library/<kind>/<key, made safe>`.

| Method | Params | Result, and notes before it |
|---|---|---|
| `describe` | `entry` | `{name, needs_sign_in, accepts, rounds_every_secs, push}` |
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
| B1 marks | `~/.config/pinhole/window.conf` `[grid]` | `MarkChoices` reads `marks` (int, 13) once, when `markChoices` (JSON) is missing, and writes `{"pinhole/video": bit 1, "pinhole.live/live": bit 2, "pinhole.favourites/favorite": bit 4, "pinhole/stack": bit 8}`. `marks` is left as it is. Ids not in `markChoices` take `defaultShown` | unit test with a temporary `window.conf` holding `marks=13`, `marks=15`, `marks=0` and no `marks`; then the app run with `XDG_CONFIG_HOME` on a copy of `~/.config` |
| A1 / B5 jpeg | `~/.cache/pinhole/compatible/` | made again on demand, so pinholed removes the folder once, as it starts, and logs it. New JPEGs go to `cache/full/…/<key>-jpeg.jpg` (`cached_path(Want::Jpeg, full_key, "jpg", true)`). They count as `full` in the storage panel, with no new `KINDS` entry | pinholed run with `XDG_CACHE_HOME` and `XDG_DATA_HOME` on reflink copies |
| A2 kinds | `settings.json` (absent for Jacek), `plugins.json` (read only by pinholed) | none. `resolve` must give the same `Settings` as now for every input. That is a table test over today's cases, plus "no file at all, one Apple ID signed in". Nothing writes settings.json because a plugin was turned off | pinholed with `XDG_DATA_HOME`, `XDG_CONFIG_HOME` and `XDG_STATE_HOME` on reflink copies and `PINHOLE_SOCKET` of its own. `sources` lists his Apple ID with `count: 49079`. After a round, `meta.listed_at` and `change_token` are unchanged (no new listing). `enabled["pinhole.icloud"] = false` in the copy's plugins.json lists nothing, leaves the library's mtime as it was, and turning it back on shows it again with no listing |
| A3 crates | none | `pinhole-auth` keeps its state folder `~/.local/state/pinhole/auth/<apple id>/`. `auth::provider_path` adds the checkout path `plugins/icloud/auth/pinhole-auth` | as A2, and `pinhole sign-in` on a scratch state folder |
| A4 providers | new `~/.local/state/pinhole/sources/` | new, nothing to migrate | the test provider |
| B3, B4, B6, B7 new plugins | `plugins.json` | **none written.** The new built-ins have no `enabled` entry and so are on. They have no `tour`, so no new-plugins card shows. A test loads a copy of Jacek's plugins.json, runs the window, and checks that the file is byte for byte the same afterwards | `XDG_CONFIG_HOME` on a copy |
| B3 Live effect chosen | memory only (`m_chosen`) | none | |

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
- `core/src/jpeg.rs`: `magick -[0] -background white -alpha remove -alpha
  off -quality 92 jpg:<part>`, the `full` on stdin. Stdin, because a
  folder's file name can hold what ImageMagick reads as its own syntax
  (`[1]`, `*`, `heic:`); `[0]` so an animated GIF makes one file, not
  `part-0`, `part-1`…; white so a see-through PNG is not black. Two at
  most at once: one ImageMagick of a 24-megapixel HEIC takes every core
  for 0.6–0.9 s, and a window may ask for fifty.
- `core/src/storage.rs`: `forget_compatible()` removes
  `old_cache()/compatible` as the shared service starts. Until B5 the
  window still makes its JPEGs there, so it may come back and go again at
  the next start; after B5 nothing writes it.
- `core/tests/service.rs`: a folder with a HEIC (made with `magick` in the
  test, skipped without it) and a JPEG. `jpeg` of the HEIC is a JPEG with
  the EXIF date kept (`magick identify -format %[EXIF:DateTimeOriginal]`).
  `jpeg` of the JPEG is the file itself. Three asks at once run one
  `magick` (a logging `magick` first on `PATH`) and all say `cached:
  false`; a fourth is `cached: true`; `storage` counts it in `full`; the
  old `compatible` folder is gone. `jpeg.rs` has a unit test: a name with
  `[1]*`, an animated GIF, a see-through PNG, a file that is no photo.
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
  - in `core/tests/service.rs`, a folder source with
    `enabled["pinhole.folder"] = false` written to the test's
    `XDG_CONFIG_HOME`. After `plugins_changed`: `sources` is empty, a
    `kinds` event has `on: false`, `current` is null, and the library
    folder and its `change_token` are untouched. Turned on again: shown,
    with no listing (`listed_at` unchanged);
  - the real-library check on a copy (Migration, A2).
- Works alone: the window ignores the new fields, and `sign_in` without
  `kind` is iCloud.

### A3: the built-in sources' Rust moves into their plugins

- `core/Cargo.toml` becomes a workspace root with
  `members = ["daemon", "../plugins/icloud/source", "../plugins/folder/source"]`
  and the same `default-members`. Each member outside `core/` sets
  `package.workspace = "../../../core"`. There is one `core/Cargo.lock`,
  regenerated once offline and committed. The target folder stays
  `core/target`, so `core/target/release/pinholed` stays where
  `serviceclient.cpp` and `bin/install-files` look for it.
- `git mv core/src/icloud plugins/icloud/source/src`. This makes crate
  `pinhole-icloud`, a lib, with `pinhole-probe` as its bin. `git mv
  core/src/folder.rs plugins/folder/source/src/lib.rs` makes crate
  `pinhole-folder`, and `add::copy_into` moves into it.
- `git mv auth plugins/icloud/auth`. `auth::provider_path` looks in the
  checkout at `plugins/icloud/auth/pyicloud/pinhole-auth`.
  `bin/install-files` still installs it as `$PREFIX/bin/pinhole-auth`.
- `pinholed` moves to package `core/daemon` (bin `pinholed`), which
  depends on `pinhole-core`, `pinhole-icloud` and `pinhole-folder`.
  `daemon/src/main.rs` builds `Kinds::new(vec![IcloudKind, FolderKind])`.
  `pinhole-core` no longer depends on `p256`, `tungstenite`, `base64`,
  `url` or `kamadak-exif`. It keeps `reqwest`, which A4's URL tickets use.
- Tests that use `FolderSource` (`sync.rs`, `core/tests/service.rs`) move
  to `plugins/folder/source/tests/` or `core/daemon/tests/`.
  `core/tests/given.rs` follows `pinholed`.
- `bin/install-local`: `cargo build --release --locked --workspace
  --manifest-path core/Cargo.toml`. `pkgbuild/PKGBUILD`: the same. README:
  the layout.
- Tests: `cargo test --release --workspace`. `makepkg -f` in a scratch
  copy of `pkgbuild/` builds, as an ordinary user, with no install.
  `bin/install-local`, then the real window opens on the real library.
  `grep -rn "icloud\|Apple" core/src` finds nothing outside comments that
  name the plugins.

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

### B1: marks from plugins (API 3 begins)

- Changed: `app/src/marks.{h,cpp}`, `photogrid.{h,cpp}`, `plugins.{h,cpp}`
  (`PluginMark`, `setMarks`, `markKinds`, `marksOf` following through
  `libraryReloaded`, `api = 3`, `apiUses`), `PinholePlugin.qml` (`marks`),
  `Main.qml`, `SidePanel.qml` (the "In the grid" switches from
  `plugins.markKinds`, with `markChoices`), `main.cpp` (`markChoices`),
  `sources.pri`, `resources.qrc`, `app/tests/pinhole_tests.cpp`,
  `selftestrun.cpp` (`neon:`), `docs/plugins.md`, `app/src/i18n/pl.json`.
- New: `app/src/markchoices.{h,cpp}`.
- Until B3 and B4, the core itself provides `pinhole.live/live` and
  `pinhole.favourites/favorite` (field marks, as today). That way the
  migration happens once, with the final ids.
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
   Decide that within A3. Do not stop.
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
