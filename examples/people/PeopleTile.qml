import QtQuick
import Pinhole.Plugin

Item {
    id: tile
    required property var row
    required property bool group
    required property bool chosen
    required property string label
    signal selected()

    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.onPressAction: selected()
    Rectangle {
        anchors.fill: parent
        anchors.margins: 3
        color: theme.background
        border.color: theme.accent
        border.width: tile.chosen ? 2 : 0
        Item {
            id: picture
            anchors.fill: parent
            anchors.margins: 3
            anchors.bottomMargin: tile.group ? caption.height + 4 : 3
            clip: true
            Image {
                id: image
                readonly property var photo: pinhole.library.item(tile.row.id)
                readonly property real ratio: Math.max(1, photo.width || 1) / Math.max(1, photo.height || 1)
                readonly property var box: tile.row.box
                // Fit the face into its group tile. Match photos show the
                // whole thumbnail and outline the particular face.
                readonly property real cropWidth: Math.min(picture.width / Math.max(0.01, box[2]),
                    picture.height * ratio / Math.max(0.01, box[3])) * 0.8
                x: tile.group ? (picture.width - width * (2 * box[0] + box[2])) / 2 : 0
                y: tile.group ? (picture.height - height * (2 * box[1] + box[3])) / 2 : 0
                width: tile.group ? cropWidth : picture.width
                height: tile.group ? cropWidth / ratio : picture.height
                source: photo.thumbnail || ""
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                Rectangle {
                    visible: !tile.group && image.status === Image.Ready
                    color: "transparent"
                    border.color: theme.accent
                    border.width: 2
                    x: (image.width - image.paintedWidth) / 2 + image.box[0] * image.paintedWidth
                    y: (image.height - image.paintedHeight) / 2 + image.box[1] * image.paintedHeight
                    width: image.box[2] * image.paintedWidth
                    height: image.box[3] * image.paintedHeight
                }
            }
        }
        UiText {
            id: caption
            visible: tile.group
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 4
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.label
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: tile.selected() }
}
