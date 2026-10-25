import QtQuick

// The date scrubber along the right edge, as in Photos: it shows up while
// the grid moves or the pointer is near the edge, marks where each year
// begins, and dragging it jumps through the library with the month and year
// under the pointer.
Item {
    id: root

    required property var grid

    width: 72
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.right: parent.right

    readonly property bool active: drag.pressed || hover.hovered || recent.running
    property var marks: []

    function refreshMarks() { marks = grid.yearMarks() }
    Component.onCompleted: refreshMarks()
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

    readonly property real trackTop: 16
    readonly property real trackHeight: height - 32

    // Year marks, with labels where they do not crowd each other.
    Repeater {
        model: root.marks
        delegate: Item {
            required property var modelData
            required property int index
            readonly property real at: root.trackTop + modelData.fraction * root.trackHeight
            readonly property real previous: index > 0 ? root.trackTop + root.marks[index - 1].fraction * root.trackHeight : -100
            y: at
            width: root.width
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 4
                width: 6
                height: 1
                color: "#80ffffff"
            }
            Text {
                visible: parent.at - parent.previous > 16
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.top
                text: parent.modelData.year
                color: "#b0ffffff"
                font.pixelSize: 10
                style: Text.Outline
                styleColor: "#a0000000"
            }
        }
    }

    // Where the view is now.
    Rectangle {
        id: thumb
        anchors.right: parent.right
        anchors.rightMargin: 2
        width: 4
        height: 28
        radius: 2
        color: "white"
        y: root.trackTop + root.grid.scrollFraction * root.trackHeight - height / 2
    }

    // The date at the thumb, while dragging.
    Rectangle {
        visible: drag.pressed
        anchors.right: thumb.left
        anchors.rightMargin: 8
        y: thumb.y + thumb.height / 2 - height / 2
        width: dateLabel.implicitWidth + 16
        height: dateLabel.implicitHeight + 8
        radius: 4
        color: "#e0202024"
        Text {
            id: dateLabel
            anchors.centerIn: parent
            text: Qt.locale().toString(new Date(root.grid.dateAt(root.grid.scrollFraction)), "MMMM yyyy")
            color: "white"
            font.pixelSize: 13
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
