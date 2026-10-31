import QtQuick
import Pinhole.Plugin

// The frame itself, over the whole window: two slides, the one showing
// over the next, which loads beneath it unseen and starts moving as the
// one above fades away. Then they change places, and the one beneath
// loads the photo after.
Item {
    id: frame
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }

    // The photos, in the order shown, and where in it the one showing is.
    property var order: []
    property int at: 0
    // Started on a photo full window: stopping goes back to the viewer, on
    // the last one shown.
    property bool fromViewer: false
    property bool paused: false

    // The slide showing, and the one under it.
    property Slide current: slideA
    property Slide next: slideB

    readonly property int fadeMs: 1400
    // A slide moves through its time and its fades.
    readonly property real span: plugin.seconds + 2 * fadeMs / 1000

    function start(list, fromViewer) {
        if (list.length === 0)
            return
        frame.fromViewer = fromViewer
        order = list
        at = 0
        paused = false
        fade.stop()
        waiting.stop()
        current.z = 1
        current.opacity = 1
        next.z = 0
        next.opacity = 1
        current.load(order[0])
        current.begin(span)
        next.load(order.length > 1 ? order[1] : -1)
        prefetch(2)
        advance.interval = plugin.seconds * 1000
        advance.restart()
        shown = true
        pointer.wake()
        // A video open in the viewer would go on beneath, heard.
        if (fromViewer)
            pinhole.viewer.close()
    }
    // The whole order, found after the first photo was up: it begins with
    // the one showing. Taken once no fade is under way.
    function extend(list) {
        if (fade.running || waiting.running) {
            extendLater.list = list
            extendLater.restart()
            return
        }
        order = list
        at = 0
        if (order.length > 1)
            next.load(order[1])
        prefetch(2)
        // Its time up before the rest was found: on at once.
        if (!advance.running && !paused)
            go(1, false)
    }
    Timer {
        id: extendLater
        property var list
        interval: 200
        onTriggered: frame.extend(list)
    }
    function stop() {
        if (!shown)
            return
        shown = false
        extendLater.stop()
        plugin.scan = null
        advance.stop()
        waiting.stop()
        fade.stop()
        const last = current.index
        slideA.load(-1)
        slideB.load(-1)
        if (last < 0)
            return
        if (fromViewer)
            pinhole.openPhoto(last)
        else
            pinhole.reveal(last)
    }
    function setPaused(on) {
        paused = on
        current.setPaused(on)
        if (on)
            advance.stop()
        else if (!fade.running && !waiting.running) {
            advance.interval = plugin.seconds * 1000
            advance.restart()
        }
        pointer.wake()
    }

    // The medium of the photo after next, fetched ahead.
    function prefetch(ahead) {
        if (order.length > ahead)
            pinhole.fetch(order[(at + ahead) % order.length], "medium").then(() => {}, () => {})
    }

    // To the photo `delta` on: once the slide beneath has it sharp, or has
    // waited long enough (a step by hand waits less).
    property int target: -1
    function go(delta, byHand) {
        if (order.length < 2)
            return
        // A fade under way ends at once.
        if (fade.running) {
            fade.stop()
            swap()
        }
        target = ((waiting.running ? target : at) + delta + order.length) % order.length
        if (next.index !== order[target])
            next.load(order[target])
        advance.stop()
        waiting.forward = delta >= 0
        waiting.skipped = 0
        waiting.byHand = byHand
        waiting.since = Date.now()
        waiting.restart()
        waiting.check()
    }
    Timer {
        id: waiting
        property bool byHand: false
        property bool forward: true
        property int skipped: 0
        property double since: 0
        interval: 40
        repeat: true
        function check() {
            if (!running)
                return
            // One the service has no medium for is passed over, a few at
            // most in a row.
            if (frame.next.missing && skipped < 8 && frame.order.length > 2) {
                ++skipped
                since = Date.now()
                frame.target = (frame.target + (forward ? 1 : -1) + frame.order.length) % frame.order.length
                frame.next.load(frame.order[frame.target])
                return
            }
            if (frame.next.ready || Date.now() - since > (byHand ? 700 : 6000)) {
                stop()
                frame.cross()
            }
        }
        onTriggered: check()
    }
    function cross() {
        at = target
        next.begin(span)
        if (paused)
            next.setPaused(true)
        fade.duration = waiting.byHand ? 450 : fadeMs
        fade.target = current
        fade.restart()
        if (!paused) {
            advance.interval = plugin.seconds * 1000
            advance.restart()
        }
    }
    NumberAnimation {
        id: fade
        property: "opacity"
        from: 1
        to: 0
        easing.type: Easing.InOutSine
        onFinished: frame.swap()
    }
    // They change places: the one shown is beneath now, unseen, and loads
    // the photo after.
    function swap() {
        const gone = current
        current = next
        next = gone
        current.z = 1
        gone.z = 0
        gone.load(order.length > 1 ? order[(at + 1) % order.length] : -1)
        gone.opacity = 1
        prefetch(2)
    }

    // On to the next once its time is up; a video plays to its end first,
    // for a minute and a half at most.
    Timer {
        id: advance
        property double shownAt: 0
        onRunningChanged: if (running && interval > 1000) shownAt = Date.now()
        onTriggered: {
            if (frame.current.playingVideo && Date.now() - shownAt < 90000) {
                interval = 500
                restart()
                return
            }
            frame.go(1, false)
        }
    }

    Slide {
        id: slideA
        anchors.fill: parent
        pinhole: frame.pinhole
        motion: frame.plugin.motion
        caption: frame.plugin.caption
        yearsAgo: frame.plugin.photos === "onthisday"
    }
    Slide {
        id: slideB
        anchors.fill: parent
        pinhole: frame.pinhole
        motion: frame.plugin.motion
        caption: frame.plugin.caption
        yearsAgo: frame.plugin.photos === "onthisday"
    }

    // A click stops it; a two-finger tap (the right button) holds it still,
    // or lets it go on; a swipe sideways steps. The pointer, once moved,
    // shows the buttons for a while, and hides with them.
    MouseArea {
        id: pointer
        anchors.fill: parent
        z: 2
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        cursorShape: awake.running || frame.paused ? Qt.ArrowCursor : Qt.BlankCursor
        function wake() { awake.restart() }
        property point last
        onPositionChanged: mouse => {
            if (Math.abs(mouse.x - last.x) + Math.abs(mouse.y - last.y) > 2)
                wake()
            last = Qt.point(mouse.x, mouse.y)
        }
        onClicked: mouse => {
            if (mouse.button === Qt.LeftButton)
                frame.stop()
            else if (mouse.button === Qt.RightButton)
                frame.setPaused(!frame.paused)
        }
        // Sideways, one step a swipe; up and down do nothing.
        property real swiped: 0
        property bool stepped: false
        onWheel: wheel => {
            wheel.accepted = true
            swipeRest.restart()
            if (stepped)
                return
            const dx = wheel.pixelDelta.x !== 0 ? wheel.pixelDelta.x : wheel.angleDelta.x / 2
            swiped += dx
            if (Math.abs(swiped) > 80) {
                frame.go(swiped > 0 ? -1 : 1, true)
                stepped = true
            }
        }
        Timer {
            id: swipeRest
            interval: 250
            onTriggered: { pointer.swiped = 0; pointer.stepped = false }
        }
        Timer { id: awake; interval: 2500 }
    }

    // Its buttons, while the pointer moves, and held still.
    Row {
        z: 3
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 6
        opacity: awake.running || frame.paused ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        ShellButton {
            floating: true
            text: "‹"
            action: previousCommand
            usable: frame.order.length > 1
        }
        ShellButton {
            floating: true
            text: frame.paused ? "▶" : "❚❚"
            action: pauseCommand
        }
        ShellButton {
            floating: true
            text: "›"
            action: nextCommand
            usable: frame.order.length > 1
        }
        ShellButton {
            floating: true
            text: "✕"
            action: stopCommand
        }
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        id: stopCommand
        name: "frame.stop"
        text: qsTr("Stop the photo frame")
        section: "Photo frame"
        keys: ["Escape", "P"]
        keyLayer: "plugin"
        when: frame.shown
        mouse: qsTr("Click")
        touchpad: qsTr("Tap")
        onTriggered: frame.stop()
    }
    Command {
        id: previousCommand
        name: "frame.previous"
        text: qsTr("Previous, next photo")
        section: "Photo frame"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        when: frame.shown
        mouse: qsTr("The ‹ › buttons")
        touchpad: qsTr("Swipe sideways with two fingers")
        onTriggered: frame.go(-1, true)
    }
    Command {
        id: nextCommand
        name: "frame.next"
        keys: ["Right"]
        keyLayer: "plugin"
        when: frame.shown
        mouse: qsTr("The ‹ › buttons")
        touchpad: qsTr("Swipe sideways with two fingers")
        onTriggered: frame.go(1, true)
    }
    Command {
        id: pauseCommand
        name: "frame.pause"
        text: qsTr("Hold still, go on")
        section: "Photo frame"
        keys: ["Space"]
        keyLayer: "plugin"
        when: frame.shown
        mouse: qsTr("The ❚❚ button")
        touchpad: qsTr("Tap with two fingers")
        onTriggered: frame.setPaused(!frame.paused)
    }
}
