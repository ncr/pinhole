import QtQuick
import QtQuick.Effects
import QtMultimedia
import Pinhole.Plugin

// One photo of the frame, full window: its thumbnail at once, the medium
// rendition over it once here, and for a video or a Live Photo its motion,
// once it is shown. Loaded while under the one showing, so it is sharp by
// the time it comes; `begin()` starts its slow pan and zoom.
Item {
    id: slide
    required property var pinhole
    // Videos and Live Photos play their motion.
    property bool motion: true

    // The library position shown, -1 for none.
    property int index: -1
    property var item: null
    property string sharp: ""
    // The medium came, or will not: showing it now is as sharp as it gets.
    property bool settled: false
    // The service has no medium for it: shown, it would be blurry.
    property bool missing: false
    readonly property bool ready: settled && (sharp === "" || sharpImage.status !== Image.Loading)
    // A video's or Live Photo's file, once fetched.
    property string motionFile: ""
    // Shown, its motion going.
    property bool running: false
    property bool paused: false
    // A video playing to its end, which the frame waits for.
    readonly property bool playingVideo: item !== null && item.video && motion && motionFile !== ""
                                         && player.playbackState === MediaPlayer.PlayingState
    signal videoEnded()

    readonly property real aspect: item && item.width > 0 && item.height > 0 ? item.width / item.height : 4 / 3
    // Filling the window when its shape is near the window's; a tall photo
    // on a wide window shows whole, on a blur of itself.
    readonly property bool fitted: Math.max(aspect / windowAspect, windowAspect / aspect) > 1.3
    readonly property real windowAspect: width > 0 && height > 0 ? width / height : 16 / 10

    // Its files on their way: another photo loaded, they are no longer
    // wanted.
    property var fetching: []
    function load(i) {
        stop()
        fetching.forEach(f => f.cancel())
        fetching = []
        index = i
        item = i >= 0 ? pinhole.library.item(i) : null
        sharp = ""
        motionFile = ""
        settled = i < 0
        missing = false
        if (i < 0)
            return
        const medium = pinhole.fetch(i, "medium")
        medium.then(path => {
            slide.sharp = "file://" + path
            slide.settled = true
        }, error => {
            if (error.code === "cancelled")
                return
            slide.missing = true
            slide.settled = true
        }).catch(pinhole.warn)
        fetching = [medium]
        // The motion, fetched now so it can play as the slide comes; a
        // still if it does not.
        if (motion && (item.video || item.live)) {
            const clip = pinhole.fetch(i, item.video ? "video" : "live")
            clip.then(path => {
                slide.motionFile = "file://" + path
                if (slide.running)
                    slide.play()
            }, () => {}).catch(pinhole.warn)
            fetching = [medium, clip]
        }
    }

    // The pan and zoom: from one framing to another, over `seconds`.
    function begin(seconds) {
        running = true
        paused = false
        const s0 = 1.0 + Math.random() * 0.04, s1 = 1.08 + Math.random() * 0.07
        const zoomIn = Math.random() < 0.6
        const from = zoomIn ? s0 : s1, to = zoomIn ? s1 : s0
        // How far it may go each way at a scale, the window always covered
        // (or, fitted, a little way only).
        const room = (s, size, window) => fitted ? window * 0.015 : Math.max(0, (size * s - window) / 2)
        const fx0 = Math.random() * 1.6 - 0.8, fy0 = Math.random() * 1.6 - 0.8
        const fx1 = Math.random() * 1.6 - 0.8, fy1 = Math.random() * 1.6 - 0.8
        zoom.from = from
        zoom.to = to
        panX.from = fx0 * room(from, photo.width, slide.width)
        panX.to = fx1 * room(to, photo.width, slide.width)
        panY.from = fy0 * room(from, photo.height, slide.height)
        panY.to = fy1 * room(to, photo.height, slide.height)
        const ms = Math.round(seconds * 1000)
        zoom.duration = ms
        panX.duration = ms
        panY.duration = ms
        drift.restart()
        if (motionFile !== "")
            play()
    }
    function stop() {
        running = false
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
        }
    }
    function play() {
        if (!motion || motionFile === "" || paused)
            return
        player.source = motionFile
        player.play()
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
                id: scale
                origin.x: photo.width / 2
                origin.y: photo.height / 2
                xScale: zoom.value
                yScale: zoom.value
            },
            Translate { id: shift; x: panX.value; y: panY.value }
        ]
        Image {
            anchors.fill: parent
            source: slide.item ? slide.item.thumbnail : ""
            asynchronous: true
            smooth: true
            fillMode: Image.PreserveAspectCrop
        }
        Image {
            id: sharpImage
            anchors.fill: parent
            source: slide.sharp
            asynchronous: true
            smooth: true
            // Sharp frames come and go all day: none kept in the cache.
            cache: false
            fillMode: Image.PreserveAspectCrop
            visible: status === Image.Ready
        }
        VideoOutput {
            id: clipOutput
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectCrop
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

    // Its own values, which the animations move and the transform follows.
    QtObject { id: zoom; property real value: 1; property real from: 1; property real to: 1; property int duration: 8000 }
    QtObject { id: panX; property real value: 0; property real from: 0; property real to: 0; property int duration: 8000 }
    QtObject { id: panY; property real value: 0; property real from: 0; property real to: 0; property int duration: 8000 }
    ParallelAnimation {
        id: drift
        NumberAnimation { target: zoom; property: "value"; from: zoom.from; to: zoom.to; duration: zoom.duration }
        NumberAnimation { target: panX; property: "value"; from: panX.from; to: panX.to; duration: panX.duration }
        NumberAnimation { target: panY; property: "value"; from: panY.from; to: panY.to; duration: panY.duration }
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
            slide.videoEnded()
        }
        onErrorOccurred: {
            clipOutput.visible = false
            slide.videoEnded()
        }
    }
}
