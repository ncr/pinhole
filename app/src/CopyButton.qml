import QtQuick

// A Copy button that answers on itself, as touch apps do, rather than in a
// line of text somewhere else: dimmed while the photos are fetched, then a
// tick for a moment, or a shake if nothing could be copied.
Rectangle {
    id: button

    // The photos to copy, asked for when pressed.
    property var photos: function () { return [] }
    property bool enabledHere: true
    property color idleColor: "#a0202024"
    property color hoverColor: "#e0303036"
    property real fontSize: 12
    property bool pill: true

    // idle, working, done, failed
    property string phase: "idle"

    width: label.implicitWidth + 22
    height: label.implicitHeight + 10
    radius: pill ? height / 2 : Math.min(6, theme.radius)
    color: hover.hovered && enabledHere && phase === "idle" ? hoverColor : idleColor
    opacity: !enabledHere ? 0.4 : phase === "working" ? 0.6 : 1

    Text {
        id: label
        anchors.centerIn: parent
        text: qsTr("Copy")
        color: "white"
        font.pixelSize: button.fontSize
        opacity: button.phase === "done" ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 90 } }
    }
    // The tick, in the label's place.
    Text {
        id: tick
        anchors.centerIn: parent
        text: "✓"
        color: theme.accent
        font.pixelSize: button.fontSize + 5
        font.bold: true
        opacity: button.phase === "done" ? 1 : 0
        scale: 1
    }

    SequentialAnimation {
        id: pop
        NumberAnimation { target: tick; property: "scale"; from: 0.4; to: 1.25; duration: 140; easing.type: Easing.OutCubic }
        NumberAnimation { target: tick; property: "scale"; to: 1; duration: 120; easing.type: Easing.InOutQuad }
        PauseAnimation { duration: 1200 }
        ScriptAction { script: button.phase = "idle" }
    }
    SequentialAnimation {
        id: shake
        loops: 1
        NumberAnimation { target: label; property: "anchors.horizontalCenterOffset"; to: -5; duration: 50 }
        NumberAnimation { target: label; property: "anchors.horizontalCenterOffset"; to: 5; duration: 80 }
        NumberAnimation { target: label; property: "anchors.horizontalCenterOffset"; to: -3; duration: 70 }
        NumberAnimation { target: label; property: "anchors.horizontalCenterOffset"; to: 0; duration: 60 }
        ScriptAction { script: button.phase = "idle" }
    }

    Connections {
        target: outgoing
        function onCopied(files, failed) {
            if (button.phase !== "working")
                return
            if (files > 0) {
                button.phase = "done"
                pop.restart()
            } else {
                button.phase = "failed"
                shake.restart()
            }
        }
    }

    HoverHandler { id: hover; cursorShape: button.enabledHere ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler {
        onTapped: {
            if (!button.enabledHere || button.phase === "working")
                return
            const photos = button.photos()
            if (photos.length === 0)
                return
            pop.stop()
            button.phase = "working"
            outgoing.copy(photos, true)
        }
    }
}
