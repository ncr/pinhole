import QtQuick

// Every shortcut, on ?. As Omarchy apps do.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false

    readonly property var sections: [
        { title: qsTr("Browsing"), keys: [
            [qsTr("Two fingers"), qsTr("Scroll; flick to coast, rest a finger to stop")],
            [qsTr("Pinch"), qsTr("Zoom in and out")],
            ["+  −", qsTr("Zoom one step")],
            [qsTr("Ctrl+scroll"), qsTr("Zoom one step at the pointer")],
            ["Home  End", qsTr("Oldest, newest")],
            ["PgUp  PgDn", qsTr("A screen up or down")],
            [qsTr("Right edge"), qsTr("Drag the date scrubber")]
        ] },
        { title: qsTr("Selecting"), keys: [
            [qsTr("Click"), qsTr("Select a photo")],
            [qsTr("Ctrl+click"), qsTr("Add or remove one")],
            [qsTr("Shift+click"), qsTr("Select a range")],
            ["Ctrl+A", qsTr("Select all")],
            ["Esc", qsTr("Clear the selection")]
        ] },
        { title: qsTr("Taking photos out"), keys: [
            [qsTr("Drag"), qsTr("Drop the selection into any app")],
            ["Ctrl+C", qsTr("Copy")],
            ["Ctrl+E", qsTr("Export originals")],
            ["Ctrl+Shift+E", qsTr("Export as JPEG and MP4")]
        ] },
        { title: qsTr("Viewing"), keys: [
            [qsTr("Double-click"), qsTr("Open the photo")],
            ["Space  Enter", qsTr("Open the photo under the pointer")],
            ["←  →", qsTr("Previous, next")],
            ["Space", qsTr("Play or pause a video; close a photo")],
            ["L", qsTr("Play a Live Photo")],
            ["Esc", qsTr("Close")]
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
                        model: root.sections.filter((s, i) => Math.floor(i * root.columns / root.sections.length) === column.index)
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
