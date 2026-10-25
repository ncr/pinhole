import QtQuick
import QtQuick.Window
import QtMultimedia

// The full-window view of one photo or video, as in Photos. It grows out of
// the photo's cell in the grid and shrinks back into it; the square crop of
// the cell relaxes into the whole photo as it grows. The photos lie side by
// side and follow the fingers; a pinch zooms one; the carousel along the
// bottom shows where in the library it is. The motion itself is the
// Viewer's (viewermotion.h); this draws it.
Item {
    id: root

    required property var grid
    property double openedAt: 0
    readonly property bool shown: viewer.open || progress > 0

    visible: shown
    // 0: the photo's cell in the grid. 1: fitted to the window.
    property real progress: 0
    // Where it grows from and shrinks back to.
    property rect cell: Qt.rect(0, 0, 0, 0)

    // The carousel's band along the bottom; photos fit the area above it.
    readonly property real stripHeight: 60
    readonly property real areaHeight: Math.max(1, height - stripHeight)
    readonly property real fitWidth: Math.min(width, areaHeight * viewer.aspect)
    readonly property real fitHeight: fitWidth / Math.max(0.01, viewer.aspect)
    onWidthChanged: viewer.setViewport(width, areaHeight)
    onAreaHeightChanged: viewer.setViewport(width, areaHeight)
    Component.onCompleted: viewer.setViewport(width, areaHeight)

    FrameAnimation {
        running: viewer.open && viewer.animating
        onTriggered: viewer.advance(frameTime)
    }

    function lerp(a, b, t) { return a + (b - a) * t }

    function closeViewer() {
        root.grid.reveal(viewer.index)
        root.cell = root.grid.rectOf(viewer.index)
        videoPlayer.stop()
        livePlayer.stop()
        closing.start()
    }

    Connections {
        target: viewer
        function onOpenChanged() {
            if (viewer.open) {
                root.openedAt = Date.now()
                root.cell = viewer.origin
                closing.stop()
                opening.start()
            }
        }
        function onIndexChanged() {
            originalImage.source = ""
            detailImage.source = ""
            liveOutput.visible = false
            livePlayer.stop()
        }
        function onImageChanged() {
            if (viewer.stage === "original")
                originalImage.source = viewer.image
        }
        function onCloseRequested() { root.closeViewer() }
        function onLiveChanged() {
            if (viewer.liveSource.toString() !== "") {
                livePlayer.source = viewer.liveSource
                liveOutput.visible = true
                livePlayer.position = 0
                livePlayer.play()
            }
        }
    }

    NumberAnimation {
        id: opening
        target: root; property: "progress"; to: 1; duration: 260; easing.type: Easing.OutCubic
    }
    NumberAnimation {
        id: closing
        target: root; property: "progress"; to: 0; duration: 220; easing.type: Easing.OutCubic
        onFinished: viewer.close()
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: root.progress
    }

    // The photos beside the shown one, two each side, where the row puts
    // them: each its best image at hand (medium once fetched). During a far
    // jump, only the photo it came from, beside it (Viewer.beside()).
    Repeater {
        model: [-2, -1, 1, 2]
        delegate: Item {
            required property int modelData
            readonly property int at: (viewer.index, viewer.jumpFrom, viewer.beside(modelData))
            readonly property real aspect: viewer.aspectAt(at)
            readonly property real centre: root.width / 2 + modelData * viewer.pageWidth + viewer.offset
            width: Math.min(root.width, root.areaHeight * aspect)
            height: width / Math.max(0.01, aspect)
            x: centre - width / 2
            y: (root.areaHeight - height) / 2
            visible: root.progress === 1 && at >= 0 && at < viewer.count && x < root.width && x + width > 0
            Image {
                anchors.fill: parent
                source: parent.visible ? viewer.thumbnailAt(parent.at) : ""
                fillMode: Image.PreserveAspectCrop
            }
            Image {
                anchors.fill: parent
                source: parent.visible ? (viewer.revision, viewer.imageFor(parent.at)) : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
            }
        }
    }

    Item {
        id: frame
        // Fitted, zoomed and panned, where the row puts it; growing from
        // the grid's cell while opening.
        readonly property real targetWidth: root.fitWidth * viewer.zoom
        readonly property real targetHeight: root.fitHeight * viewer.zoom
        x: root.lerp(root.cell.x, root.width / 2 + viewer.offset + viewer.panX - targetWidth / 2, root.progress)
        y: root.lerp(root.cell.y, root.areaHeight / 2 + viewer.panY - targetHeight / 2, root.progress)
        width: root.lerp(root.cell.width, targetWidth, root.progress)
        height: root.lerp(root.cell.height, targetHeight, root.progress)
        clip: true

        // Sharpest last: each one covers the one below once it has loaded.
        Image {
            anchors.fill: parent
            source: viewer.thumbnail
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            smooth: true
        }
        // The medium rendition: the same image the photo had beside this
        // one, so stepping onto it shows it sharp at once.
        Image {
            id: mediumImage
            anchors.fill: parent
            source: (viewer.revision, viewer.imageFor(viewer.index))
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
        }
        Image {
            id: originalImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
        // Zoomed in: the original at its full size, decoded once it is
        // wanted, so detail holds up close.
        Image {
            id: detailImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            mipmap: true
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
            readonly property bool wanted: viewer.zoom > 1.3 && originalImage.status === Image.Ready
            onWantedChanged: if (wanted && source.toString() === "") source = originalImage.source
        }

        MediaPlayer {
            id: videoPlayer
            source: viewer.video ? viewer.videoSource : ""
            videoOutput: videoOutput
            audioOutput: AudioOutput { muted: root.muted }
            onSourceChanged: if (source.toString() !== "" && viewer.open) play()
        }
        VideoOutput {
            id: videoOutput
            anchors.fill: parent
            visible: viewer.video && videoPlayer.playbackState !== MediaPlayer.StoppedState
            fillMode: VideoOutput.PreserveAspectCrop
        }

        MediaPlayer {
            id: livePlayer
            videoOutput: liveOutput
            onPlaybackStateChanged: if (playbackState === MediaPlayer.StoppedState) liveOutput.visible = false
        }
        VideoOutput {
            id: liveOutput
            anchors.fill: parent
            visible: false
            fillMode: VideoOutput.PreserveAspectCrop
        }
    }

    // The mouse. The sides step to the previous and next photo; elsewhere a
    // click plays or pauses a video and a double-click goes back to the
    // library, as does the mouse's back button. The wheel and sideways
    // swipes are the Viewer's own (viewer.cpp), as the window's events.
    MouseArea {
        id: pointer
        anchors.fill: parent
        enabled: viewer.open
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.BackButton
        readonly property real side: Math.min(160, width * 0.15)
        // Zoomed, the sides are part of the photo, and dragging pans it.
        function sideAt(x) { return viewer.zoomed ? 0 : x < side ? -1 : x > width - side ? 1 : 0 }
        property point last
        property bool panned: false
        onPositionChanged: mouse => {
            if (!(mouse.buttons & Qt.LeftButton) || !viewer.zoomed)
                return
            viewer.panBy(mouse.x - last.x, mouse.y - last.y)
            if (Math.abs(mouse.x - last.x) + Math.abs(mouse.y - last.y) > 0)
                panned = true
            last = Qt.point(mouse.x, mouse.y)
        }
        readonly property int hoveredSide: containsMouse ? sideAt(mouseX) : 0

        onPressed: mouse => {
            last = Qt.point(mouse.x, mouse.y)
            panned = false
            // Any press is a touch first: a coasting zoomed photo stops (a
            // two-finger tap, which arrives as the right button, does only that).
            viewer.stopCoasting()
            if (mouse.button === Qt.BackButton)
                root.closeViewer()
        }
        onClicked: mouse => {
            if (mouse.button !== Qt.LeftButton || panned)
                return
            const s = sideAt(mouse.x)
            if (s !== 0)
                viewer.step(s)
            else
                root.togglePlay()
        }
        onDoubleClicked: mouse => {
            // The second click of a double-click that opened the photo
            // (a single click opens) must not close it again.
            if (Date.now() - root.openedAt < 600)
                return
            const s = sideAt(mouse.x)
            if (s !== 0)
                viewer.step(s)
            else if (viewer.zoomed)
                viewer.resetZoom()
            else
                root.closeViewer()
        }
    }

    // The sides, while the pointer is over one.
    Repeater {
        model: [-1, 1]
        delegate: Text {
            required property int modelData
            x: modelData < 0 ? 24 : root.width - width - 24
            text: modelData < 0 ? "‹" : "›"
            color: "white"
            font.pixelSize: 56
            style: Text.Outline
            styleColor: "#80000000"
            readonly property bool can: modelData < 0 ? viewer.index > 0 : viewer.index < root.grid.count - 1
            opacity: pointer.hoveredSide === modelData && can ? 0.9 * root.progress : 0
            y: (root.areaHeight - height) / 2
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }

    // What is shown, and what is still on its way.
    Column {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 4
        opacity: root.progress
        Text {
            text: viewer.title
            color: "white"
            font.pixelSize: 15
            style: Text.Outline
            styleColor: "#80000000"
        }
        Text {
            text: viewer.message !== "" ? viewer.message
                  : viewer.video ? ""
                  : viewer.stage === "original" ? "" : qsTr("Loading the full photo…")
            visible: text !== ""
            color: "#c0ffffff"
            font.pixelSize: 12
        }
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 8
        opacity: root.progress
        Text {
            visible: viewer.favorite
            text: "♥"
            color: "white"
            font.pixelSize: 16
        }
        Text {
            visible: viewer.live
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("LIVE  (L)")
            color: "white"
            font.pixelSize: 12
            font.bold: true
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: viewer.playLive()
            }
        }
        // Out of the app, as buttons: no menu to find them in.
        Repeater {
            model: [
                { label: qsTr("Copy"), act: () => outgoing.copy([viewer.index]) },
                { label: qsTr("Export"), act: () => outgoing.exportTo([viewer.index], false) }
            ]
            delegate: Rectangle {
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                width: buttonLabel.implicitWidth + 22
                height: buttonLabel.implicitHeight + 10
                radius: height / 2
                color: buttonHover.hovered ? "#e0303036" : "#a0202024"
                Text {
                    id: buttonLabel
                    anchors.centerIn: parent
                    text: parent.modelData.label
                    color: "white"
                    font.pixelSize: 12
                }
                HoverHandler { id: buttonHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: parent.modelData.act() }
            }
        }
        // Back to the library.
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "✕"
            color: closeHover.hovered ? theme.accent : "white"
            font.pixelSize: 18
            style: Text.Outline
            styleColor: "#80000000"
            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.closeViewer() }
        }
    }

    // A video's progress along the bottom. Over it, the pointer gets a
    // thicker bar to click or drag through the video, the time, and the
    // sound switch.
    Item {
        id: seekBar
        visible: viewer.video && videoPlayer.duration > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: carousel.top
        height: 56
        opacity: root.progress
        readonly property bool active: seekHover.hovered || seekArea.pressed
        HoverHandler { id: seekHover }

        Rectangle {
            id: track
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: seekBar.active ? 6 : 3
            Behavior on height { NumberAnimation { duration: 100 } }
            color: seekBar.active ? "#50ffffff" : "transparent"
            Rectangle {
                height: parent.height
                width: parent.width * (videoPlayer.duration > 0 ? videoPlayer.position / videoPlayer.duration : 0)
                color: seekBar.active ? theme.accent : "white"
                opacity: 0.85
            }
        }

        MouseArea {
            id: seekArea
            anchors.fill: parent
            preventStealing: true
            // Seeks while dragging go out at most every 80 ms: a video still
            // downloading answers each with a new request.
            property real wanted: -1
            function seekTo(x) {
                wanted = Math.max(0, Math.min(1, x / width)) * videoPlayer.duration
                if (!seekThrottle.running) {
                    videoPlayer.setPosition(wanted)
                    wanted = -1
                    seekThrottle.start()
                }
            }
            Timer {
                id: seekThrottle
                interval: 80
                onTriggered: if (seekArea.wanted >= 0) { videoPlayer.setPosition(seekArea.wanted); seekArea.wanted = -1 }
            }
            onPressed: mouse => seekTo(mouse.x)
            onPositionChanged: mouse => { if (pressed) seekTo(mouse.x) }
        }

        Text {
            visible: seekBar.active || videoPlayer.playbackState !== MediaPlayer.PlayingState
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.bottom: track.top
            anchors.bottomMargin: 10
            text: root.clock(videoPlayer.position) + " / " + root.clock(videoPlayer.duration)
            color: "white"
            font.pixelSize: 12
            font.family: "monospace"
            style: Text.Outline
            styleColor: "#80000000"
        }
        Text {
            visible: seekBar.active || root.muted
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.bottom: track.top
            anchors.bottomMargin: 10
            text: root.muted ? qsTr("Sound off  (M)") : qsTr("Sound on  (M)")
            color: muteHover.hovered ? theme.accent : "white"
            font.pixelSize: 12
            style: Text.Outline
            styleColor: "#80000000"
            HoverHandler { id: muteHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.muted = !root.muted }
        }
    }

    // The carousel: the library's thumbnails in a row under the photo, the
    // shown one in the middle, sliding along as the photos do. Dragging it
    // scrubs through the library; clicking one shows it. Drawn directly: a
    // fixed set of slots, each keeping its photo until it scrolls off, so
    // sliding reloads nothing.
    Item {
        id: carousel
        objectName: "carousel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.stripHeight
        opacity: root.progress
        visible: root.progress > 0
        clip: true
        readonly property real cell: 38
        readonly property real thumb: 36
        // Where the row is, in photos: the shown one, plus how far the
        // photos still have to slide (all the way, for a long step).
        readonly property real position: viewer.index - viewer.stripOffset / viewer.pageWidth
        readonly property int slots: Math.ceil(width / cell) + 3
        readonly property int first: Math.floor(position - width / 2 / cell) - 1
        // The photo at a point of the strip.
        function indexAt(x) { return Math.round(position + (x - width / 2) / cell) }
        // The middle, where the shown photo sits: thumbnails grow as they
        // pass it, and the marker stays there while they slide under it.
        readonly property real middle: width / 2
        // Its own band: a zoomed photo passes under it.
        Rectangle {
            anchors.fill: parent
            color: "black"
        }

        Repeater {
            model: carousel.slots
            delegate: Item {
                required property int index
                // The photo in [first, first + slots) that falls to this slot.
                readonly property int photo: {
                    const n = carousel.slots
                    return carousel.first + ((index - carousel.first) % n + n) % n
                }
                // 1 at the middle, 0 a cell or more away.
                readonly property real nearness: Math.max(0, 1 - Math.abs(x + width / 2 - carousel.middle) / carousel.cell)
                visible: photo >= 0 && photo < viewer.count
                x: carousel.width / 2 + (photo - carousel.position) * carousel.cell - carousel.cell / 2
                width: carousel.cell
                height: carousel.height
                z: nearness
                Image {
                    anchors.centerIn: parent
                    width: carousel.thumb
                    height: carousel.thumb
                    scale: 1 + 0.2 * parent.nearness
                    source: parent.visible ? viewer.thumbnailAt(parent.photo) : ""
                    sourceSize: Qt.size(72, 72)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }
        }

        Rectangle {
            x: carousel.middle - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: carousel.thumb * 1.2 + 3
            height: width
            z: 2
            color: "transparent"
            border.color: "white"
            border.width: 1.5
        }

        MouseArea {
            anchors.fill: parent
            enabled: viewer.open
            preventStealing: true
            property real pressX: 0
            property int pressIndex: 0
            property bool scrubbed: false
            onPressed: mouse => {
                pressX = mouse.x
                pressIndex = viewer.index
                scrubbed = false
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                if (Math.abs(mouse.x - pressX) > 4)
                    scrubbed = true
                if (scrubbed)
                    viewer.jumpTo(Math.max(0, Math.min(viewer.count - 1,
                        pressIndex - Math.round((mouse.x - pressX) / carousel.cell))))
            }
            onClicked: mouse => {
                if (!scrubbed)
                    viewer.goTo(Math.max(0, Math.min(viewer.count - 1, carousel.indexAt(mouse.x))))
            }
        }
    }

    // Sound stays off from one video to the next until turned on again.
    property bool muted: false
    readonly property real videoPosition: videoPlayer.position
    readonly property real videoDuration: videoPlayer.duration

    // 75000 → "1:15", 3725000 → "1:02:05".
    function clock(ms) {
        const s = Math.floor(ms / 1000)
        const two = n => (n < 10 ? "0" : "") + n
        return s >= 3600 ? Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
                         : Math.floor(s / 60) + ":" + two(s % 60)
    }

    // Back or ahead in a video; false when there is no video to move in.
    function skip(seconds) {
        if (!viewer.video || videoPlayer.duration <= 0)
            return false
        videoPlayer.setPosition(Math.max(0, Math.min(videoPlayer.duration - 100, videoPlayer.position + seconds * 1000)))
        return true
    }

    function togglePlay() {
        if (!viewer.video)
            return false
        if (videoPlayer.playbackState === MediaPlayer.PlayingState)
            videoPlayer.pause()
        else
            videoPlayer.play()
        return true
    }
}
