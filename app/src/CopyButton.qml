import QtQuick

// A Copy button that answers on itself, as touch apps do, rather than in a
// line of text somewhere else: dimmed while the photos are fetched, then a
// tick for a moment, or a shake if nothing could be copied. Drawn as an
// Omarchy control (see ShellButton.qml); `flat` for a row in a bar, `hint` for
// its keys beside it.
Rectangle {
    id: button

    // The photos to copy, asked for when pressed.
    property var photos: function () { return [] }
    property bool enabledHere: true
    property bool flat: false
    // Over the photos rather than on a surface of the app: backed with the
    // theme's background, as the chips are, so it reads on any photo.
    property bool floating: false
    property string hint: ""

    // idle, working, done, failed
    property string phase: "idle"

    readonly property color ink: theme.foreground
    width: Math.max(label.implicitWidth, tick.implicitWidth) + 2 * Math.round(theme.fontSize * 0.833)
    height: Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(ink.r, ink.g, ink.b, tap.pressed ? 0.22 : hover.hovered && enabledHere ? 0.08 : flat ? 0 : 0.04)
    border.width: 1
    border.color: Qt.rgba(ink.r, ink.g, ink.b, hover.hovered && enabledHere ? 0.25 : flat ? 0 : 0.4)
    opacity: !enabledHere ? 0.4 : phase === "working" ? 0.6 : 1
    // Every press shows: the button gives a little under the finger.
    scale: tap.pressed ? 0.94 : 1
    Behavior on scale { NumberAnimation { duration: 80 } }

    UiText {
        id: label
        anchors.horizontalCenter: parent.horizontalCenter
        y: centredIn(button.height)
        text: qsTr("Copy") + (button.hint ? "  <font color=\"" + theme.darkForeground + "\">" + button.hint + "</font>" : "")
        opacity: button.phase === "done" ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 90 } }
    }
    // The tick, in the label's place.
    UiText {
        id: tick
        anchors.horizontalCenter: parent.horizontalCenter
        y: centredIn(button.height)
        text: "✓ " + qsTr("Copied")
        color: theme.accent
        opacity: button.phase === "done" ? 1 : 0
    }

    // The tick pops in; how long it stays is the timer's (an animation's
    // end can come late when the window draws few frames, and then the tick
    // stayed up and a second click changed nothing).
    SequentialAnimation {
        id: pop
        NumberAnimation { target: tick; property: "scale"; from: 0.6; to: 1.12; duration: 140; easing.type: Easing.OutCubic }
        NumberAnimation { target: tick; property: "scale"; to: 1; duration: 120; easing.type: Easing.InOutQuad }
    }
    Timer {
        id: shown
        interval: 1400
        onTriggered: {
            pop.stop()
            tick.scale = 1
            button.phase = "idle"
        }
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
                shown.restart()
            } else {
                button.phase = "failed"
                shake.restart()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        visible: button.floating
        radius: parent.radius
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    }
    HoverHandler { id: hover; cursorShape: button.enabledHere ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler {
        id: tap
        onTapped: {
            if (!button.enabledHere || button.phase === "working")
                return
            const photos = button.photos()
            if (photos.length === 0)
                return
            // Again while the tick shows: it starts afresh.
            pop.stop()
            shown.stop()
            button.phase = "working"
            outgoing.copy(photos, true)
        }
    }
}
