# Make Pinhole yours

A plugin can add a photo workflow, a view, a button, a mark on the grid,
or a whole new source of photos. No rebuild of Pinhole is needed: save a
file and the running app reloads it. The handbook in Settings → Plugins →
Build a plugin is bundled with the app and works offline.

## Start in two commands

```sh
pinhole plugin new me.photo-ideas
pinhole plugin check me.photo-ideas
```

Open Settings with Tab. Under Plugins, turn on your new plugin. Press F2:
the generated example already shows recent photos, with scrolling and zoom.
Edit `~/.config/pinhole/plugins/me.photo-ideas/Main.qml` and save it. Your
changes appear immediately; errors appear under its row in Settings.

The generated manifest asks for the installed API. Pinhole currently has
**plugin API 6**; a manifest's `api` is the minimum version it requires.
The plugin runs as your user and can start programs. Enable code you trust.

## A view or a workflow

Try a birthday wall, a print preparation tool, a client selection board, or
a view that finds photos for your next project. Start from the generated
plugin; use `PinholePlugin`, `PluginAction`, `Panel`, `ScrollGrid` and
`PinchZoom` so input, theme and navigation match the app.

The [kit and working examples](plugins.md#the-kit) include a year list, a
zoomable colour wall, a colour swatch maker and an asynchronous SQL query.
Examples live in `app/tests/examples/` in the source checkout.

## A tool that makes something

Fetch only the selected photos. Make a contact sheet, a GIF, a label layout,
or a file for another program. Use `pinhole.fetch`, `pinhole.run` and
`pinhole.picturePath`; wait for `pinhole.copyFile` before saying “Copied”.
Use `pinhole.notify` for a quiet confirmation and `PluginAction.complain`
for an error beside the action. Respect the user's originals: make a new
file rather than editing one in place.

The [API reference](plugins.md#pinhole) describes promises, cancellation,
undo, settings, state and cache. Contact sheet (`plugins/sheet`) and Send
to phone (`plugins/send`) are complete examples in the checkout.

## A new source of photos

Connect a NAS, Immich, another cloud, or a local collection. The QML plugin
provides the source's UI; a provider executable speaks JSON lines to
`pinholed`. Any language works. The service owns the index, downloads and
cache; a provider returns assets and files through the protocol.

See [source providers](source-providers.md): manifests, lifecycle, examples,
limits and error handling. Sources must be namespaced by the plugin id.
An out-of-process provider is a protocol boundary, not an OS sandbox.

## Build with an agent

Use **Copy instructions for an agent** in the handbook. It copies a short
implementation contract, commands and where to find the full reference.
Describe your idea after it. The same instructions are available without a
window:

```sh
pinhole plugin docs plugin-agents
pinhole plugin docs plugins
pinhole plugin docs --json
```

`--json` returns the installed API version and all bundled documents with
their Markdown. It is suitable for tooling and works with packaged builds.
The CLI and the app read the same embedded files, so documentation follows
the version of Pinhole you are using.

## Keep it quick

A plugin's root loads **before the first frame**. Keep it small. Create large
views through a `Loader` on first use. Defer indexing and external commands
until needed; if the first grid must reflect your grouping, restore a small
cache first and refresh it asynchronously. `Qt.callLater` alone does not
guarantee work happens after the first frame.

Use `pinhole.library.field(name)` for a whole column, or `pinhole.query` for
asynchronous read-only queries. Avoid a JavaScript `item()` call per photo
at startup. Keep photo ids, not positions: positions change on reload.
The plugin library is in capture-date order; the main grid can use a
different timeline order. Test both and do not infer a grid position from a
library index.

See [the complete plugin contract](plugins.md) before sharing your plugin.
