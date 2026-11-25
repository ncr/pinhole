import QtQuick
import Pinhole.Plugin

// The rainbow wall, over the window: the wall (Wall.qml) under a row that
// says what it is of, makes another, and makes its tiles bigger and
// smaller. A photo clicked opens full window; closing it comes back here.
Item {
    id: rainbow
    readonly property var plugin: view.plugin

    // Opened (Main.qml): the library's colours read, if they are not yet or
    // it has changed since. The wall is as it was left.
    function start() { plugin.ensure() }
    // A photo on the wall, by its id.
    function open(id) {
        const index = pinhole.library.indexOf(id)
        if (index < 0)
            return
        // ← → through the wall's photos; back here when it closes
        // (onReturned).
        const among = wall.tiles.filter(tile => tile !== "" && !pinhole.isHidden(tile))
        pinhole.openPhotos(among.indexOf(id) >= 0 ? among : [id], id, { comeBack: true })
    }
    // Photos hidden from the grid or shown again (Main.qml).
    function hiddenMoved() { wall.hiddenMoved() }
    // The wall's tiles a step bigger.
    readonly property bool canGrow: wall.density < wall.densities.length - 1
    function grow() {
        if (canGrow)
            wall.zoomBy(1)
    }

    Connections {
        target: pinhole
        // As it was left: the wall's photos stay, by their ids, and the
        // library's colours are read again if it has changed, for the wall
        // made next.
        function onReturned(photo) {
            rainbow.plugin.ensure()
            wall.keepFound()
        }
    }

    // Nothing over it reaches the photos beneath.
    Panel {}

    // What is shown, where there is room for it.
    Chip {
        x: 16
        y: 16
        visible: rainbow.plugin.colours !== null && x + width + 12 <= controls.x
        textColor: theme.darkForeground
        text: qsTr("%Ln of your most colourful photos, by hue", "", wall.shownCount)
    }
    Row {
        id: controls
        anchors.right: closeButton.left
        anchors.rightMargin: 6
        y: 16
        spacing: 6
        ShellButton {
            text: qsTr("Another")
            key: "Space"
            keyLayer: "plugin"
            hint: "Space"
            onTriggered: wall.another()
        }
        // The text is the key.
        ShellButton {
            text: "−"
            key: "-"
            keyLayer: "plugin"
            hint: ""
            usable: wall.density > 0
            onTriggered: wall.zoomBy(-1)
        }
        ShellButton {
            text: "+"
            key: "+"
            keyLayer: "plugin"
            hint: ""
            usable: rainbow.canGrow
            onTriggered: wall.zoomBy(1)
        }
    }
    ShellButton {
        id: closeButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        hint: "Esc"
        onTriggered: view.close()
    }

    Wall {
        id: wall
        anchors.fill: parent
        anchors.topMargin: controls.y + controls.height + 16
        anchors.margins: 16
        plugin: rainbow.plugin
        onPicked: id => rainbow.open(id)
    }
}
