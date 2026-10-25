import QtQuick

// The date scrubber along the right edge, as in Photos: it shows up while
// the grid moves or the pointer is near the edge, marks where each year
// begins, and dragging it jumps through the library with the month and year
// under the pointer. On a dark band of its own, so the years read over any
// photos.
Item {
    id: root

    required property var grid

    width: 84
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.right: parent.right

    readonly property bool active: drag.pressed || hover.hovered || recent.running
    property var marks: []

    function refreshMarks() { marks = grid.yearMarks() }
    Component.onCompleted: refreshMarks()
    onVisibleChanged: if (visible) refreshMarks()
    Connections {
        target: root.grid
        function onColumnsChanged() { root.refreshMarks() }
        function onLibraryChanged() { root.refreshMarks() }
        function onScrolled() { recent.restart() }
    }
    // Shown for a moment after the grid last moved.
    Timer { id: recent; interval: 1400 }

    opacity: active ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    HoverHandler { id: hover }

    // Below the Select button at the top right.
    readonly property real trackTop: 74
    readonly property real trackHeight: height - trackTop - 26

    // The band the years sit on.
    Rectangle {
        id: band
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 58
        anchors.bottomMargin: 10
        width: 60
        radius: Math.min(10, theme.radius + 4)
        color: "#d0101012"
    }

    // The year each mark begins, where it does not crowd the one above.
    Repeater {
        model: root.marks
        delegate: Text {
            required property var modelData
            required property int index
            readonly property real at: root.trackTop + modelData.fraction * root.trackHeight
            // Shown unless within 22 px of the last one shown above it.
            readonly property bool room: {
                let last = -100
                for (let i = 0; i < index; ++i) {
                    const y = root.trackTop + root.marks[i].fraction * root.trackHeight
                    if (y - last >= 22)
                        last = y
                }
                return at - last >= 22
            }
            visible: room
            anchors.right: band.right
            anchors.rightMargin: 12
            y: at - height / 2
            text: modelData.year
            color: "white"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
    }

    // Where the view is now: a line across the band with a knob at its
    // edge, the thing to grab.
    Item {
        id: pointer
        anchors.left: band.left
        anchors.right: band.right
        y: root.trackTop + root.grid.scrollFraction * root.trackHeight
        Rectangle {
            anchors.verticalCenter: parent.top
            width: parent.width
            height: 2
            color: theme.accent
        }
        Rectangle {
            anchors.verticalCenter: parent.top
            x: -width / 2
            width: 16
            height: 16
            radius: 8
            color: theme.accent
            border.color: "white"
            border.width: 2
        }
    }

    // The month and year at the pointer, while dragging.
    Rectangle {
        visible: drag.pressed
        anchors.right: band.left
        anchors.rightMargin: 16
        y: pointer.y - height / 2
        width: dateLabel.implicitWidth + 24
        height: dateLabel.implicitHeight + 12
        radius: height / 2
        color: "#f0161618"
        border.color: theme.accent
        border.width: 1
        Text {
            id: dateLabel
            anchors.centerIn: parent
            text: Qt.locale().toString(new Date(root.grid.dateAt(root.grid.scrollFraction)), "MMMM yyyy")
            color: "white"
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        id: drag
        anchors.fill: parent
        preventStealing: true
        function scrubAt(y) {
            root.grid.scrubTo(Math.max(0, Math.min(1, (y - root.trackTop) / root.trackHeight)))
        }
        onPressed: mouse => {
            root.grid.setScrubbing(true)
            scrubAt(mouse.y)
        }
        onPositionChanged: mouse => { if (pressed) scrubAt(mouse.y) }
        onReleased: root.grid.setScrubbing(false)
        onCanceled: root.grid.setScrubbing(false)
    }
}
