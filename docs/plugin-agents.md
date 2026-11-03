# Pinhole plugin implementation contract

Build the requested workflow as a personal Pinhole plugin. Do not patch the
core app unless the required capability is absent from the public API.

## Discover the installed contract

```sh
pinhole plugin docs --json
pinhole plugin docs plugins
pinhole plugin docs source-providers
```

The JSON includes the installed `api` and complete Markdown documents.
Use that version's contract. In a source checkout the same references are
`docs/plugins.md`, `docs/source-providers.md`, `docs/service.md` and
`docs/sources.md`. The runnable examples are in `app/tests/examples/`.

## Create and validate

```sh
pinhole plugin new me.descriptive-name
pinhole plugin check me.descriptive-name
```

Edit the generated folder under `~/.config/pinhole/plugins/`. Personal
plugins start disabled; the user enables them in Settings (Tab) → Plugins.
Saving reloads an enabled plugin. Use the installed scaffold's manifest API;
API 6 is the current source version. Run `plugin check` after changes.
It checks the manifest, QML and JS, translations and known API footguns;
it is not a substitute for running the interaction in the app.

## UI and data rules

- Import `Pinhole.Plugin`. Use its components and the `theme` properties.
  Use `Panel` or `Card` to contain input, and `ScrollList`, `ScrollGrid`,
  `ScrollArea`, `PinchZoom`, `Choices` and `ShellButton` for app behaviour.
- Use `PluginAction` for discoverable actions. Set `covering` while a view
  covers the photos. Scope its `Command`s to `keyLayer: "plugin"` and its
  open state. Escape closes it. Do not leave global `Shortcut`s active.
- Keep the root cheap: it loads before the first frame. Load large views
  on demand and do expensive work asynchronously. Do not fetch a whole
  library's originals. Use `field()` for columns and `query()` for SQL.
- Persist photo ids. Library positions are capture-date order, independent
  of the main grid's Automatic/Date taken/Date added timeline. Reloads move
  positions. Use `openPhoto` and `reveal` to navigate.
- Originals and the library database are read-only to the UI. Fetch with
  `pinhole.fetch`; query with `pinhole.query`, not an external sqlite3.
  Keep user choices in `settings`, user work in `state`, derived data in
  `cache`/`cacheDir`. Make outputs with `picturePath`.
- Show files through `image://file/` or `image://photo/` as documented, so
  orientation and colours are correct. Video uses `VideoView`. A plugin's
  SDR overlay in the HDR viewer uses `holdSdr` (API 6) when appropriate.
- Wait for promises to finish before confirming success. Use `copyFile`
  for the clipboard, `notify` for quiet success, `complain` and `warn` for
  failures. Handle cancellation and missing/offline files. Record reversible
  edits with `did`, and drop a refused step with `dropStep`.
- Wrap visible words in `qsTr()` and supply `i18n/pl.json`; counted strings
  need plural forms. Give files distinctive names to avoid context clashes.
- A new source uses a namespaced provider in any language speaking the
  documented JSON-lines protocol. The service owns index/cache writes.
  A plugin and its provider have the user's permissions; neither is an OS
  sandbox. Never claim the checker or a provider isolates untrusted code.

## Verify the result

Run the checker, then exercise the real plugin: mouse hover and press,
keyboard navigation, Escape, narrow windows, Polish labels, empty and
offline sources, errors, repeated opening, reload and disabling during a
job. Confirm scrolling, wheel and pinch stay inside its panel. Confirm the
first launch stays quick and the main grid's ordering does not break it.
Use an isolated profile and a virtual display or a hidden workspace for
automated runs. A test must not alter the user's photos or clipboard.

Deliver the plugin files, a short usage note, and what you actually verified.
