import QtQuick
import QtMultimedia
import Pinhole.Plugin

// The motion's player and its picture, silent (LiveLayer.qml). A file of
// its own, made the first time a motion plays: Qt Multimedia is read then,
// once the viewer has it, not as Pinhole starts with the plugin.
Item {
    id: motion
    // A loop or a bounce: round and round.
    property bool looping: false
    readonly property alias player: player
    readonly property alias output: output
    readonly property bool shows: output.visible
    // Not yet moving: before its first frames, or waiting for more.
    readonly property bool stalled: player.position <= 0 || player.mediaStatus === MediaPlayer.StalledMedia
    signal failed()

    // A motion in HDR (HLG) made SDR on its way, as the viewer's video is.
    SdrVideo {
        id: sdr
        output: output
    }
    MediaPlayer {
        id: player
        videoOutput: sdr
        loops: motion.looping ? MediaPlayer.Infinite : 1
        onPlaybackStateChanged: if (playbackState === MediaPlayer.StoppedState) output.visible = false
        onErrorOccurred: {
            output.visible = false
            motion.failed()
        }
    }
    VideoOutput {
        id: output
        anchors.fill: parent
        visible: false
        fillMode: VideoOutput.PreserveAspectCrop
    }
}
