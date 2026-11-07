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
