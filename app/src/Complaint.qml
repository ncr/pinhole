import QtQuick
import QtQuick.Window

// What pressing a button could not do, in a few words: under the bar it is
// in (over it, for a bar at the bottom), at the button and within the
// window, for four seconds, the button shaking as Copy does when nothing
// could be copied. MenuGroup.qml makes one the first time it is needed.
Chip {
    id: complaint
    objectName: "complaint"
    // Where it is said, and what shakes.
    property Item button
    property Translate nudged
    property bool upward: false
    // Said now, for the four seconds (self-tests).
    readonly property bool saying: shown.running
    textColor: theme.darkForeground
    visible: opacity > 0
    opacity: 0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    function say(message) {
        text = message
        const window = complaint.Window.window ? complaint.Window.window.contentItem : null
        const at = button ? button.mapToItem(parent, 0, 0) : Qt.point(0, 0)
        const left = window ? parent.mapToItem(window, 0, 0).x : 0
        const room = window ? window.width : 1e9
        x = Math.max(16, Math.min(left + at.x, room - 16 - width)) - left
        y = upward ? at.y - height - 7 : at.y + (button ? button.height : 0) + 7
        opacity = 1
        shown.restart()
        if (nudged)
            nudge.restart()
    }
    function hush() {
        shown.stop()
        opacity = 0
    }
    Timer { id: shown; interval: 4000; onTriggered: complaint.opacity = 0 }
    SequentialAnimation {
        id: nudge
        NumberAnimation { target: complaint.nudged; property: "x"; to: -5; duration: 50 }
        NumberAnimation { target: complaint.nudged; property: "x"; to: 5; duration: 80 }
        NumberAnimation { target: complaint.nudged; property: "x"; to: -3; duration: 70 }
        NumberAnimation { target: complaint.nudged; property: "x"; to: 0; duration: 60 }
    }
}
