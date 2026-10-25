import QtQuick
import QtQuick.Window
import QtMultimedia

// The full-window view of one photo or video. It grows out of the photo's
// cell in the grid and shrinks back into it; the square crop of the cell
// relaxes into the whole photo as it grows.
Item {
    id: root

    required property var grid
    property double openedAt: 0
    // Right-click: the window's photo menu.
    signal menuRequested(real x, real y)
    readonly property bool shown: viewer.open || progress > 0

    visible: shown
    // 0: the photo's cell in the grid. 1: fitted to the window.
    property real progress: 0
    // Where it grows from and shrinks back to.
    property rect cell: Qt.rect(0, 0, 0, 0)

    readonly property real fitWidth: Math.min(width, height * viewer.aspect)
    readonly property real fitHeight: fitWidth / Math.max(0.01, viewer.aspect)

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
            mediumImage.source = ""
            originalImage.source = ""
            liveOutput.visible = false
            livePlayer.stop()
        }
        function onImageChanged() {
            if (viewer.stage === "medium")
                mediumImage.source = viewer.image
            else if (viewer.stage === "original")
                originalImage.source = viewer.image
        }
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

    Item {
        id: frame
        x: root.lerp(root.cell.x, (root.width - root.fitWidth) / 2, root.progress)
        y: root.lerp(root.cell.y, (root.height - root.fitHeight) / 2, root.progress)
        width: root.lerp(root.cell.width, root.fitWidth, root.progress)
        height: root.lerp(root.cell.height, root.fitHeight, root.progress)
        clip: true

        // Sharpest last: each one covers the one below once it has loaded.
        Image {
            anchors.fill: parent
            source: viewer.thumbnail
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            smooth: true
        }
        Image {
            id: mediumImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
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
        function sideAt(x) { return x < side ? -1 : x > width - side ? 1 : 0 }
        readonly property int hoveredSide: containsMouse ? sideAt(mouseX) : 0

        onPressed: mouse => {
            if (mouse.button === Qt.BackButton)
                root.closeViewer()
            else if (mouse.button === Qt.RightButton)
                root.menuRequested(mouse.x, mouse.y)
        }
        onClicked: mouse => {
            if (mouse.button !== Qt.LeftButton)
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
            else
                root.closeViewer()
        }
    }

    // The sides, while the pointer is over one.
    Repeater {
        model: [-1, 1]
        delegate: Text {
            required property int modelData
            anchors.verticalCenter: parent.verticalCenter
            x: modelData < 0 ? 24 : root.width - width - 24
            text: modelData < 0 ? "‹" : "›"
            color: "white"
            font.pixelSize: 56
            style: Text.Outline
            styleColor: "#80000000"
            readonly property bool can: modelData < 0 ? viewer.index > 0 : viewer.index < root.grid.count - 1
            opacity: pointer.hoveredSide === modelData && can ? 0.9 * root.progress : 0
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
        anchors.bottom: parent.bottom
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
