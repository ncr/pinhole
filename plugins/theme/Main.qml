import QtQuick
import Pinhole.Plugin
import "Palette.js" as Palette

// Theme: an Omarchy theme made from the photo shown full window, and set,
// so the whole desktop takes the photo's colours; Pinhole follows the
// Omarchy theme, so it does too. The palette is Palette.js's, from the
// photo's colours as magick finds them; the folder and the switch are
// pinhole-theme's. Ctrl Z sets the theme there was before.
PinholePlugin {
    id: plugin

    // Omarchy's here: its theme scripts are on PATH.
    property bool available: false
    // The theme being made: its fetch and programs, to cancel.
    property var making: null
    // The theme each of its own replaced, by name: making one again from
    // the same photo replaces it in place, and its undo still goes back to
    // the one before it.
    property var replaced: ({})

    // The photo's colours kept strong, or muted; the theme dark, light or
    // as the photo is.
    readonly property bool strong: pinhole.settings.strong !== false
    readonly property string mode: pinhole.settings.mode || "auto"

    readonly property bool photoShown: pinhole.viewer.open && pinhole.viewer.index >= 0
                                       && !pinhole.library.item(pinhole.viewer.index).video

    Component.onCompleted: pinhole.run("pinhole-theme", ["available"]).then(() => available = true, () => {})

    // A theme's name: the photo's own, as Omarchy's theme picker shows it
    // ("Pinhole Img 7940 K3f9"), with a short hash of its id, since many
    // photos share a name (IMG_0001 again and again, a phone's screenshots).
    // The same photo has the same theme whatever the settings, so making it
    // again replaces it.
    function nameFor(item) {
        const stem = plain(item.filename || "").replace(/\.[^.]*$/, "").toLowerCase()
                         .replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").substr(0, 20).replace(/-+$/, "")
        return "pinhole-" + (stem || "photo") + "-" + hash(item.id)
    }
    // Letters without their marks, as a name can have them: Łąka is Laka.
    function plain(text) {
        const special = { "ł": "l", "Ł": "L", "ß": "ss", "æ": "ae", "Æ": "Ae", "ø": "o", "Ø": "O", "đ": "d", "Đ": "D", "œ": "oe", "Œ": "Oe" }
        return text.normalize("NFD").replace(/[\u0300-\u036f]/g, "").replace(/[łŁßæÆøØđĐœŒ]/g, c => special[c])
    }
    // FNV-1a, four letters and digits of it.
    function hash(text) {
        let h = 0x811c9dc5
        for (let i = 0; i < text.length; ++i) {
            h ^= text.charCodeAt(i)
            h = Math.imul(h, 0x01000193) >>> 0
        }
        return (h % 1679616).toString(36).padStart(4, "0")
    }
    function titleOf(name) {
        return name.split("-").map(word => word.charAt(0).toUpperCase() + word.substr(1)).join(" ")
    }
    // What a program or the service said went wrong, its last line.
    function why(error) {
        const text = String(error.stderr || error.stdout || error.message || error.code || error).trim()
        return text.split("\n").pop()
    }

    function make(index) {
        if (making)
            return
        const item = pinhole.library.item(index)
        const name = nameFor(item)
        let path = "", palette = null, before = ""
        const step = promise => making = promise
        step(pinhole.fetch(index, "full"))
            .then(fetched => {
                path = fetched
                return step(pinhole.run("magick", Palette.magickArgs(path)))
            })
            .then(result => {
                palette = Palette.make(Palette.parseHistogram(result.stdout), strong, mode)
                return step(pinhole.run("pinhole-theme", ["current"]))
            })
            .then(result => {
                const current = result.stdout.trim()
                before = current === name && replaced[name] ? replaced[name] : current
                const args = ["make", name, path, Palette.icons(palette), before].concat(Palette.swatches(palette))
                return step(pinhole.run("pinhole-theme", args, { input: Palette.toml(palette, titleOf(name)) }))
            })
            .then(() => {
                replaced[name] = before
                const what = qsTr("theme from %1").arg(item.filename)
                // Pinhole's notice says it, with its undo.
                if (before === "" || before === name)
                    pinhole.notify(qsTr("Your desktop's theme now: %1").arg(titleOf(name)))
                else
                    pinhole.did(what, () => set(before, qsTr("Could not go back to %1: %2")),
                                      () => set(name, qsTr("Could not set %1 again: %2")))
            })
            .catch(error => failed(error, qsTr("Could not make a theme: %1").arg(why(error))))
            .then(() => making = null)
    }
    // Undone or done again: that theme set, if it is still there; one of
    // Pinhole's own gone (exit 3) was pruned, as only the last 12 are kept.
    function set(name, saying) {
        pinhole.run("pinhole-theme", ["set", name]).catch(error => failed(error, error.exitCode === 3
            ? qsTr("%1 is gone: only the last 12 themes made from photos are kept").arg(titleOf(name))
            : saying.arg(titleOf(name)).arg(why(error))))
    }
    function failed(error, text) {
        if (error.code === "cancelled")
            return
        pinhole.warn(text)
        pinhole.notify(text)
    }
    actions: [
        PluginAction {
            id: themeAction
            place: "viewer"
            text: qsTr("Theme")
            key: "T"
            shown: plugin.available && plugin.photoShown
            busy: plugin.making !== null
            onTriggered: plugin.make(pinhole.viewer.index)
        }
    ]
    help: [["T", qsTr("A desktop theme from the photo")]]

    tourReady: available && pinhole.library.count > 0
    // The photo nearest `index` that is not a video: the Theme button
    // shows on one.
    function nearestPhoto(index) {
        const count = pinhole.library.count
        for (let d = 0; d < count; ++d)
            for (const i of [index - d, index + d])
                if (i >= 0 && i < count && !pinhole.library.item(i).video)
                    return i
        return -1
    }
    tour: [
        {
            title: qsTr("Your photo, your desktop"),
            text: qsTr("On a photo shown full window, T makes an Omarchy theme from it: the photo as the background, its colours everywhere, the text kept readable."),
            open: () => plugin.nearestPhoto(pinhole.currentIndex()),
            action: themeAction
        },
        {
            title: qsTr("Back again"),
            text: qsTr("Ctrl\u00a0Z sets the theme there was before. The last 12 themes made stay in Omarchy's theme picker, each named Pinhole and the photo's name."),
            action: themeAction
        },
        {
            title: qsTr("Strong or muted"),
            text: qsTr("Here, in the side panel (Tab): the photo's colours strong or muted, and the theme dark, light or as the photo is."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: Math.round(theme.fontSize * 0.5)
            Item {
                width: parent.width
                height: strongSwitch.height
                UiText {
                    anchors.left: parent.left
                    anchors.right: strongSwitch.left
                    anchors.rightMargin: theme.fontSize
                    y: Math.round((parent.height - height) / 2)
                    elide: Text.ElideRight
                    color: theme.foreground
                    text: qsTr("The photo's colours strong")
                }
                Toggle {
                    id: strongSwitch
                    anchors.right: parent.right
                    checked: plugin.strong
                    onToggled: pinhole.settings.strong = !plugin.strong
                }
            }
            Row {
                spacing: Math.round(theme.fontSize * 0.5)
                Repeater {
                    model: [["auto", qsTr("As the photo")], ["dark", qsTr("Dark")], ["light", qsTr("Light")]]
                    ShellButton {
                        required property var modelData
                        text: modelData[1]
                        active: plugin.mode === modelData[0]
                        onTapped: pinhole.settings.mode = modelData[0]
                    }
                }
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: qsTr("The last 12 themes made or used are kept in Omarchy's theme picker; older ones are removed, never the one set.")
            }
        }
    }
}
