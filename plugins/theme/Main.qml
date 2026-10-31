import QtQuick
import Pinhole.Plugin
import "Palette.js" as Palette

// Theme: an Omarchy theme made from the photo shown full window, and set,
// so the whole desktop takes the photo's colours; Pinhole follows the
// Omarchy theme, so it does too. The palette is Palette.js's, from the
// photo's colours as magick finds them; the folder and the switch are
// pinhole-theme's. Ctrl+Z sets the theme there was before.
PinholePlugin {
    id: plugin

    // Omarchy's here: its theme scripts are on PATH.
    property bool available: false
    property bool making: false
    // What went wrong, or the theme made when there is nothing to undo,
    // shown a moment over the photo.
    property string message
    // The theme each of its own replaced, by name: making one again from
    // the same photo replaces it in place, and its undo still goes back to
    // the one before it.
    property var replaced: ({})

    // The photo's colours kept strong, or muted; the theme dark, light or
    // as the photo is.
    property bool strong: pinhole.setting("strong", true)
    property string mode: pinhole.setting("mode", "auto")

    readonly property bool photoShown: pinhole.viewer.open && pinhole.viewer.index >= 0
                                       && !pinhole.library.item(pinhole.viewer.index).video

    Component.onCompleted: pinhole.run("pinhole-theme", ["available"], code => available = code === 0)

    // A name Omarchy's theme picker shows well ("Pinhole Coral Img 7940"),
    // the same for a photo each time, so making it again replaces it.
    function nameFor(palette, filename) {
        const stem = filename.replace(/\.[^.]*$/, "").toLowerCase().replace(/[^a-z0-9]+/g, "-")
                             .replace(/^-+|-+$/g, "").substr(0, 24).replace(/-+$/, "")
        return "pinhole-" + Palette.hueName(palette) + (stem !== "" ? "-" + stem : "")
    }
    function titleOf(name) {
        return name.split("-").map(word => word.charAt(0).toUpperCase() + word.substr(1)).join(" ")
    }

    function make(index) {
        if (making)
            return
        making = true
        const item = pinhole.library.item(index)
        pinhole.fetch(index, "full", (path, error) => {
            if (path === "")
                return fail(error)
            pinhole.run("magick", Palette.magickArgs(path), (code, out, err) => {
                if (code !== 0)
                    return fail(err)
                const palette = Palette.make(Palette.parseHistogram(out), strong, mode)
                const name = nameFor(palette, item.filename)
                pinhole.run("pinhole-theme", ["current"], (code, current) => {
                    current = current.trim()
                    const before = current === name && replaced[name] ? replaced[name] : current
                    const args = ["make", name, path, Palette.toml(palette, titleOf(name)), Palette.icons(palette)]
                    pinhole.run("pinhole-theme", args.concat(Palette.swatches(palette)), (code, out, err) => {
                        making = false
                        if (code !== 0)
                            return fail(err)
                        replaced[name] = before
                        // Pinhole's notice names it, with its undo.
                        if (before === "" || before === name)
                            return show(titleOf(name))
                        pinhole.did(titleOf(name), () => set(before), () => set(name))
                    })
                })
            })
        })
    }
    function set(name) {
        pinhole.run("pinhole-theme", ["set", name], (code, out, err) => {
            if (code !== 0)
                fail(err || out)
        })
    }
    function fail(error) {
        making = false
        console.warn("Theme: " + error)
        show(qsTr("Could not make a theme: %1").arg((error || "").trim().split("\n").pop()))
    }
    function show(text) {
        message = text
        shown.restart()
    }
    function keep(key, value) {
        if (!pinhole.setSetting(key, value))
            console.warn("Theme: the setting could not be kept")
    }

    actions: [
        PluginAction {
            id: themeAction
            place: "viewer"
            text: qsTr("Theme")
            key: "T"
            shown: plugin.available && plugin.photoShown
            busy: plugin.making
            onTriggered: plugin.make(pinhole.viewer.index)
        }
    ]
    help: [["T", qsTr("Make the desktop's theme from the photo")]]

    tourReady: available
    tour: [
        {
            title: qsTr("Your photo, your desktop"),
            text: qsTr("On a photo shown full window, T makes an Omarchy theme from it: the photo as the background, its colours everywhere, the text kept readable."),
            open: () => pinhole.currentIndex(),
            action: themeAction
        },
        {
            title: qsTr("Back again"),
            text: qsTr("Ctrl+Z sets the theme there was before. Each theme made stays in Omarchy's theme picker as Pinhole and the photo's colour."),
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
                    onToggled: {
                        plugin.strong = !plugin.strong
                        plugin.keep("strong", plugin.strong)
                    }
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
                        onTapped: {
                            plugin.mode = modelData[0]
                            plugin.keep("mode", plugin.mode)
                        }
                    }
                }
            }
        }
    }

    Timer { id: shown; interval: 5000 }
    Chip {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: theme.fontSize * 8
        text: plugin.message
        opacity: shown.running && pinhole.viewer.open ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }
}
