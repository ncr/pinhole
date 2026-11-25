import QtQuick
import Pinhole.Plugin

// The photos of screens together, the newest first, over the window. A
// photo opens full window, ← → through these alone, and closing it comes
// back here. X says the one the keys are on is not of a screen: it goes
// from here, and the worker learns from it.
Item {
    id: gallery
    readonly property var plugin: view.plugin
    // Newest first.
    readonly property var photos: plugin.ids.slice().reverse()

    function start() { tiles.forceActiveFocus() }

    Connections {
        target: pinhole
        function onReturned(photo) {
            tiles.moveTo(photo)
            tiles.forceActiveFocus()
        }
    }

    Panel {}

    PanelHeader {
        id: header
        title: qsTr("Photos of screens")
        detail: gallery.plugin.looking && gallery.plugin.status.phase === "scanning"
                ? qsTr("%n so far, still looking", "", gallery.photos.length)
                : qsTr("%n photos", "", gallery.photos.length)
        progress: gallery.plugin.status && gallery.plugin.status.phase === "scanning" && gallery.plugin.status.total > 0
                  ? gallery.plugin.status.done / gallery.plugin.status.total : -1
        closeHint: "⇧ D"
        tools: [
            ShellButton {
                objectName: "screensNot"
                text: qsTr("Not a screen")
                helpText: qsTr("Say the photo is not of a screen")
                action: notCommand
                hint: notCommand.keyName
            },
            ShellButton {
                objectName: "screensHide"
                text: qsTr("Hide from the grid")
                checkable: true
                active: gallery.plugin.hiding
                onTriggered: gallery.plugin.setHiding(!gallery.plugin.hiding)
            }
        ]
    }

    UiText {
        anchors.centerIn: parent
        visible: gallery.photos.length === 0
        color: theme.muted
        text: gallery.plugin.read ? qsTr("No photos of screens here.") : qsTr("Looking for photos of screens…")
    }

    PhotoTiles {
        id: tiles
        objectName: "screensTiles"
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        clip: true
        visible: gallery.photos.length > 0
        library: pinhole.library
        photos: gallery.photos
        active: view.shown && view.onTop
        minCell: theme.fontSize * 10
        Accessible.name: qsTr("Photos of screens")
        onOpened: (photo, index) => pinhole.openPhotos(gallery.photos, photo, { comeBack: true })
    }

    Command {
        id: notCommand
        name: "screens.not"
        keys: ["X"]
        keyLayer: "plugin"
        usable: view.shown && view.onTop && tiles.cursor >= 0 && tiles.cursor < gallery.photos.length
        mouse: qsTr("The Not a screen button")
        touchpad: qsTr("The Not a screen button")
        onTriggered: gallery.plugin.say([gallery.photos[tiles.cursor]], false)
    }
}
