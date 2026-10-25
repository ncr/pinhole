import QtQuick

// Every shortcut, on ?. As Omarchy apps do.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false

    // Touchpad and mouse side by side: each does everything.
    readonly property var sections: [
        { title: qsTr("Browsing"), keys: [
            [qsTr("Click"), qsTr("Open the photo")],
            [qsTr("Two fingers"), qsTr("Scroll; flick to coast, rest a finger or tap to stop")],
            [qsTr("Wheel"), qsTr("Scroll")],
            [qsTr("Pinch"), qsTr("Zoom in and out")],
            [qsTr("Ctrl+wheel"), qsTr("Zoom one step at the pointer")],
            ["+  −", qsTr("Zoom one step")],
            ["Home  End", qsTr("Oldest, newest")],
            ["PgUp  PgDn", qsTr("A screen up or down")],
            [qsTr("Right edge"), qsTr("Drag the date scrubber")]
        ] },
        { title: qsTr("Selecting"), keys: [
            [qsTr("Select"), qsTr("The button at the top: start choosing photos")],
            [qsTr("Click"), qsTr("Tick or untick a photo, while selecting")],
            [qsTr("Shift+click"), qsTr("Tick a range")],
            [qsTr("Ctrl+click"), qsTr("Start selecting with this photo")],
            ["Ctrl+A", qsTr("Select all")],
            [qsTr("Done  Esc"), qsTr("Stop selecting")]
        ] },
        { title: qsTr("Taking photos out"), keys: [
            [qsTr("Drag"), qsTr("Drop the selection into any app")],
            [qsTr("Copy  Export"), qsTr("The buttons above an open photo")],
            ["Ctrl+C", qsTr("Copy")],
            ["Ctrl+E", qsTr("Export originals")],
            ["Ctrl+Shift+E", qsTr("Export as JPEG and MP4")]
        ] },
        { title: qsTr("Viewing"), keys: [
            ["Space  Enter", qsTr("Open the photo under the pointer")],
            [qsTr("Swipe sideways"), qsTr("Previous, next: the photo follows two fingers")],
            [qsTr("Click a side"), qsTr("Previous, next")],
            [qsTr("Wheel"), qsTr("Previous, next")],
            ["←  →", qsTr("Previous, next")],
            [qsTr("Carousel"), qsTr("Click or drag to move through the library")],
            [qsTr("Double-click"), qsTr("Back to the library")],
            [qsTr("Pinch in"), qsTr("Back to the library")],
            [qsTr("Esc  Mouse back"), qsTr("Back to the library")]
        ] },
        { title: qsTr("Zooming in"), keys: [
            [qsTr("Pinch"), qsTr("Zoom the photo at the fingers")],
            [qsTr("Ctrl+wheel"), qsTr("Zoom at the pointer")],
            ["+  −  0", qsTr("Zoom in, out, back to fitted")],
            [qsTr("Drag"), qsTr("Move around a zoomed photo; or two fingers, flick to coast")],
            [qsTr("Double-click"), qsTr("Back to fitted")]
        ] },
        { title: qsTr("Videos"), keys: [
            [qsTr("Click  Space"), qsTr("Play or pause")],
            [qsTr("Video bar"), qsTr("Seek: click or drag")],
            ["Shift+←  →", qsTr("Back or ahead 10 seconds")],
            ["M", qsTr("Sound on or off")],
            ["L", qsTr("Play a Live Photo")]
        ] },
        { title: qsTr("Pinhole"), keys: [
            ["?", qsTr("Show or hide these keys")],
            ["S", qsTr("Show or hide the frame statistics")],
            ["Q", qsTr("Quit")]
        ] }
    ]

    Rectangle {
        anchors.fill: parent
        color: "#c0000000"
    }
    MouseArea {
        anchors.fill: parent
        onClicked: root.shown = false
    }

    // As many columns of 340 as fit, up to three.
    readonly property int columns: Math.max(1, Math.min(3, Math.floor((width - 96) / 372)))

    // The sections in order, cut into columns as even in height as can be:
    // every way of cutting them is tried, there being few.
    readonly property var columnOf: {
        const rows = sections.map(s => s.keys.length + 2)
        let best = null, bestHeight = Infinity
        const cut = (from, column, assigned, tallest) => {
            if (column === columns - 1) {
                const last = rows.slice(from).reduce((a, b) => a + b, 0)
                const height = Math.max(tallest, last)
                if (height < bestHeight) {
                    bestHeight = height
                    best = assigned.concat(rows.slice(from).map(() => column))
                }
                return
            }
            for (let to = from + 1; to <= rows.length - (columns - 1 - column); ++to) {
                const height = rows.slice(from, to).reduce((a, b) => a + b, 0)
                cut(to, column + 1, assigned.concat(rows.slice(from, to).map(() => column)), Math.max(tallest, height))
            }
        }
        cut(0, 0, [], 0)
        return best
    }

    Rectangle {
        anchors.centerIn: parent
        width: content.implicitWidth + 48
        height: Math.min(parent.height - 48, content.implicitHeight + 48)
        radius: Math.min(10, theme.radius)
        color: "#f0161618"
        border.color: theme.accent
        border.width: 1
        clip: true

        // The sections in order, split over the columns.
        Row {
            id: content
            x: 24
            y: 24
            spacing: 32
            Repeater {
                model: root.columns
                delegate: Column {
                    id: column
                    required property int index
                    spacing: 24
                    Repeater {
                        model: root.sections.filter((s, i) => root.columnOf[i] === column.index)
                        delegate: Column {
                            required property var modelData
                            spacing: 6
                            width: 340
                            Text {
                                text: parent.modelData.title
                                color: theme.accent
                                font.pixelSize: 14
                                font.bold: true
                                bottomPadding: 4
                            }
                            Repeater {
                                model: parent.modelData.keys
                                delegate: Row {
                                    required property var modelData
                                    spacing: 12
                                    Text {
                                        width: 140
                                        text: parent.modelData[0]
                                        color: "white"
                                        font.pixelSize: 12
                                        font.family: "monospace"
                                        horizontalAlignment: Text.AlignRight
                                        elide: Text.ElideLeft
                                    }
                                    Text {
                                        width: 188
                                        text: parent.modelData[1]
                                        color: "#d0ffffff"
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
