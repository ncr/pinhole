import QtQuick
import QtQuick.Window
import Pinhole

// The stage (stage.h, docs/hdr.md H3): a window under the main one, its
// subsurface, that draws the viewer's backdrop, its settled photo in the
// photo's own colours and its HDR video, in linear light (scRGB). Shown
// only while the viewer is settled: open, grown, still, not dragged. Then
// it takes over at SDR, the very pixels the main window drew, and fades
// its wider gamut and its HDR in over 400 ms; until then, and as soon as
// the viewer moves, the main window draws the photo as ever.
//
// The hand-over, frame by frame: the stage's window shown, empty and
// transparent, and placed under the main window's surface, which takes
// that with its next frame; then its backdrop and photo, drawn; once that
// frame is out, the main window turns transparent there (`handedOver`).
// Going back, the reverse: the main window draws its photo again, and once
// that frame is out the stage is hidden.
Window {
    id: stageWindow

    // The main window (Main.qml), its parent.
    required property Window mainWindow
    // The PhotoViewer, once made.
    readonly property var photoViewer: mainWindow.photoViewer
    // A plugin asks for SDR (PinholePlugin.holdSdr): the Live plugin while
    // its motion, SDR, plays over the photo.
    readonly property bool holdSdr: mainWindow.pluginsHoldSdr

    flags: Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
    x: 0
    y: 0
    width: mainWindow.width
    height: mainWindow.height
    visible: false

    // In linear light on scRGB: sRGB's curve undone.
    function linear(c) {
        return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
    }
    readonly property color backdrop: stage.scrgb ? Qt.rgba(linear(theme.darkerBackground.r), linear(theme.darkerBackground.g),
                                                            linear(theme.darkerBackground.b), 1)
                                                  : theme.darkerBackground
    color: placed ? backdrop : "transparent"

    // Kept from taking over, for a test to see the main window's photo
    // settled first (PINHOLE_SELFTEST=stage).
    property bool paused: false
    // Open, grown, still and not dragged away: where the stage shows.
    readonly property bool settled: stage.usable && !paused && photoViewer !== null && viewer.open && viewer.index >= 0
                                    && photoViewer.progress === 1 && viewer.offset === 0 && viewer.dismiss === 0
                                    && !photoViewer.closing
    readonly property var sharpest: photoViewer !== null ? photoViewer.sharpest : null
    // The stage draws the viewer's photo: decoded, and of this photo.
    readonly property bool ready: photo.shownTag === viewer.index && photo.shown.toString() !== ""

    // Under the main window, its stacking taken.
    property bool placed: false
    // The main window turns transparent where the stage shows.
    property bool handedOver: false
    // Frames of the main window, or the stage's, still to come before the
    // next step.
    property int mainFrames: 0
    property int stageFrames: 0

    // 0 at the hand-over, the main window's photo exactly; to 1 in 400 ms.
    property real fade: 0
    NumberAnimation on fade { id: fadeIn; running: false; from: 0; to: 1; duration: 400 }
    // Kept at SDR while a plugin asks; the switch in the side panel.
    property real hold: holdSdr ? 0 : 1
    Behavior on hold { NumberAnimation { duration: 400 } }
    property real full: stage.fullHdr ? 1 : 0
    Behavior on full { NumberAnimation { duration: 400 } }
    // How much of the photo's gamut beyond sRGB, and of the display's
    // headroom, are shown: none on a stage without scRGB.
    readonly property real gamutWeight: stage.scrgb ? fade * hold : 0
    readonly property real hdrWeight: stage.scrgb ? fade * hold * full : 0

    // What it shows: the viewer's sharpest image while settled, kept while
    // it goes away, let go once it has.
    function follow() {
        if (settled && sharpest !== null) {
            photo.tag = viewer.index
            photo.sourceSize = sharpest.size
            photo.mipmap = sharpest.mipmap
            photo.source = sharpest.source
        } else if (!visible) {
            photo.source = ""
            photo.tag = -1
        }
        step()
    }
    onSettledChanged: follow()
    onSharpestChanged: follow()
    onReadyChanged: step()
    onVisibleChanged: if (!visible) follow()

    function step() {
        if (settled && ready) {
            if (!visible) {
                placed = false
                handedOver = false
                visible = true
                if (!stage.placeBelow(stageWindow)) {
                    visible = false
                    return
                }
                // Its stacking is the main window's to commit: a frame of it.
                mainFrames = 2
                mainWindow.update()
            }
            return
        }
        // Back to the main window: it draws the photo again, then the
        // stage goes.
        fadeIn.stop()
        fade = 0
        stageFrames = 0
        if (handedOver) {
            handedOver = false
            mainFrames = 2
            mainWindow.update()
        } else if (visible && mainFrames === 0) {
            visible = false
            placed = false
        }
    }

    Connections {
        target: stageWindow.mainWindow
        function onFrameSwapped() {
            if (stageWindow.mainFrames === 0)
                return
            if (--stageWindow.mainFrames > 0) {
                stageWindow.mainWindow.update()
                return
            }
            if (stageWindow.visible && !stageWindow.placed && stageWindow.settled && stageWindow.ready) {
                // Under it now: the backdrop and the photo drawn.
                stageWindow.placed = true
                stageWindow.stageFrames = 2
            } else if (!stageWindow.handedOver) {
                stageWindow.visible = false
                stageWindow.placed = false
            }
        }
    }
    onFrameSwapped: {
        if (stageFrames === 0)
            return
        if (--stageFrames > 0) {
            update()
            return
        }
        if (placed && settled && ready && !handedOver) {
            handedOver = true
            fadeIn.restart()
        }
    }

    // As the viewer's frame: fitted, zoomed and panned (PhotoViewer.qml).
    Item {
        id: frame
        readonly property Item viewerFrame: stageWindow.photoViewer !== null ? stageWindow.photoViewer.frameItem : null
        visible: stageWindow.placed
        x: viewerFrame !== null ? viewerFrame.x : 0
        y: viewerFrame !== null ? viewerFrame.y : 0
        width: viewerFrame !== null ? viewerFrame.width : 0
        height: viewerFrame !== null ? viewerFrame.height : 0
        scale: viewerFrame !== null ? viewerFrame.scale : 1
        clip: true

        StagePhoto {
            id: photo
            objectName: "stagePhoto"
            // The viewer's photo it is of.
            property int tag: -1
            property int shownTag: -1
            onShownChanged: shownTag = shown.toString() !== "" ? tag : -1
            anchors.fill: parent
            gamut: stageWindow.gamutWeight
            linear: stage.scrgb
        }
        // The viewer's video, its frames as they come, HDR up to the
        // display's headroom.
        VideoView {
            objectName: "stageVideo"
            anchors.fill: parent
            visible: stageWindow.photoViewer !== null && stageWindow.photoViewer.videoShown
            follows: stageWindow.placed && stageWindow.photoViewer !== null ? stageWindow.photoViewer.videoView : null
            fillMode: VideoView.PreserveAspectCrop
            linear: stage.scrgb
            headroom: Math.pow(2, display.headroom * stageWindow.hdrWeight)
        }
    }
}
