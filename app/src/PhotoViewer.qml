import QtQuick
import QtQuick.Window
import QtMultimedia
import "keys.js" as KeyNames

// The full-window view of one photo or video, as in Photos. It grows out of
// the photo's cell in the grid and shrinks back into it; the square crop of
// the cell relaxes into the whole photo as it grows. The photos lie side by
// side and follow the fingers; a pinch zooms one; the carousel along the
// bottom shows where in the library it is. The motion itself is the
// Viewer's (viewermotion.h); this draws it.
Item {
    id: root

    required property var grid
    // Plugins' buttons for a photo (PluginAction), before Copy.
    property var pluginActions: []
    property double openedAt: 0
    // Presses and clicks the viewer's mouse area has had (self-tests).
    property int presses: 0
    property int clicks: 0
    readonly property bool shown: viewer.open || progress > 0
    // A menu of its bar is open, and has the arrows, ⏎ and Esc.
    readonly property bool menuOpen: actionBar.menuOpen

    visible: shown
    // 0: the photo's cell in the grid. 1: fitted to the window.
    property real progress: 0
    // Where it grows from and shrinks back to.
    property rect cell: Qt.rect(0, 0, 0, 0)

    // The carousel's band along the bottom; photos fit the area above it.
    readonly property real stripHeight: 60
    readonly property real areaHeight: Math.max(1, height - stripHeight)
    // Dragged up or down to close: how far along, 0 to 1, and what the
    // background and controls keep of their opacity meanwhile.
    readonly property real dismissed: Math.min(1, Math.abs(viewer.dismiss) / Math.max(1, height * 0.45))
    readonly property real chrome: 1 - dismissed
    readonly property real fitWidth: Math.min(width, areaHeight * viewer.aspect)
    readonly property real fitHeight: fitWidth / Math.max(0.01, viewer.aspect)
    onWidthChanged: viewer.setViewport(width, areaHeight)
    onAreaHeightChanged: viewer.setViewport(width, areaHeight)
    Component.onCompleted: {
        viewer.setViewport(width, areaHeight)
        // Opened before the viewer was made (Main.qml): opens now.
        if (viewer.open)
            opened()
    }

    FrameAnimation {
        running: viewer.open && viewer.animating
        onTriggered: viewer.advance(frameTime)
    }

    function lerp(a, b, t) { return a + (b - a) * t }

    function opened() {
        root.openedAt = Date.now()
        root.cell = viewer.origin
        closing.stop()
        opening.start()
    }

    function closeViewer() {
        actionBar.closeMenus()
        root.grid.reveal(viewer.index)
        root.cell = root.grid.rectOf(viewer.index)
        videoPlayer.stop()
        livePlayer.stop()
        closing.start()
    }

    Connections {
        target: viewer
        function onOpenChanged() {
            if (viewer.open)
                root.opened()
        }
        function onIndexChanged() {
            originalImage.source = ""
            detailImage.source = ""
            liveOutput.visible = false
            livePlayer.stop()
            liveNotice.hide()
            actionBar.closeMenus()
        }
        // Another effect: a loop or a bounce plays once it is here (liveSource).
        function onEffectChanged() {
            if (!viewer.looping)
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
        function onLiveFailed(message) { liveNotice.say(message) }
    }
    Connections {
        target: favourites
        enabled: root.shown
        function onFailed(message) { liveNotice.say(message, favoriteButton, favoriteNudge) }
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
        color: theme.darkerBackground
        opacity: root.progress * root.chrome
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
            opacity: root.chrome
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
        y: root.lerp(root.cell.y, root.areaHeight / 2 + viewer.panY + viewer.dismiss - targetHeight / 2, root.progress)
        // Shrinking a little as it is dragged away, as in Photos.
        scale: 1 - 0.25 * root.dismissed * root.progress
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
        // A Live Photo shown as a long exposure: its frames as one.
        Image {
            id: longExposureImage
            anchors.fill: parent
            source: viewer.effect === "long_exposure" ? viewer.longExposure : ""
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
            audioOutput: AudioOutput { muted: root.muted || silent }
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
            // A loop and a bounce go round until another effect or photo.
            loops: viewer.looping ? MediaPlayer.Infinite : 1
            onPlaybackStateChanged: if (playbackState === MediaPlayer.StoppedState) liveOutput.visible = false
            onErrorOccurred: {
                liveOutput.visible = false
                liveNotice.say(qsTr("Could not play the motion"))
            }
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
    // library, as does the mouse's back button. Dragged, the photo goes as a
    // touchpad swipe takes it: up or down and let go to close, sideways to
    // the next photo (zoomed, a drag pans it). The wheel and the swipes are
    // the Viewer's own (viewer.cpp), as the window's events.
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
        property point pressedAt
        property bool panned: false
        // The unzoomed photo held by the left button (viewer.grab()).
        property bool grabbed: false
        onPositionChanged: mouse => {
            if (!(mouse.buttons & Qt.LeftButton))
                return
            if (grabbed)
                viewer.drag(mouse.x - last.x, mouse.y - last.y)
            else if (viewer.zoomed)
                viewer.panBy(mouse.x - last.x, mouse.y - last.y)
            else
                return
            // Past a few pixels it is a drag, not a click.
            if (viewer.zoomed ? Math.abs(mouse.x - last.x) + Math.abs(mouse.y - last.y) > 0
                              : Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) > 8)
                panned = true
            last = Qt.point(mouse.x, mouse.y)
        }
        function letGo() {
            if (!grabbed)
                return
            grabbed = false
            viewer.drop()
        }
        onReleased: mouse => { if (mouse.button === Qt.LeftButton) letGo() }
        onCanceled: letGo()
        readonly property int hoveredSide: containsMouse ? sideAt(mouseX) : 0

        onPressed: mouse => {
            root.presses += 1
            last = Qt.point(mouse.x, mouse.y)
            pressedAt = last
            panned = false
            // Any press is a touch first: a coasting zoomed photo stops (a
            // two-finger tap, which arrives as the right button, does only that).
            viewer.stopCoasting()
            if (mouse.button === Qt.BackButton)
                root.closeViewer()
            if (mouse.button === Qt.LeftButton && !viewer.zoomed) {
                grabbed = true
                viewer.grab()
            }
        }
        onClicked: mouse => {
            root.clicks += 1
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
        delegate: Chip {
            required property int modelData
            x: modelData < 0 ? 24 : root.width - width - 24
            text: modelData < 0 ? "‹" : "›"
            label.font.pixelSize: theme.fontSize * 2
            width: height
            readonly property bool can: modelData < 0 ? viewer.index > 0 : viewer.index < viewer.count - 1
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
        spacing: 6
        opacity: root.progress * root.chrome
        Row {
            spacing: 6
            Chip {
                // The date bright, the time after it muted.
                readonly property var parts: viewer.title.split(" · ")
                text: parts.length > 1 ? parts[0] + " <font color=\"" + theme.darkForeground + "\">· " + parts[1] + "</font>"
                                       : viewer.title
                textColor: theme.brightForeground
            }
            // The full photo or the video still coming, after a second.
            Spinner {
                objectName: "loadingSpinner"
                busy: viewer.loading
            }
        }
        Chip {
            text: viewer.message
            visible: text !== ""
            textColor: theme.darkForeground
        }
    }

    // What can be done with it, as one card: its favourite, Live and its
    // effects, the plugins' groups, Copy and Export, and back to the
    // library.
    Row {
        id: actions
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 8
        opacity: root.progress * root.chrome
        ActionBar {
            id: actionBar
            objectName: "viewerActions"
            // A favourite or not: a filled heart in the accent, or an empty
            // one (Nerd Font's: the typeface's own ♥ is an outline too).
            MenuGroup {
                ShellButton {
                    id: favoriteButton
                    objectName: "favoriteButton"
                    flat: true
                    text: viewer.favorite ? "\uf004" : "\uf08a"
                    labelColor: viewer.favorite ? theme.accent : theme.foreground
                    key: "F"
                    keyLayer: "viewer"
                    section: "photo"
                    helpText: qsTr("Favourite")
                    onTapped: favourites.toggle([viewer.index])
                    transform: Translate { id: favoriteNudge }
                }
            }
            // The effects, as on the iPhone: Live, Loop, Bounce, Long
            // exposure, the one set on the iPhone saying so.
            MenuGroup {
                visible: viewer.live
                menuKey: "Shift+L"
                menuText: qsTr("Live effects")
                keyLayer: "viewer"
                section: "photo"
                items: root.effects.map(effect => ({
                    text: root.effectName(effect),
                    hint: effect === viewer.savedEffect && effect !== "live" ? "iPhone" : "",
                    active: viewer.effect === effect,
                    triggered: () => viewer.setEffect(effect)
                }))
                // Lit from the click until the motion has played. While it
                // is on its way (asked for, or a stream before its first
                // frames) its key turns; a file already here plays at once,
                // before it would.
                ShellButton {
                    id: liveButton
                    objectName: "liveButton"
                    flat: true
                    text: root.effectName(viewer.effect)
                    key: "L"
                    keyLayer: "viewer"
                    section: "photo"
                    helpText: qsTr("Play the Live Photo")
                    active: viewer.liveLoading || liveOutput.visible
                    busy: viewer.liveLoading || root.liveBuffering
                    busyDelay: 300
                    onTapped: viewer.playLive()
                    transform: Translate { id: liveNudge }
                }
            }
            Repeater {
                model: actionBar.groupsOf(root.pluginActions)
                delegate: PluginGroup {
                    required property var modelData
                    items: modelData
                    keyLayer: "viewer"
                }
            }
            // Out of the app: Copy answers with a tick on itself; Export as
            // JPEG or MP4, what opens anywhere, or the original.
            MenuGroup {
                menuKey: "Shift+C"
                keyLayer: "viewer"
                section: "photo"
                items: [
                    { text: qsTr("Export"), key: "E", triggered: () => outgoing.exportTo([viewer.index], true) },
                    { text: qsTr("Export original"), key: "Shift+E", triggered: () => outgoing.exportTo([viewer.index], false) }
                ]
                CopyButton {
                    objectName: "viewerCopy"
                    flat: true
                    photos: () => [viewer.index]
                    key: "C"
                    keyLayer: "viewer"
                    section: "photo"
                }
            }
            // Back to the library; with a menu open, Esc closes that.
            MenuGroup {
                ShellButton {
                    flat: true
                    text: "✕"
                    key: "Escape"
                    keyLayer: "viewer"
                    section: "photo"
                    helpText: qsTr("Back to the library")
                    onTapped: root.closeViewer()
                }
            }
        }
    }

    // The motion could not be had: said for a moment under the Live button,
    // which shakes, as Copy does when nothing could be copied.
    Chip {
        id: liveNotice
        objectName: "liveNotice"
        y: actions.y + actions.height + 6
        textColor: theme.darkForeground
        visible: opacity > 0
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        // Under the button it is about (Live's unless said), which shakes.
        property var nudged: liveNudge
        function say(message, button = liveButton, shaken = liveNudge) {
            text = message
            x = Math.max(16, Math.min(button.mapToItem(root, 0, 0).x, root.width - 16 - width))
            opacity = root.progress * root.chrome
            nudged = shaken
            noticeShown.restart()
            nudge.restart()
        }
        function hide() {
            noticeShown.stop()
            opacity = 0
        }
        Timer { id: noticeShown; interval: 4000; onTriggered: liveNotice.opacity = 0 }
        SequentialAnimation {
            id: nudge
            NumberAnimation { target: liveNotice.nudged; property: "x"; to: -5; duration: 50 }
            NumberAnimation { target: liveNotice.nudged; property: "x"; to: 5; duration: 80 }
            NumberAnimation { target: liveNotice.nudged; property: "x"; to: -3; duration: 70 }
            NumberAnimation { target: liveNotice.nudged; property: "x"; to: 0; duration: 60 }
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
        opacity: root.progress * root.chrome
        readonly property bool active: seekHover.hovered || seekArea.pressed
        HoverHandler { id: seekHover }

        Rectangle {
            id: track
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: seekBar.active ? 6 : 3
            Behavior on height { NumberAnimation { duration: 100 } }
            color: seekBar.active ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.3) : "transparent"
            Rectangle {
                height: parent.height
                width: parent.width * (videoPlayer.duration > 0 ? videoPlayer.position / videoPlayer.duration : 0)
                color: seekBar.active ? theme.accent : theme.foreground
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

        Chip {
            visible: seekBar.active || videoPlayer.playbackState !== MediaPlayer.PlayingState
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.bottom: track.top
            anchors.bottomMargin: 10
            text: root.clock(videoPlayer.position) + " / " + root.clock(videoPlayer.duration)
        }
        ShellButton {
            visible: seekBar.active || root.muted
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.bottom: track.top
            anchors.bottomMargin: 10
            text: root.muted ? qsTr("Sound off") : qsTr("Sound on")
            // Its key works with the button hidden.
            action: soundAction
            active: root.muted
            floating: true
        }
        Command {
            id: soundAction
            name: "video.sound"
            text: qsTr("Sound")
            section: "video"
            keys: ["M"]
            keyLayer: "viewer"
            onTriggered: root.muted = !root.muted
        }
    }

    // The carousel: the library's thumbnails in a row under the photo, the
    // shown one in the middle, sliding along as the photos do. Dragging it
    // scrubs through the library; clicking one shows it. A stack's photos
    // (a series) have a line under them; those of the shown photo's stack
    // a line in the accent, and the rest dim. Drawn directly: a
    // fixed set of slots, each keeping its photo until it scrolls off, so
    // sliding reloads nothing.
    Item {
        id: carousel
        objectName: "carousel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.stripHeight
        opacity: root.progress * root.chrome
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
        // The stack the shown photo is in, by its card's photo, or -1.
        readonly property int stack: (root.grid.stacks, viewer.open ? root.grid.stackOf(viewer.index) : -1)
        function stackOf(i) { return i >= 0 && i < viewer.count ? (root.grid.stacks, root.grid.stackOf(i)) : -1 }
        // The middle, where the shown photo sits: thumbnails grow as they
        // pass it, and the marker stays there while they slide under it.
        readonly property real middle: width / 2
        // Its own band, the theme's dark background with a hairline above:
        // a zoomed photo passes under it.
        Rectangle {
            anchors.fill: parent
            color: theme.darkBackground
            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.15)
            }
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
                readonly property int stack: visible ? carousel.stackOf(photo) : -1
                readonly property bool shownStack: stack >= 0 && stack === carousel.stack
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
                    opacity: carousel.stack >= 0 && !parent.shownStack ? 0.4 : 1
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                    // As Omarchy's image picker draws the ones not chosen.
                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 1
                        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.28)
                    }
                }
                // The stack's line: one across its photos, rounded at its ends.
                Rectangle {
                    readonly property bool first: carousel.stackOf(parent.photo - 1) !== parent.stack
                    readonly property bool last: carousel.stackOf(parent.photo + 1) !== parent.stack
                    visible: parent.stack >= 0
                    // A pixel over into the next, so the line has no seams.
                    x: first ? 3 : 0
                    width: parent.width - x + (last ? -3 : 1)
                    y: parent.height - 6
                    height: 3
                    radius: first || last ? 1.5 : 0
                    color: parent.shownStack ? theme.accent
                         : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.35)
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
            border.color: theme.accent
            border.width: 2
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

    readonly property var effects: ["live", "loop", "bounce", "long_exposure"]
    function effectName(effect) {
        switch (effect) {
        case "loop": return qsTr("Loop")
        case "bounce": return qsTr("Bounce")
        case "long_exposure": return qsTr("Long exposure")
        default: return qsTr("Live")
        }
    }

    // Sound stays off from one video to the next until turned on again.
    property bool muted: false
    readonly property real videoPosition: videoPlayer.position
    readonly property real videoDuration: videoPlayer.duration
    // The motion streaming in, not yet moving.
    readonly property bool liveBuffering: liveOutput.visible && livePlayer.source.toString().startsWith("http")
        && (livePlayer.position <= 0 || livePlayer.mediaStatus === MediaPlayer.StalledMedia)

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
