import QtQuick
import QtQuick.Effects
import QtQuick.Window
import QtMultimedia
import Pinhole.Plugin

// One photo of the frame, full window: its thumbnail at once, the medium
// rendition over it once here, then the full size where the screen has the
// pixels for it (in the medium's place for a photo without one), decoded at
// the size it is shown; and for a video or a Live Photo its motion, fetched
// once it is shown.
// Loaded while under the one showing, so it is sharp by the time it comes;
// `begin()` starts its slow pan and zoom.
Item {
    id: slide
    // Videos and Live Photos play their motion.
    property bool motion: true

    // The photo shown, by its id ("" for none), where the library has it
    // (or last had it), and whether it has gone from it since.
    property string photoId: ""
    property int index: -1
    property bool gone: false
    property var item: null
    property string sharp: ""
    property string full: ""
    // The size the full one is decoded at, set as it comes: the slide
    // going on to another photo does not decode it again at that one's.
    property size fullSize
    property bool fullAsked: false
    // The medium came, or will not: showing it now is as sharp as it gets.
    // A photo with no medium (one in eight of a library, some of them
    // favourites) has its full size in its place.
    property bool settled: false
    // Not in the library (not_found): shown, it would be its thumbnail
    // for good.
    property bool missing: false
    // Why the medium did not come for now ("offline", "no_service", …):
    // shown as its thumbnail, and asked for again.
    property string failure: ""
    readonly property bool ready: settled && (sharp === "" || sharpImage.status !== Image.Loading)
                                  && (sharp !== "" || full === "" || fullImage.status !== Image.Loading)
    // A video's or Live Photo's file, once fetched; fetched once shown.
    property string motionFile: ""
    property bool motionAsked: false
    property bool motionComing: false
    // Shown, its motion going.
    property bool running: false
    property bool paused: false
    property bool played: false
    // A video coming or playing to its end, which the frame waits for.
    readonly property bool playingVideo: item !== null && item.video && motion
                                         && (motionComing || motionFile !== ""
                                             && player.playbackState === MediaPlayer.PlayingState)

    readonly property real aspect: item && item.width > 0 && item.height > 0 ? item.width / item.height : 4 / 3
    // Filling the window when its shape is near the window's; a tall photo
    // on a wide window shows whole, on a blur of itself.
    readonly property bool fitted: Math.max(aspect / windowAspect, windowAspect / aspect) > 1.3
    readonly property real windowAspect: width > 0 && height > 0 ? width / height : 16 / 10
    // How far in it goes: a little only for one shown whole, whose top
    // and bottom are the window's.
    readonly property real zoomMax: fitted ? 1.05 : 1.2

    // Its files on their way, and which load they are for: an answer for
    // one before is dropped (a cancel can come after it was sent).
    property var fetching: []
    property int serial: 0
    function load(id) {
        stop()
        fetching.forEach(f => f.cancel())
        fetching = []
        const loading = ++serial
        // Its images let go first: the photo changing changes the size
        // they are shown at, and they would be decoded again at it.
        sharp = ""
        full = ""
        fullAsked = false
        photoId = id
        index = id !== "" ? pinhole.library.indexOf(id) : -1
        item = index >= 0 ? pinhole.library.item(index) : null
        motionFile = ""
        motionAsked = false
        motionComing = false
        played = false
        failure = ""
        gone = false
        retry.stop()
        settled = index < 0
        missing = id !== "" && index < 0
        if (index < 0)
            return
        fetchMedium(loading)
    }
    function fetchMedium(loading) {
        const medium = pinhole.fetch(photoId, "medium")
        medium.then(path => {
            if (slide.serial !== loading)
                return
            // In sRGB, whatever its profile (docs/plugins.md).
            slide.sharp = "image://file/" + encodeURIComponent(path)
            slide.failure = ""
            slide.settled = true
        }, error => {
            if (slide.serial !== loading || error.code === "cancelled")
                return
            // No medium, the photo still in the library: its full size
            // instead (a video stays its thumbnail, and plays).
            if (error.code === "not_found" && pinhole.library.indexOf(slide.photoId) >= 0) {
                if (slide.item && slide.item.video)
                    slide.settled = true
                else
                    slide.fetchFull(loading, true)
                return
            }
            slide.failed(error)
        }).catch(pinhole.warn)
        fetching.push(medium)
    }
    // Not in the library: passed over. Anything else (no service, offline,
    // the service starting again) is for now: its thumbnail, and asked for
    // again in a while.
    function failed(error) {
        if (error.code === "not_found")
            missing = true
        else {
            failure = error.code || "failed"
            retry.restart()
        }
        settled = true
    }
    Timer {
        id: retry
        interval: 8000
        onTriggered: if (slide.failure !== "" && slide.photoId !== "") slide.fetchMedium(slide.serial)
    }
    // The full size, where the medium would show clearly soft: smaller
    // than three quarters of the size the slide shows it at (a 2048 px
    // medium on a 2× screen, not on a 1× one). Decoded at the size it is
    // shown at its closest, by Pinhole's own decoder, as it is mostly HEIC.
    // Not for a video, whose frames are the medium's size. In points: Qt
    // asks a sourceSize times the screen's ratio.
    readonly property size shownSize: Qt.size(Math.ceil(photo.width * zoomMax), Math.ceil(photo.height * zoomMax))
    readonly property bool wantsFull: sharpImage.status === Image.Ready && item !== null && !item.video
                                      && Math.max(sharpImage.implicitWidth, sharpImage.implicitHeight)
                                         < 0.75 * Math.max(photo.width, photo.height) * Screen.devicePixelRatio
    onWantsFullChanged: if (wantsFull) fetchFull(serial, false)
    // `instead`: of the medium, which it does not have; it is ready once
    // this is decoded, or failed as the medium would have.
    function fetchFull(loading, instead) {
        if (fullAsked)
            return
        fullAsked = true
        const big = pinhole.fetch(photoId, "full")
        big.then(path => {
            if (slide.serial !== loading)
                return
            slide.fullSize = slide.shownSize
            slide.full = "image://photo/" + encodeURIComponent(path)
            if (instead) {
                slide.failure = ""
                slide.settled = true
            }
        }, error => {
            if (slide.serial !== loading || error.code === "cancelled")
                return
            slide.fullAsked = false
            if (instead)
                slide.failed(error)
        }).catch(pinhole.warn)
        fetching.push(big)
    }
    // The motion, once its slide shows: never fetched for one passed on
    // before it came. A still until it is here, if it does not come.
    function fetchMotion() {
        if (!motion || !item || !(item.video || item.live) || motionAsked)
            return
        motionAsked = true
        motionComing = true
        const loading = serial
        const clip = pinhole.fetch(photoId, item.video ? "video" : "live")
        clip.then(path => {
            if (slide.serial !== loading)
                return
            slide.motionComing = false
            slide.motionFile = "file://" + path
            if (slide.running)
                slide.play()
        }, () => {
            if (slide.serial === loading)
                slide.motionComing = false
        }).catch(pinhole.warn)
        fetching.push(clip)
    }

    // A pan and zoom, from one framing to another: first through its time
    // and its fades, steadily; held past that (a video playing on, the
    // next not here yet), easing out of it and then drifting slowly on,
    // never stopping dead.
    function begin(seconds) {
        running = true
        paused = false
        const s0 = 1.0 + Math.random() * (fitted ? 0.01 : 0.04)
        const s1 = fitted ? 1.03 + Math.random() * 0.02 : 1.08 + Math.random() * 0.07
        const zoomIn = Math.random() < 0.6
        const fromZoom = zoomIn ? s0 : s1, toZoom = zoomIn ? s1 : s0
        const at = framing(fromZoom, Math.random() * 1.6 - 0.8, Math.random() * 1.6 - 0.8)
        zoom.value = fromZoom
        panX.value = at.x
        panY.value = at.y
        const to = framing(toZoom, Math.random() * 1.6 - 0.8, Math.random() * 1.6 - 0.8)
        legs = 0
        leg(to.zoom, to.x, to.y, Math.round(seconds * 1000), Easing.Linear)
        if (motionFile !== "")
            play()
        else
            fetchMotion()
    }
    // How far it may go each way at a scale, the window always covered
    // (or, shown whole, a little way sideways only).
    function room(s, size, window) { return Math.max(0, (size * s - window) / 2) }
    // A framing: at zoom `s`, `fx` and `fy` from -1 to 1 of the way to
    // each side.
    function framing(s, fx, fy) {
        return fitted ? { zoom: s, x: fx * slide.width * 0.015, y: 0 }
                      : { zoom: s, x: fx * room(s, photo.width, slide.width), y: fy * room(s, photo.height, slide.height) }
    }
    function leg(toZoom, toX, toY, ms, easing) {
        drift.stop()
        zoomAnimation.from = zoom.value
        zoomAnimation.to = toZoom
        panXAnimation.from = panX.value
        panXAnimation.to = toX
        panYAnimation.from = panY.value
        panYAnimation.to = toY
        for (const a of [zoomAnimation, panXAnimation, panYAnimation]) {
            a.duration = ms
            a.easing.type = easing
        }
        drift.start()
        if (paused)
            drift.pause()
    }
    property int legs: 0
    function onward() {
        if (!running)
            return
        if (legs++ === 0) {
            // Easing out: on as it was going, slowing to a stop over 12 s,
            // as far as the edges let it. The same easing and time for
            // all three keep the window covered on the way.
            const ms = 12000
            const per = a => (a.to - a.from) / Math.max(1, a.duration) * ms / 2
            const s = Math.max(1, Math.min(zoomMax, zoom.value + per(zoomAnimation)))
            const small = Math.min(s, zoom.value)
            const clamp = (v, r) => Math.max(-r, Math.min(r, v))
            const rx = fitted ? slide.width * 0.015 : room(small, photo.width, slide.width)
            const ry = fitted ? 0 : room(small, photo.height, slide.height)
            leg(s, clamp(panX.value + per(panXAnimation), rx), clamp(panY.value + per(panYAnimation), ry), ms, Easing.OutQuad)
            return
        }
        // Then, held on, slowly elsewhere and back, a little at a time.
        const s = fitted ? 1.0 + Math.random() * 0.04 : 1.03 + Math.random() * 0.12
        const to = framing(Math.min(s, zoomMax), Math.random() * 1.2 - 0.6, Math.random() * 1.2 - 0.6)
        leg(to.zoom, to.x, to.y, 20000, Easing.InOutSine)
    }
    // The window resized (tiled beside another, made full screen): what it
    // was moving to may no longer cover it, or cut a photo now shown
    // whole. From where it is to a framing that fits, then drifting on.
    onWidthChanged: if (running) resized.restart()
    onHeightChanged: if (running) resized.restart()
    Timer {
        id: resized
        interval: 150
        onTriggered: {
            if (!slide.running)
                return
            const to = slide.framing(Math.max(1, Math.min(slide.zoomMax, zoom.value)), Math.random() * 1.2 - 0.6,
                                     Math.random() * 1.2 - 0.6)
            slide.legs = 1
            slide.leg(to.zoom, to.x, to.y, 1500, Easing.InOutSine)
        }
    }
    function stop() {
        running = false
        resized.stop()
        paused = false
        drift.stop()
        player.stop()
        clipOutput.visible = false
    }
    function setPaused(on) {
        paused = on
        if (!running)
            return
        if (on) {
            drift.pause()
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause()
        } else {
            drift.resume()
            if (player.playbackState === MediaPlayer.PausedState)
                player.play()
            else if (!played && motionFile !== "")
                play()
        }
    }
    function play() {
        if (!motion || motionFile === "" || paused)
            return
        played = true
        player.source = motionFile
        player.play()
    }
    // The library read again: where the photo is now, if anywhere.
    function relocate() {
        if (photoId === "")
            return
        const now = pinhole.library.indexOf(photoId)
        gone = now < 0
        if (gone)
            return
        index = now
        item = pinhole.library.item(now)
    }

    clip: true
    Rectangle {
        anchors.fill: parent
        color: "black"
    }
    // Behind a fitted photo: itself, blurred and darkened, so the frame is
    // filled in its colours. Blurred small and drawn large, which blurs it
    // more and costs next to nothing.
    Item {
        anchors.centerIn: parent
        width: 160
        height: Math.round(width / slide.windowAspect)
        scale: slide.width / width * 1.06
        visible: slide.fitted && slide.item !== null
        layer.enabled: visible
        layer.smooth: true
        layer.effect: MultiEffect {
            autoPaddingEnabled: false
            blurEnabled: true
            blur: 1
            blurMax: 16
            brightness: -0.35
            saturation: 0.1
        }
        Image {
            anchors.fill: parent
            source: parent.visible ? slide.item.thumbnail : ""
            sourceSize.width: 160
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            smooth: true
        }
    }

    Item {
        id: photo
        anchors.centerIn: parent
        // Covering the window, or whole within it.
        width: slide.fitted !== (slide.aspect > slide.windowAspect) ? slide.height * slide.aspect : slide.width
        height: width / slide.aspect
        transform: [
            Scale {
                origin.x: photo.width / 2
                origin.y: photo.height / 2
                xScale: zoom.value
                yScale: zoom.value
            },
            Translate { x: panX.value; y: panY.value }
        ]
        Image {
            anchors.fill: parent
            source: slide.item ? slide.item.thumbnail : ""
            asynchronous: true
            smooth: true
            fillMode: Image.PreserveAspectCrop
        }
        // Sharper ones, each fading in over the one before once decoded
        // (at once while the slide is beneath, unseen). Sharp frames come
        // and go all day: none kept in the cache.
        Image {
            id: sharpImage
            anchors.fill: parent
            // Let go under the full size once that has faded in all the
            // way: the photo sharpens from the medium, not from the
            // thumbnail.
            objectName: "sharpImage"
            source: fullImage.status === Image.Ready && fullImage.opacity >= 1 ? "" : slide.sharp
            asynchronous: true
            smooth: true
            cache: false
            fillMode: Image.PreserveAspectCrop
            opacity: status === Image.Ready ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { enabled: slide.running; NumberAnimation { duration: 500 } }
        }
        Image {
            id: fullImage
            objectName: "fullImage"
            anchors.fill: parent
            source: slide.full
            sourceSize: slide.fullSize
            asynchronous: true
            smooth: true
            cache: false
            fillMode: Image.PreserveAspectCrop
            opacity: status === Image.Ready ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { enabled: slide.running; NumberAnimation { duration: 500 } }
        }
        // An HDR clip (an iPhone's HLG) tone mapped as it is drawn, as the
        // viewer's is.
        VideoView {
            id: clipOutput
            anchors.fill: parent
            fillMode: VideoView.PreserveAspectCrop
            visible: false
        }
    }
    // When it was taken, small in the corner; on this day, how long ago.
    property bool caption: true
    property bool yearsAgo: false
    Rectangle {
        visible: slide.caption && slide.item !== null
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: Math.round(theme.fontSize * 1.6)
        width: captionText.implicitWidth + 2 * Math.round(theme.fontSize * 0.7)
        height: captionText.implicitHeight + Math.round(theme.fontSize * 0.6)
        radius: Math.min(6, theme.radius)
        color: Qt.rgba(0, 0, 0, 0.38)
        UiText {
            id: captionText
            anchors.centerIn: parent
            color: "white"
            text: {
                if (!slide.item)
                    return ""
                const date = new Date(slide.item.date)
                const years = new Date().getFullYear() - date.getFullYear()
                return date.toLocaleDateString(Qt.locale(), Locale.LongFormat)
                    + (slide.yearsAgo && years > 0 ? "  ·  " + qsTr("%n years ago", "", years) : "")
            }
        }
    }

    // Where its pan and zoom are, and its sharpest image up, for the
    // self-test (PINHOLE_SELFTEST=frame).
    readonly property vector3d framed: Qt.vector3d(zoom.value, panX.value, panY.value)
    readonly property string sharpest: fullImage.status === Image.Ready ? "full"
                                       : sharpImage.status === Image.Ready ? "medium" : "thumbnail"
    // Its own values, which the animations move and the transform follows.
    QtObject { id: zoom; property real value: 1 }
    QtObject { id: panX; property real value: 0 }
    QtObject { id: panY; property real value: 0 }
    ParallelAnimation {
        id: drift
        NumberAnimation { id: zoomAnimation; target: zoom; property: "value" }
        NumberAnimation { id: panXAnimation; target: panX; property: "value" }
        NumberAnimation { id: panYAnimation; target: panY; property: "value" }
        onFinished: slide.onward()
    }

    // No sound: a frame plays quietly.
    MediaPlayer {
        id: player
        videoOutput: clipOutput
        onPlaybackStateChanged: {
            if (playbackState === MediaPlayer.PlayingState)
                clipOutput.visible = true
        }
        onMediaStatusChanged: {
            if (mediaStatus !== MediaPlayer.EndOfMedia)
                return
            // A Live Photo goes back to its still; a video holds its last
            // frame as the next comes.
            if (slide.item && slide.item.live && !slide.item.video)
                clipOutput.visible = false
        }
        onErrorOccurred: clipOutput.visible = false
    }
}
