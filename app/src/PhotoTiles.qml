import QtQuick
import QtQuick.Window

// Photos as square tiles, as a plugin's view shows what it found:
// search results, a place's photos, the newest additions. Each cropped to
// fill its square at the display's own pixels, a ring in the accent on the
// one the keys are on and a fainter one under the pointer; ← → ↑ ↓, Home
// and End, PgUp and PgDn move, ⏎ and Space open, while `active`. The
// wheel and the touchpad scroll it as the grid does (ScrollGrid).
//
//     PhotoTiles {
//         library: pinhole.library
//         photos: results.map(row => row.id)
//         active: shown
//         onOpened: (id, index) => pinhole.openPhotos(photos, id, { comeBack: true })
//     }
//
// `caption`, a Component, goes under each tile, `captionHeight` tall: its
// root declares `property string photo` (the id) and `property int index`,
// which are set.
FocusScope {
    id: tiles
    required property var library
    property var photos: []
    property int cursor: 0
    property bool active: false
    // The ring on the one the keys are on: while `active`, or as the view
    // says (on a search's best match while its field has the keys).
    property bool showCursor: active
    property real minCell: theme.fontSize * 11
    property real gap: 5
    property Component caption: null
    property real captionHeight: 0
    readonly property int count: photos.length
    readonly property int columns: Math.max(1, Math.floor(width / minCell))
    readonly property real cell: width / columns
    readonly property bool keyboard: keymap.input === "keyboard" // Pinhole's own: plugins have pinhole.keyboardInUse
    property alias view: grid
    // Each with the photo's id and its place among `photos`.
    signal opened(string photo, int index)
    // Up from the first row: for a field above to take the keys.
    signal pastTop()

    function move(by) {
        if (photos.length === 0) return
        const next = cursor + by
        if (next < 0 && by <= -columns && cursor < columns) {
            pastTop()
            return
        }
        cursor = Math.max(0, Math.min(photos.length - 1, next))
        grid.positionViewAtIndex(cursor, GridView.Contain)
    }
    // The keys to the photo `photo` (an id), in view: as a view comes back
    // to the one last looked at.
    function moveTo(photo) {
        const at = photos.indexOf(photo)
        if (at >= 0) {
            cursor = at
            grid.positionViewAtIndex(at, GridView.Contain)
        }
    }
    function open(index) {
        if (index < 0 || index >= photos.length) return
        cursor = index
        opened(photos[index], index)
    }
    onPhotosChanged: cursor = Math.max(0, Math.min(cursor, photos.length - 1))

    ScrollGrid {
        id: grid
        anchors.fill: parent
        focus: true
        model: tiles.photos.length
        cellWidth: tiles.cell
        cellHeight: tiles.cell + tiles.captionHeight
        keyNavigationEnabled: false
        Accessible.role: Accessible.List
        delegate: Item {
            id: tile
            required property int index
            objectName: "photoTile_" + index
            readonly property string photo: tiles.photos[index] ?? ""
            readonly property var item: (tiles.library.revision, tiles.library.item(photo))
            readonly property bool chosen: tiles.cursor === index && tiles.keyboard && tiles.showCursor
            width: tiles.cell - tiles.gap
            height: tiles.cell - tiles.gap + tiles.captionHeight
            Accessible.role: Accessible.Button
            Accessible.name: item.filename || qsTr("Photo %1").arg(index + 1)
            Accessible.onPressAction: tiles.open(index)
            Rectangle {
                id: square
                width: parent.width
                height: width
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.06)
                Image {
                    anchors.fill: parent
                    source: tile.item.thumbnail || ""
                    sourceSize: Qt.size(width, height)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: tile.chosen ? 2 : 1
                    border.color: theme.accent
                    visible: tile.chosen || hover.hovered
                }
            }
            Loader {
                y: square.height + 4
                width: parent.width
                height: tiles.captionHeight - 4
                active: tiles.caption !== null && tiles.captionHeight > 0
                sourceComponent: tiles.caption
                onLoaded: {
                    item.photo = Qt.binding(() => tile.photo)
                    item.index = Qt.binding(() => tile.index)
                }
            }
            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler { objectName: "photoTileTap"; onTapped: tiles.open(tile.index) }
        }
    }

    Repeater {
        model: [["Left", -1], ["Right", 1], ["Up", -tiles.columns], ["Down", tiles.columns],
                ["PgUp", -tiles.columns * Math.max(1, Math.floor(tiles.height / tiles.cell))],
                ["PgDown", tiles.columns * Math.max(1, Math.floor(tiles.height / tiles.cell))]]
        delegate: Item {
            required property var modelData
            Command {
                name: "tiles." + modelData[0]
                keys: [modelData[0]]
                keyLayer: "plugin"
                usable: tiles.active && tiles.photos.length > 0
                autoRepeat: true
                onTriggered: tiles.move(modelData[1])
            }
        }
    }
    Command {
        name: "tiles.first"
        keys: ["Home"]
        keyLayer: "plugin"
        usable: tiles.active && tiles.photos.length > 0
        onTriggered: tiles.move(-tiles.cursor)
    }
    Command {
        name: "tiles.last"
        keys: ["End"]
        keyLayer: "plugin"
        usable: tiles.active && tiles.photos.length > 0
        onTriggered: tiles.move(tiles.photos.length - 1 - tiles.cursor)
    }
    Command {
        name: "tiles.open"
        keys: ["Return", "Enter", "Space"]
        keyLayer: "plugin"
        usable: tiles.active && tiles.photos.length > 0
        onTriggered: tiles.open(tiles.cursor)
    }
}
