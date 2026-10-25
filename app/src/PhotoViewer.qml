import QtQuick
import QtQuick.Window
import QtMultimedia

// The full-window view of one photo or video. It grows out of the photo's
// cell in the grid and shrinks back into it; the square crop of the cell
// relaxes into the whole photo as it grows.
Item {
    id: root

    required property var grid
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
            audioOutput: AudioOutput {}
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
            text: qsTr("LIVE  (L)")
            color: "white"
            font.pixelSize: 12
            font.bold: true
        }
    }

    // A video's progress, along the bottom.
    Rectangle {
        visible: viewer.video && videoPlayer.duration > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        height: 3
        width: parent.width * (videoPlayer.duration > 0 ? videoPlayer.position / videoPlayer.duration : 0)
        color: "white"
        opacity: 0.8 * root.progress
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
