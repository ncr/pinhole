import QtQuick
import QtQuick.Window
import QtQuick.Effects
import Pinhole.Plugin

// A face from its thumbnail, centred and filling `fraction` of the tile:
// round for a person or a group, square with some of the photo around it
// for one of their photos.
Item {
    id: tile
    required property var row
    property bool round: true
    property real fraction: round ? 0.5 : 0.36
    property bool chosen: false
    property bool hovered: hover.hovered
    signal selected()

    Accessible.role: Accessible.Button
    Accessible.onPressAction: selected()

    Item {
        id: picture
        anchors.fill: parent
        clip: !tile.round
        layer.enabled: tile.round
        layer.smooth: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1
        }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.08)
        }
        Image {
            id: image
            readonly property var photo: (pinhole.library.revision, pinhole.library.item(tile.row.id))
            readonly property real ratio: Math.max(1, photo.width || 1) / Math.max(1, photo.height || 1)
            readonly property var box: tile.row.box || [0, 0, 1, 1]
            readonly property real side: Math.max(1, picture.width)
            // The width the whole thumbnail is drawn at, for the face to
            // take `fraction` of the tile.
            readonly property real drawn: Math.max(side, side * ratio, tile.fraction * side / Math.max(0.01, box[2], box[3] / ratio))
            readonly property real centreX: box[0] + box[2] / 2
            readonly property real centreY: box[1] + box[3] / 2
            function placed(at, length) {
                // Keep the tile covered where the photo allows it.
                return length <= side ? (side - length) / 2 : Math.min(0, Math.max(side - length, at))
            }
            width: drawn
            height: drawn / ratio
            x: placed(side / 2 - centreX * width, width)
            y: placed(side / 2 - centreY * height, height)
            source: photo.thumbnail || ""
            sourceSize: Qt.size(Math.min(1024, width * Screen.devicePixelRatio), Math.min(1024, height * Screen.devicePixelRatio))
            asynchronous: true
            smooth: true
        }
    }
    Item {
        id: mask
        anchors.fill: parent
        visible: false
        layer.enabled: tile.round
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "black"
        }
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: tile.round ? -4 : 0
        radius: tile.round ? width / 2 : 0
        color: "transparent"
        border.color: theme.accent
        border.width: tile.chosen ? 2 : 1
        visible: tile.chosen || hover.hovered
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: tile.selected() }
}
