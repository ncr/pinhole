# Plugin examples

The photo-content search plugin (`examples/semantic-search` in Pinhole's
repository) shows how a covering results panel calls
`pinhole.openPhotos(results, id)`, so ← → go through the results,
and comes back on the photo last looked at when the viewer closes.

Choose the pattern closest to your idea. These are common uses of the same
`PinholePlugin` API, not different manifest types; a plugin can combine them.
Each example below includes every file needed, its dependencies and how to
try it. They work with the installed app, without cloning or rebuilding Pinhole.

| Pattern | Complete example | What you build |
|---|---|---|
| Custom view | [Years](#view-years) | A screen listing photo counts by year |
| Photo tool | [Swatch](#tool-swatch) | A small dialog that makes and copies a PNG |
| Photo source | [JPEG folder](#source-jpeg-folder) | A provider that adds JPEGs to the main grid |

Use separate folders under `~/.config/pinhole/plugins/` as shown below.
Save the files, run the check command, then enable the plugin in
**Settings → Plugins**. Personal plugins start disabled. Keep the manifest
id and source kind unchanged while trying these copies. If you rename a
plugin, update its folder/id and any namespaced source kind together.
A plugin runs with your user account's permissions; enable code you trust.

## View: Years

A custom view presents your existing library differently. This example reads
the date column only when you press **F5**, then opens a themed, scrollable
list. **Escape** returns to the grid. It creates no files and changes no photos.

**Dependencies:** Pinhole of plugin API 15; no external programs.
**Folder:** `~/.config/pinhole/plugins/example.years/`.

```sh
mkdir -p ~/.config/pinhole/plugins/example.years/i18n
# Save the three files below into that folder.
pinhole plugin check example.years
```

Enable **Example years**, press **F5**, and compare the counts with your
library. The [kit reference](plugins.md#the-kit) explains the shared controls.

**manifest.json**

```json
{
  "api": 15,
  "id": "example.years",
  "name": "Example years",
  "version": "1.0.0",
  "description": "A complete example from the plugin workshop."
}
```

**Main.qml**

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
    Command { name: "years.close"; keys: ["Escape"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: plugin.open = false }

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

**i18n/pl.json**

```json
{
  "Example years": "Przykład: lata",
  "Years": "Lata",
  "%n photo(s)": {
    "one": "%n zdjęcie",
    "few": "%n zdjęcia",
    "many": "%n zdjęć",
    "other": "%n zdjęcia"
  },
  "A complete example from the plugin workshop.": "Kompletny przykład z pracowni wtyczek."
}
```

## Tool: Swatch

A tool acts on a photo and produces a result. Point at a photo in the grid,
press **F7**, and choose a size. This example creates a PNG filled with that
photo's average colour, keeps it in Pictures/Pinhole and copies it. It
confirms success only after the clipboard is ready. **Escape** cancels the
dialog. This demonstrates the workflow; it does not export the photo itself.

**Dependencies:** Pinhole of plugin API 15 and ImageMagick's `magick` executable
(on Arch/Omarchy, package `imagemagick`). A missing executable is reported
beside the action. Originals remain unchanged.
**Folder:** `~/.config/pinhole/plugins/example.ask/`.

```sh
mkdir -p ~/.config/pinhole/plugins/example.ask/i18n
# Save the three files below into that folder.
pinhole plugin check example.ask
```

Enable **Example ask**, point at a photo and press **F7**. Check the resulting
PNG and paste it into another app. The [promise contract](plugins.md#promises)
explains cancellation and error handling.

**manifest.json**

```json
{
  "api": 15,
  "id": "example.ask",
  "name": "Example ask",
  "version": "1.0.0",
  "description": "A complete example from the plugin workshop."
}
```

**Main.qml**

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
    property var photo: ({})
    actions: [PluginAction { id: action; place: "grid"; text: qsTr("Swatch"); key: "F7"; usable: pinhole.library.count > 0; onTriggered: plugin.begin() }]
    Command { name: "ask.close"; keys: ["Escape"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: plugin.open = false }

    function begin() {
        const picked = pinhole.library.item(pinhole.currentIndex())
        if (picked.index === undefined || !picked.avgColor) {
            action.complain(qsTr("Could not make it"))
            return
        }
        photo = picked
        open = true
    }

    function make(size) {
        open = false
        const photo = plugin.photo
        if (photo.index === undefined || !photo.avgColor)
            return
        const path = pinhole.picturePath((photo.filename || "Swatch").replace(/\.[^.]*$/, "") + " " + size, "png")
        pinhole.run("magick", ["-size", size + "x" + size, "xc:" + photo.avgColor, path])
            .then(() => pinhole.copyFile(path))
            .then(() => pinhole.notify(qsTr("Copied, and kept in Pictures")), error => {
                action.complain(qsTr("Could not make it"))
                pinhole.warn(error)
            })
    }

    Panel { visible: plugin.open; color: Qt.rgba(0, 0, 0, 0.45); onTriggered: plugin.open = false }
    Card {
        visible: plugin.open
        anchors.centerIn: parent
        width: sizes.width + 48
        height: sizes.height + 48
        Choices {
            id: sizes
            anchors.centerIn: parent
            options: [{ value: 64, text: qsTr("Small") }, { value: 256, text: qsTr("Medium") }, { value: 1024, text: qsTr("Large") }]
            usable: plugin.open
            helpText: qsTr("Size")
            onChosen: size => plugin.make(size)
        }
    }
}
```

**i18n/pl.json**

```json
{
  "Example ask": "Przykład: próbka koloru",
  "Swatch": "Próbka koloru",
  "Could not make it": "Nie udało się utworzyć próbki",
  "Copied, and kept in Pictures": "Skopiowano i zapisano w Pictures",
  "Small": "Mały",
  "Medium": "Średni",
  "Large": "Duży",
  "Size": "Rozmiar",
  "A complete example from the plugin workshop.": "Kompletny przykład z pracowni wtyczek."
}
```

## Source: JPEG folder

A source plugin supplies photos to the existing main grid. Its QML adds a
**JPEG folder…** choice to **Add a source**; its Python provider lists regular
JPEG files and supplies their paths. The service owns the index and generated
thumbnails. This example is read-only: it neither imports nor deletes originals.
It lists only the chosen folder's immediate JPEG files, not subfolders, and
uses modification time as the capture-date fallback.

**Dependencies:** Pinhole of plugin API 15, Python 3, and Pinhole's normal image
decoding dependencies. No Python packages or network account are needed.
**Folder:** `~/.config/pinhole/plugins/me.jpegs/`.

```sh
mkdir -p ~/.config/pinhole/plugins/me.jpegs/i18n
# Save all four files below into that folder.
chmod +x ~/.config/pinhole/plugins/me.jpegs/pinhole-jpegs
pinhole plugin check me.jpegs
```

Enable **JPEG folder**, open **Add a source → JPEG folder…**, and choose a
small folder of JPEGs. Its photos should appear in the main grid. A missing
folder reports offline, rather than publishing an empty successful listing.
The [provider reference](source-providers.md) explains the JSON-lines protocol,
stable ids, rendition keys, batching and errors.

**manifest.json**

```json
{
  "api": 15,
  "id": "me.jpegs",
  "name": "JPEG folder",
  "sources": [
    {
      "kind": "me.jpegs",
      "provider": "pinhole-jpegs"
    }
  ],
  "version": "1.0.0",
  "description": "A read-only JPEG source example from the plugin workshop."
}
```

**Main.qml**

```qml
import QtQuick
import Pinhole.Plugin

// Add a JPEG folder through the source picker; nothing runs during startup.
PinholePlugin {
    id: plugin

    function choose() {
        pinhole.chooseFolder(qsTr("Choose a JPEG folder"))
            .then(path => pinhole.sources.add("me.jpegs", { folder: path }))
            .catch(error => { if (error.code !== "cancelled") pinhole.warn(error) })
    }

    sources: [PluginSource {
        kind: "me.jpegs"
        noun: qsTr("JPEG folder")
        place: source => source.name
        ways: [{ title: qsTr("JPEG folder…"), key: "J", triggered: () => plugin.choose() }]
    }]
}
```

**i18n/pl.json**

```json
{
  "JPEG folder": "Folder JPEG",
  "A read-only JPEG source example from the plugin workshop.": "Przykład źródła JPEG tylko do odczytu z pracowni wtyczek.",
  "Choose a JPEG folder": "Wybierz folder JPEG",
  "JPEG folder…": "Folder JPEG…"
}
```

**pinhole-jpegs**

```python
#!/usr/bin/env -S python3 -I
"""A folder of JPEGs as a Pinhole source; timestamps use file modification time."""
import base64, hashlib, json, os, sys


class ProviderError(Exception):
    pass


def say(line):
    sys.stdout.write(json.dumps(line) + "\n")
    sys.stdout.flush()


def assets(folder):
    with os.scandir(folder) as directory:
        entries = sorted(directory, key=lambda item: os.fsencode(item.name))
    for item in entries:
        if not item.name.lower().endswith((".jpg", ".jpeg")) or not item.is_file(follow_symlinks=False):
            continue
        raw = os.fsencode(item.name)
        name = base64.urlsafe_b64encode(raw).decode("ascii")
        aid = hashlib.sha256(raw).hexdigest()
        st = item.stat(follow_symlinks=False)
        key = hashlib.sha256(raw + f"\0{st.st_size}\0{st.st_mtime_ns}".encode()).hexdigest()
        # Display strings may not contain protocol control characters.
        display = "".join(c if ord(c) >= 32 and not 127 <= ord(c) <= 159 and not 0xD800 <= ord(c) <= 0xDFFF
                          else "\ufffd" for c in item.name)
        yield {"id": aid, "kind": "photo", "filename": display, "taken": st.st_mtime_ns // 1_000_000,
               "renditions": [{"kind": k, "name": name, "key": key, "make": True} for k in ("thumb", "medium")]
               + [{"kind": k, "name": name, "key": key, "file_type": "public.jpeg"} for k in ("full", "original")]}


def ticket(folder, name):
    raw = base64.b64decode(name.encode("ascii"), altchars=b"-_", validate=True)
    filename = os.fsdecode(raw)
    if not filename or "\0" in filename or filename in (".", "..") or os.path.basename(filename) != filename:
        raise ProviderError("bad_request", "The rendition name is not a filename")
    return {"path": os.path.join(folder, filename)}


for line in sys.stdin:
    request = json.loads(line)
    rid, method, params = request["id"], request["method"], request.get("params") or {}
    entry = params.get("entry") or {}
    try:
        if method == "add_entry":
            folder = os.path.abspath(params.get("folder", ""))
            if not os.path.isdir(folder):
                raise ProviderError("offline", f"{folder} is not a folder")
            key = hashlib.sha256(os.fsencode(folder)).hexdigest()
            result = {"entry": {"key": key, "name": os.path.basename(folder), "folder": folder}}
        elif method == "start":
            if not os.path.isdir(entry["folder"]):
                raise ProviderError("offline", f"{entry['folder']} is not there")
            result = {}
        elif method == "list":
            # Finish scanning before sending any put: a failed mount must
            # never become a successful empty listing that removes photos.
            found = list(assets(entry["folder"]))
            for i in range(0, len(found), 500):
                say({"id": rid, "note": "put", "assets": found[i:i + 500]})
            result = {"token": None, "summary": f"{len(found)} photos"}
        elif method == "prepare":
            result = {"tickets": [ticket(entry["folder"], w["name"]) for w in params["wanted"]]}
        else:
            raise ProviderError("unsupported", f"no {method}")
        say({"id": rid, "ok": True, "result": result})
    except ProviderError as error:
        say({"id": rid, "ok": False, "error": {"code": error.args[0], "message": error.args[1]}})
    except OSError as error:
        say({"id": rid, "ok": False, "error": {"code": "offline", "message": str(error)}})
    except (KeyError, ValueError, TypeError) as error:
        say({"id": rid, "ok": False, "error": {"code": "bad_request", "message": str(error)}})
```
