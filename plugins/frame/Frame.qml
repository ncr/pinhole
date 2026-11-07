import QtQuick
import Pinhole.Plugin

// The frame itself, over the whole window: two slides, the one showing
// over the next, which loads beneath it unseen and starts moving as the
// one above fades away. Then they change places, and the one beneath
// loads the photo after.
Item {
    id: frame
    objectName: "photoFrame"
    // Its PluginView (Main.qml), and the plugin's root.
    property var view
    readonly property var plugin: view ? view.plugin : null
    readonly property bool shown: view ? view.shown : false
    // Asleep, the screen would put a screensaver over it, then lock.
    // Let go by Pinhole too when the plugin is turned off or loaded again.
    // Closed (Esc, P, a click, ✕), it hands its photo back.
    onShownChanged: {
        pinhole.keepAwake(shown)
        if (!shown)
            stopped()
    }

    // The photos chosen, by id, oldest first; how to choose them again when
    // the library is read again (the ids of those still there); and the
    // order they are shown in, by id, a round of them after another, with
    // where in it the one showing is.
    property var chosen: []
    property var choose: null
    property var order: []
    property int at: 0
    // How many different photos it has: one, and it stays.
    readonly property int distinct: chosen.length + (order.length > 0 && chosen.indexOf(order[0]) < 0 ? 1 : 0)
    // Started on a photo full window: stopping goes back to the viewer, on
    // the last one shown.
    property bool fromViewer: false
    property bool paused: false
    // The photos ran out (the library read again without them).
    signal emptied()

    // The slide showing, and the one under it.
    property Slide current: slideA
    property Slide next: slideB

    readonly property int fadeMs: 1400
    // A slide moves through its time and its fades.
    readonly property real span: pinhole.settings.seconds + 2 * fadeMs / 1000

    // From `first` (an id, of `chosen` or not; "" for the round's own
    // first), then round and round `chosen`. `choose()` gives them again
    // as the library changes.
    function play(first, list, fromViewer, again) {
        if (list.length === 0 && first === "")
            return
        emptyLater.stop()
        frame.fromViewer = fromViewer
        chosen = list
        choose = again
        order = first !== "" ? [first].concat(round(first, true)) : round("", false)
        at = 0
        paused = false
        fade.stop()
        waiting.stop()
        waiting.held = false
        target = -1
        changedLater = false
        current.z = 1
        current.opacity = 1
        next.z = 0
        next.opacity = 1
        current.load(idAt(0))
        current.begin(span)
        next.load(distinct > 1 ? idAt(1) : "")
        prefetch(2)
        advance.begin(pinhole.settings.seconds * 1000)
        view.open()
        pointer.wake()
        // A video open in the viewer would go on beneath, heard.
        if (fromViewer)
            pinhole.viewer.close()
    }
    // A round of `chosen` to follow `after` (the last in the order, or ""):
    // shuffled anew each time, not beginning with the one just shown, and
    // without it at all when it was shown before its round (`fresh`: the
    // first, the one showing when the library changed); in order, on in
    // time from it and round to the start.
    function round(after, fresh) {
        const list = chosen
        if (list.length === 0)
            return []
        if (pinhole.settings.shuffle) {
            const rest = fresh ? list.filter(id => id !== after) : list.slice()
            for (let k = rest.length - 1; k > 0; --k) {
                const j = Math.floor(Math.random() * (k + 1))
                const t = rest[k]
                rest[k] = rest[j]
                rest[j] = t
            }
            if (rest.length > 1 && rest[0] === after) {
                const j = 1 + Math.floor(Math.random() * (rest.length - 1))
                rest[0] = rest[j]
                rest[j] = after
            }
            return rest
        }
        let k = after === "" ? 0 : list.indexOf(after) + 1
        if (after !== "" && k === 0) {
            // Not one of them: the first taken after it.
            const library = pinhole.library
            let where = library.indexOf(after)
            if (where < 0 && after === current.photoId)
                where = current.index
            let lo = 0, hi = list.length
            while (lo < hi) {
                const mid = (lo + hi) >> 1
                if (library.indexOf(list[mid]) <= where)
                    lo = mid + 1
                else
                    hi = mid
            }
            k = lo % list.length
        }
        return list.slice(k).concat(list.slice(0, k))
    }
    // The photo `k` on in the order, rounds added as it gets there; "" if
    // there is none.
    function idAt(k) {
        while (order.length <= k && chosen.length > 0)
            order = order.concat(round(order.length > 0 ? order[order.length - 1] : "", false))
        return k >= 0 && k < order.length ? order[k] : ""
    }

    function stop() {
        view.close()
    }
    function stopped() {
        changedLater = false
        advance.stop()
        // A wait held still is over with it: shown again, it begins anew.
        waiting.stop()
        waiting.held = false
        target = -1
        if (fade.running)
            fade.pause()
        const last = current.photoId
        // The photos fade out with the frame; then they are let go.
        emptyLater.restart()
        if (last === "" || pinhole.library.indexOf(last) < 0)
            return
        if (fromViewer)
            pinhole.openPhoto(last)
        else
            pinhole.reveal(last)
    }
    Timer {
        id: emptyLater
        interval: 300
        onTriggered: {
            fade.stop()
            slideA.load("")
            slideB.load("")
            frame.order = []
            frame.chosen = []
        }
    }

    // Held still: the slides' motion, a fade under way, the wait for the
    // next and the time to it, all where they are until it goes on.
    function setPaused(on) {
        if (paused === on)
            return
        paused = on
        slideA.setPaused(on)
        slideB.setPaused(on)
        if (on) {
            if (fade.running)
                fade.pause()
            advance.hold()
            if (waiting.running && !waiting.byHand) {
                waiting.stop()
                waiting.held = true
            }
        } else {
            if (fade.paused)
                fade.resume()
            if (waiting.held) {
                waiting.held = false
                waiting.since = Date.now()
                waiting.restart()
            }
            advance.release()
        }
        pointer.wake()
    }

    // The medium of the photo after next, fetched ahead.
    function prefetch(ahead) {
        const id = distinct > ahead ? idAt(at + ahead) : ""
        if (id !== "")
            pinhole.fetch(id, "medium").then(() => {}, () => {})
    }

    // To the photo `delta` on: once the slide beneath has it sharp, or has
    // waited long enough (a step by hand waits less).
    property int target: -1
    function go(delta, byHand) {
        if (distinct < 2 && !current.gone)
            return
        // A fade under way ends at once.
        if (fade.running) {
            fade.stop()
            swap()
        }
        const to = (waiting.running || waiting.held ? target : at) + delta
        if (to < 0)
            return
        target = to
        if (next.photoId !== idAt(target))
            next.load(idAt(target))
        advance.stop()
        waiting.forward = delta >= 0
        waiting.deferred = 0
        waiting.byHand = byHand
        waiting.held = false
        waiting.since = Date.now()
        waiting.restart()
        waiting.check()
    }
    Timer {
        id: waiting
        property bool byHand: false
        property bool forward: true
        property bool held: false
        property int deferred: 0
        property double since: 0
        interval: 40
        repeat: true
        function check() {
            if (!running)
                return
            const n = frame.next
            if (frame.skip(n)) {
                since = Date.now()
                // None left to show: the one showing stays.
                if (frame.idAt(frame.target) === "") {
                    stop()
                    return
                }
                n.load(frame.idAt(frame.target))
                return
            }
            if (n.ready || Date.now() - since > (byHand ? 700 : 6000)) {
                stop()
                frame.cross()
            }
        }
        onTriggered: check()
    }
    // Whether the slide beneath is passed over: one not in the library
    // leaves the order; one whose medium the service could not bring now
    // (offline, iCloud failing) comes a dozen photos later, while those
    // already here go first, a few in a row at most, and then it shows as
    // its thumbnail until the medium comes. With no service at all nothing
    // would come sooner: each shows so in its turn.
    function skip(slide) {
        if (!slide.settled || slide.photoId !== order[target])
            return false
        if (slide.missing) {
            const gone = slide.photoId
            chosen = chosen.filter(id => id !== gone)
            order = order.filter((id, k) => k !== target)
            if (!waiting.forward && target > 0)
                --target
            return true
        }
        if (slide.failure !== "" && slide.failure !== "no_service" && waiting.forward && waiting.deferred < 8
                && distinct > 2) {
            ++waiting.deferred
            const later = target + 12
            idAt(later)
            const put = order.slice()
            put.splice(later + 1, 0, put[target])
            put.splice(target, 1)
            order = put
            return true
        }
        return false
    }
    function cross() {
        at = target
        next.begin(span)
        if (paused)
            next.setPaused(true)
        fade.duration = waiting.byHand ? 450 : fadeMs
        fade.target = current
        fade.restart()
        advance.begin(pinhole.settings.seconds * 1000)
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
        // What is far behind is let go.
        const keep = 50
        if (at > Math.max(2 * keep, chosen.length + keep)) {
            order = order.slice(at - keep)
            at = keep
        }
        gone.load(distinct > 1 ? idAt(at + 1) : "")
        gone.opacity = 1
        prefetch(2)
        if (changedLater)
            libraryChanged()
    }

    // On to the next once its time is up; a video comes and plays to its
    // end first, for a minute and a half at most. Held still, what is left
    // of its time is kept.
    Timer {
        id: advance
        property double startedAt: 0
        property double shownAt: 0
        property int left: -1
        function begin(ms) {
            shownAt = Date.now()
            left = -1
            if (frame.paused) {
                left = ms
                return
            }
            interval = ms
            startedAt = Date.now()
            start()
        }
        function hold() {
            if (!running)
                return
            left = Math.max(500, interval - (Date.now() - startedAt))
            stop()
        }
        function release() {
            if (left < 0)
                return
            interval = left
            left = -1
            startedAt = Date.now()
            start()
        }
        onTriggered: {
            if (frame.current.playingVideo && Date.now() - shownAt < 90000) {
                interval = 500
                startedAt = Date.now()
                start()
                return
            }
            frame.go(1, false)
        }
    }

    // The library read again, or photos hidden or shown again: the photos
    // chosen again, by id, so each slide keeps its photo and the one
    // showing stays. Gone (deleted,
    // another source shown), it goes on to the next. Not in the middle of
    // a fade: after it.
    property bool changedLater: false
    function libraryChanged() {
        if (!shown)
            return
        if (fade.running) {
            changedLater = true
            return
        }
        changedLater = false
        slideA.relocate()
        slideB.relocate()
        const list = choose ? choose() : []
        if (list.length === 0) {
            stop()
            emptied()
            return
        }
        // The same photos chosen (a sync that brought others): the order
        // stays. Shuffled, those gone leave it and a few new ones come in
        // anywhere in what is left of it; else a new round from the one
        // showing.
        const was = chosen
        chosen = list
        if (list.length !== was.length || list.some((id, i) => id !== was[i])) {
            const before = new Set(was), now = new Set(list)
            const added = list.filter(id => !before.has(id))
            if (pinhole.settings.shuffle && added.length <= 50) {
                const rest = order.slice(at + 1).filter(id => now.has(id))
                for (const id of added)
                    rest.splice(Math.floor(Math.random() * (rest.length + 1)), 0, id)
                order = order.slice(0, at + 1).concat(rest)
            } else {
                order = order.slice(0, at + 1).concat(round(current.photoId, true))
            }
        }
        // A step under way keeps its way: on to the next, or back where
        // it was going (what is behind stays as it was).
        const waited = waiting.running || waiting.held
        if (waited && waiting.forward)
            target = at + 1
        const coming = waited ? target : at + 1
        if (next.photoId !== idAt(coming) || next.gone)
            next.load(distinct > 1 || current.gone ? idAt(coming) : "")
        prefetch(2)
        if (current.gone && !waited)
            go(1, false)
    }
    Connections {
        target: pinhole.library
        function onChanged() { frame.libraryChanged() }
    }
    // Photos hidden from the grid, or shown again: chosen again too.
    Connections {
        target: pinhole
        function onHiddenChanged() { frame.libraryChanged() }
    }

    // Nothing reaches the photos beneath; a swipe sideways, or the sideways
    // wheel, steps.
    Panel {
        color: "black"
        steps: Qt.Horizontal
        onStepped: step => frame.go(step, true)
    }
    Slide {
        id: slideA
        anchors.fill: parent
        motion: pinhole.settings.motion
        caption: pinhole.settings.caption
        yearsAgo: pinhole.settings.photos === "onthisday"
    }
    Slide {
        id: slideB
        anchors.fill: parent
        motion: pinhole.settings.motion
        caption: pinhole.settings.caption
        yearsAgo: pinhole.settings.photos === "onthisday"
    }

    // A click stops it; a two-finger tap (the right button) holds it still,
    // or lets it go on (a swipe sideways steps: the Panel beneath). The
    // pointer, once moved, shows the buttons for a while, and hides with
    // them.
    MouseArea {
        id: pointer
        anchors.fill: parent
        z: 2
        enabled: frame.shown
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
            usable: frame.distinct > 1 && frame.at > 0
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
            usable: frame.distinct > 1
        }
        ShellButton {
            floating: true
            text: "✕"
            action: frame.view.closeCommand
        }
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        id: previousCommand
        name: "frame.previous"
        text: qsTr("Previous, next photo")
        section: "Photo frame"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        usable: frame.shown
        mouse: qsTr("The ‹ › buttons")
        touchpad: qsTr("Swipe sideways with two fingers")
        onTriggered: frame.go(-1, true)
    }
    Command {
        id: nextCommand
        name: "frame.next"
        keys: ["Right"]
        keyLayer: "plugin"
        usable: frame.shown
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
        usable: frame.shown
        mouse: qsTr("The ❚❚ button")
        touchpad: qsTr("Tap with two fingers")
        onTriggered: frame.setPaused(!frame.paused)
    }
}
