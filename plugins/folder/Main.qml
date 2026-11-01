import QtQuick
import Pinhole.Plugin

// Folders: a folder of photos on this computer or a disk as a source.
// pinholed reads it, pairs a Live Photo's two files, follows it as it
// changes and copies in what is dropped on the window (its Rust is this
// plugin's, in source/, linked into pinholed). This is what the window
// shows of it: its rows in the side panel ("Not there" while its disk is
// away), the folders with photos the first run and Add a source offer, and
// Choose a folder… in the desktop's own picker.
PinholePlugin {
    id: plugin

    // Shown at once, as asked: the picker it came from closes as it shows.
    // What went wrong the picker says (Sources' error).
    function add(path) {
        pinhole.sources.add("folder", { path: path }).catch(() => {})
    }
    // As the storage panel says a size: a decimal from 1 GB to 10 GB unless
    // it is whole, none in MB.
    function size(bytes) {
        const gb = bytes / (1 << 30)
        return pinhole.sizeText(bytes, gb >= 1 && gb < 10 && gb !== Math.floor(gb) ? 1 : 0)
    }

    sources: [
        PluginSource {
            kind: "folder"
            icon: ""
            noun: qsTr("Folder")
            place: source => source.name
            // The biggest folders found, past a few stray pictures when
            // there is more: three at most, a parent and its own
            // subfolders alike.
            offers: {
                let folders = (pinhole.sources.discovered.folder?.folders ?? []).slice().sort((a, b) => b.assets - a.assets)
                if (folders.some(f => f.assets >= 20))
                    folders = folders.filter(f => f.assets >= 20)
                return folders.slice(0, 3).map(folder => {
                    const parts = [qsTr("%Ln photo(s) and video(s)", "", folder.assets)]
                    if (folder.live > 0)
                        parts.push(qsTr("%Ln Live Photo(s)", "", folder.live))
                    parts.push(plugin.size(folder.bytes))
                    return { title: folder.name, detail: parts.join(" · "), triggered: () => plugin.add(folder.path) }
                })
            }
            ways: [{
                title: qsTr("Choose a folder…"),
                key: "F",
                triggered: () => pinhole.chooseFolder(qsTr("Show photos from")).then(path => plugin.add(path), () => {})
            }]
            note: source => source.shown === true && source.health === "offline" ? qsTr("Not there: is its disk connected?") : ""
            forgets: source => qsTr("Forget its index? The photos stay in the folder.")
            adds: source => qsTr("Copied into the folder. The files here stay as they are.")
        }
    ]
}
