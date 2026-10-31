import QtQuick
import QtQuick.Window
import Pinhole.Plugin
import "Colour.js" as Colour

// The rainbow wall: as many of the library's most colourful photos as fill
// the window, round the hues from left to right, light at the top to dark
// at the foot (Colour.wall). Each tile is its photo's average colour until
// the thumbnail is in, so the wall is a rainbow at once and the photos
// come into it. A tile clicked opens its photo; Space makes another wall
// of other photos; + and -, or a pinch, make the tiles bigger and smaller.
Item {
    id: wall
    required property var finder
    readonly property var pinhole: finder.pinhole

    // The tile's size, by step.
    readonly property var densities: [36, 48, 64, 88, 120]
    property int density: 2
    readonly property int tile: densities[density]
    readonly property int columns: Math.max(1, Math.floor(width / tile))
    readonly property int rows: Math.max(1, Math.floor(height / tile))
    // Red to pink, starting at the pinks.
    readonly property real start: 345
    property int seed: 0
    property var tiles: []
    readonly property int shownCount: tiles.filter(i => i >= 0).length

    // What the tiles were laid for, so coming back to the wall as it was
    // (from a photo opened on it) does not lay it again.
    property var laid: null
    function lay() {
        const d = finder.plugin.colours
        if (!visible || !d)
            return
        if (laid && laid.colours === d && laid.columns === columns && laid.rows === rows && laid.seed === seed)
            return
        laid = { colours: d, columns: columns, rows: rows, seed: seed }
        tiles = Colour.wall(d, columns, rows, start, seed)
    }
    function another() {
        seed = seed + 1 + Math.floor(Math.random() * 1000)
        lay()
    }
    // Laid out again once the size holds still.
    Timer { id: settle; interval: 120; onTriggered: wall.lay() }
    onColumnsChanged: settle.restart()
    onRowsChanged: settle.restart()
    onVisibleChanged: if (visible) lay()
    Connections {
        target: wall.finder.plugin
        function onColoursChanged() { settle.restart() }
    }

    // A pinch makes the tiles bigger or smaller, a step at a time.
    PinchHandler {
        target: null
        property real from: 1
        onActiveChanged: if (active) from = activeScale
        onActiveScaleChanged: {
            if (activeScale / from > 1.3 && wall.density < wall.densities.length - 1) {
                wall.density += 1
                from = activeScale
            } else if (activeScale / from < 0.77 && wall.density > 0) {
                wall.density -= 1
                from = activeScale
            }
        }
    }

    // The wall in the middle of its room.
    Item {
        id: board
        width: wall.columns * wall.tile
        height: wall.rows * wall.tile
        anchors.centerIn: parent
        Repeater {
            model: wall.tiles
            delegate: Item {
                id: cell
                required property int modelData
                required property int index
                readonly property int column: Math.floor(index / wall.rows)
                readonly property int row: index % wall.rows
                readonly property var item: modelData >= 0 ? wall.pinhole.library.item(modelData) : null
                x: column * wall.tile
                y: row * wall.tile
                width: wall.tile
                height: wall.tile
                z: hover.hovered ? 2 : 0
                visible: modelData >= 0

                Rectangle {
                    id: face
                    anchors.fill: parent
                    anchors.margins: 1
                    color: cell.item ? cell.item.avgColor : "transparent"
                    clip: true
                    // A wave from left to right as the wall comes in.
                    opacity: 0
                    scale: hover.hovered ? 2.2 : 1
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    border.width: hover.hovered ? 1 : 0
                    border.color: theme.brightForeground
                    SequentialAnimation on opacity {
                        PauseAnimation { duration: cell.column * 12 + cell.row * 4 }
                        NumberAnimation { to: 1; duration: 260; easing.type: Easing.OutCubic }
                    }
                    Image {
                        anchors.fill: parent
                        anchors.margins: face.border.width
                        source: cell.item ? cell.item.thumbnail : ""
                        // Sharp enough to be looked at close up.
                        sourceSize: Qt.size(Math.round(wall.tile * 2.2 * Screen.devicePixelRatio),
                                            Math.round(wall.tile * 2.2 * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 300 } }
                    }
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: wall.finder.open(cell.modelData) }
            }
        }
    }
}
