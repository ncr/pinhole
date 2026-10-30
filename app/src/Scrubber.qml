import QtQuick

// The whole library at the right edge of the grid, for a pointer: the years
// down a thin track, and where the view is. Pointed at, scrolled or dragged
// it shows; a click or a drag goes straight there, the date of that place
// beside the pointer. The way to the oldest photos, or any year, without
// scrolling there.
Item {
    id: scrubber
    property var grid
    // Shows while scrolling, a moment after.
    property bool scrolling: false
    readonly property bool shown: hover.containsMouse || area.pressed || scrolling
    // Where each year begins, as the grid says; again as the grid changes.
    readonly property var marks: (grid.columns, grid.count, grid.yearMarks())

    width: Math.round(theme.fontSize * 3.6)

    Connections {
        target: scrubber.grid
        function onScrolled() {
            scrubber.scrolling = true
            settled.restart()
        }
    }
    Timer { id: settled; interval: 1500; onTriggered: scrubber.scrolling = false }

    Item {
        anchors.fill: parent
        opacity: scrubber.shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Rectangle {
            id: track
            anchors.right: parent.right
            anchors.rightMargin: 6
            width: 3
            height: parent.height
            radius: 1.5
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25)
        }
        // Where the view is.
        Rectangle {
            anchors.horizontalCenter: track.horizontalCenter
            width: 7
            height: Math.max(12, Math.round(theme.fontSize * 1.4))
            radius: 3
            y: Math.round(scrubber.grid.scrollFraction * (scrubber.height - height))
            color: theme.accent
        }
        // The years, each where it begins, those too close to the last one
        // shown left out.
        Repeater {
            model: {
                const shown = []
                let last = -Infinity
                const gap = theme.fontSize * 1.6
                for (const m of scrubber.marks) {
                    const y = m.fraction * scrubber.height
                    if (y - last >= gap) {
                        shown.push({ year: m.year, y: y })
                        last = y
                    }
                }
                return shown
            }
            delegate: UiText {
                required property var modelData
                anchors.right: track.left
                anchors.rightMargin: 6
                y: Math.round(Math.min(scrubber.height - height, modelData.y))
                text: String(modelData.year)
                color: theme.darkForeground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                style: Text.Outline
                styleColor: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.8)
            }
        }
    }
    // Where the pointer is, while dragging: the date there.
    Chip {
        visible: area.pressed
        anchors.right: parent.left
        anchors.rightMargin: 4
        y: Math.round(Math.max(0, Math.min(scrubber.height - height, area.mouseY - height / 2)))
        textColor: theme.brightForeground
        text: {
            const ms = scrubber.grid.dateAt(area.fraction)
            return ms > 0 ? new Date(ms).toLocaleDateString(Qt.locale(), "MMMM yyyy") : ""
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        readonly property real fraction: Math.max(0, Math.min(1, mouseY / Math.max(1, height)))
        // The wheel and the touchpad still scroll the grid under it.
        onWheel: wheel => wheel.accepted = false
        onPressed: {
            scrubber.grid.setScrubbing(true)
            scrubber.grid.scrubTo(fraction)
        }
        onPositionChanged: if (pressed) scrubber.grid.scrubTo(fraction)
        onReleased: scrubber.grid.setScrubbing(false)
        onCanceled: scrubber.grid.setScrubbing(false)
    }
    // The pointer over it, alone, for `shown`.
    readonly property alias hover: area
}
