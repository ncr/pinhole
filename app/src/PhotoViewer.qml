import QtQuick
import Pinhole
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
    // Plugins' buttons for a photo (PluginAction), before Copy, in their
    // order.
    property var pluginActions: []
    // Plugins' layers over the photo (PinholePlugin.viewerLayer), each
    // { plugin, component }, in the plugins' order.
    property var pluginLayers: []
    // The window's keys and gestures (Main.qml), for its ? button here.
    property var helpAction: null
    property double openedAt: 0
    // Presses and clicks the viewer's mouse area has had (self-tests).
    property int presses: 0
    property int clicks: 0
    readonly property bool shown: viewer.open || progress > 0
    // A menu of its bar is open, and has the arrows, ⏎ and Esc.
    readonly property bool menuOpen: actionBar.menuOpen

    // The stage shows the photo (Stage.qml, docs/hdr.md H3): here its
    // backdrop, image and video are not drawn, and the window is
    // transparent where they were; the rest (the bar, the plugins' layers)
    // as ever.
    property bool staged: false
    // For the stage: where the photo is, its video, and the sharpest image
    // shown now, { source, size, mipmap }, or null.
    readonly property Item frameItem: frame
    readonly property VideoView videoView: videoOutput
    readonly property bool videoShown: viewer.video && videoPlayer.playbackState !== MediaPlayer.StoppedState
    readonly property var sharpest: {
        const of = (image, mipmap) => ({ source: image.source, size: Qt.size(image.implicitWidth, image.implicitHeight),
                                         mipmap: mipmap })
        if (detailImage.status === Image.Ready && detailImage.opacity >= 1)
            return of(detailImage, true)
        if (originalImage.status === Image.Ready && originalImage.opacity >= 1)
            return of(originalImage, false)
        if (mediumImage.status === Image.Ready)
            return of(mediumImage, false)
        if (thumbnailImage.status === Image.Ready)
            return of(thumbnailImage, false)
        return null
    }

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
        shrinking.stop()
        if (viewer.origin.width <= 0 || viewer.origin.height <= 0) {
            growing.stop()
            root.progress = 1
        } else {
            growing.start()
        }
    }

    // Shrinking back into the grid: viewer.open still, until it is there.
    readonly property bool closing: shrinking.running

    function closeViewer() {
        actionBar.closeMenus()
        videoPlayer.stop()
        layers.hide()
        // Still opening, it would grow again after it closed, and stay.
        growing.stop()
        if (viewer.origin.width <= 0 || viewer.origin.height <= 0) {
            root.progress = 0
            viewer.close()
            return
        }
        root.grid.reveal(viewer.index)
        root.cell = root.grid.rectOf(viewer.index)
        shrinking.start()
    }

    Connections {
        target: viewer
        function onOpenChanged() {
            if (viewer.open)
                root.opened()
            // Closed by something else (the photo frame starting from it,
            // its photo gone): it shrinks away as Esc has it, rather than
            // stay over the grid. With no photo to shrink into, at once.
            else if (!shrinking.running && (root.progress > 0 || growing.running)) {
                if (viewer.index >= 0) {
                    root.closeViewer()
                } else {
                    growing.stop()
                    root.progress = 0
                }
            }
        }
        // A photo opened as it shrinks away: it grows back.
        function onOpenAsked() {
            if (shrinking.running)
                root.opened()
        }
        function onIndexChanged() {
            originalImage.source = ""
            detailImage.source = ""
            actionBar.hush()
            actionBar.closeMenus()
            layers.photoChanged()
        }
        function onImageChanged() {
            if (viewer.stage === "original")
                originalImage.source = viewer.image
        }
        function onCloseRequested() { root.closeViewer() }
    }

    NumberAnimation {
        id: growing
        target: root; property: "progress"; to: 1; duration: 260; easing.type: Easing.OutCubic
    }
    NumberAnimation {
        id: shrinking
        target: root; property: "progress"; to: 0; duration: 220; easing.type: Easing.OutCubic
        onFinished: viewer.close()
    }

    Rectangle {
        anchors.fill: parent
        color: theme.darkerBackground
        opacity: root.progress * root.chrome
        visible: !root.staged
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
            visible: root.progress === 1 && at >= 0 && x < root.width && x + width > 0
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
        objectName: "viewerFrame"
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
            id: thumbnailImage
            anchors.fill: parent
            source: viewer.thumbnail
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            smooth: true
            visible: !root.staged
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
            visible: !root.staged
        }
        Image {
            id: originalImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
            visible: !root.staged
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
            visible: !root.staged
            readonly property bool wanted: viewer.zoom > 1.3 && originalImage.status === Image.Ready
            onWantedChanged: if (wanted && source.toString() === "") source = originalImage.source
        }
        MediaPlayer {
            id: videoPlayer
            source: viewer.video ? viewer.videoSource : ""
            videoOutput: videoOutput
            audioOutput: AudioOutput { muted: root.muted || silent }
            onSourceChanged: if (source.toString() !== "" && viewer.open) play()
        }
        // HDR video (an iPhone's HLG, Dolby Vision) tone mapped as it is
        // drawn (videoview.h); any other as Qt draws it.
        VideoView {
            id: videoOutput
            anchors.fill: parent
            visible: root.videoShown && !root.staged
            fillMode: VideoView.PreserveAspectCrop
        }

        // The plugins' layers, in their order, each made afresh for each
        // photo shown (`photo`), told `shown` false the moment the photo
        // changes or the viewer starts to shrink away, and gone once it
        // has. Photos passing quicker than a person looks at them (a held
        // key, the carousel scrubbed) get theirs once it stops on one, as
        // the viewer fetches (viewer.cpp's kPassing).
        Item {
            id: layers
            objectName: "viewerLayers"
            anchors.fill: parent
            property double changedAt: 0
            function each(f) {
                for (let i = 0; i < layerSlots.count; ++i) {
                    const slot = layerSlots.itemAt(i)
                    if (slot)
                        f(slot)
                }
            }
            function hide() { each(slot => slot.hide()) }
            function drop() {
                later.stop()
                each(slot => slot.drop())
            }
            function make() {
                if (viewer.open && viewer.index >= 0 && !root.closing)
                    each(slot => slot.make())
            }
            function photoChanged() {
                drop()
                const now = Date.now()
                const passing = now - changedAt < 150
                changedAt = now
                if (passing) {
                    later.restart()
                } else {
                    later.stop()
                    Qt.callLater(makeSoon)
                }
            }
            // Not when another photo came meanwhile, passing: then later.
            function makeSoon() {
                if (!later.running)
                    make()
            }
            Timer { id: later; interval: 150; onTriggered: layers.make() }
            // Any of them still bringing what it shows: the spinner turns.
            readonly property bool loading: {
                let any = false
                for (let i = 0; i < layerSlots.count; ++i) {
                    const slot = layerSlots.itemAt(i)
                    if (slot && slot.loading)
                        any = true
                }
                return any
            }
            Connections {
                target: viewer
                function onOpenChanged() {
                    if (!viewer.open)
                        layers.drop()
                }
            }
            Repeater {
                id: layerSlots
                model: root.pluginLayers
                delegate: Item {
                    id: layerSlot
                    required property var modelData
                    anchors.fill: parent
                    property var made: null
                    // What it shows still on its way (a layer's `loading`).
                    readonly property bool loading: made !== null && made.loading === true
                    function make() {
                        drop()
                        made = modelData.component.createObject(layerSlot, { photo: viewer.index, shown: true })
                        if (made === null)
                            plugins.layerFailed(modelData.plugin, modelData.component.errorString())
                    }
                    function hide() {
                        if (made)
                            made.shown = false
                    }
                    function drop() {
                        if (!made)
                            return
                        made.shown = false
                        made.destroy()
                        made = null
                    }
                    // One that came while a photo shows has its layer at once.
                    Component.onCompleted: if (viewer.open && viewer.index >= 0 && !root.closing) make()
                    Component.onDestruction: drop()
                }
            }
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
            readonly property bool can: viewer.beside(modelData) >= 0
            opacity: pointer.hoveredSide === modelData && can ? 0.9 * root.progress : 0
            y: (root.areaHeight - height) / 2
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
    }

    // What is shown, and what is still on its way.
    Column {
        id: titleColumn
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 6
        opacity: root.progress * root.chrome
        Row {
            spacing: 6
            Chip {
                id: dateChip
                // The date bright, the time after it muted.
                readonly property var parts: viewer.title.split(" · ")
                text: parts.length > 1 ? parts[0] + " <font color=\"" + theme.darkForeground + "\">· " + parts[1] + "</font>"
                                       : viewer.title
                textColor: theme.brightForeground
                // A click on it is not one on the photo's left side,
                // which steps to the one before.
                Shield { anchors.fill: parent; z: -1 }
            }
            // The full photo or the video still coming, or what a plugin's
            // layer shows over it (a Live Photo's effect), after a second.
            Spinner {
                objectName: "loadingSpinner"
                busy: viewer.loading || layers.loading
            }
        }
        Chip {
            text: viewer.message
            visible: text !== ""
            textColor: theme.darkForeground
            Shield { anchors.fill: parent; z: -1 }
        }
    }

    // What can be done with it, as one card: the plugins' groups (the
    // heart and Live with its effects first), Copy and Export, and back to
    // the library. Right of the date, as much as fits there (`fit`); under it
    // when not even the least does.
    Row {
        id: actions
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 16
        anchors.topMargin: actionBar.below ? titleColumn.y + titleColumn.height + 6 : 16
        spacing: 8
        opacity: root.progress * root.chrome
        ActionBar {
            id: actionBar
            objectName: "viewerActions"

            // How much it says, from 0, everything, to 2: the keys go, then
            // the plugins' buttons fold into More ▾. The first that fits,
            // from the words (as select mode's bar does, Main.qml), right
            // of the date and the arc beside it; where not even the least
            // does, it goes under the date, the window's width its room.
            function levelIn(room) {
                for (let level = 0; level < 3; ++level)
                    if (widthAt(level) <= room)
                        return level
                return 3
            }
            readonly property int besideDate: {
                // The date's width a step at a time, so that a photo's
                // longer or shorter date does not refold the bar.
                const step = 3 * theme.fontSize
                const date = Math.ceil((dateChip.width + 6 + dateChip.height) / step) * step
                return levelIn(root.width - 16 - (16 + date + 8))
            }
            readonly property bool below: besideDate > 2
            readonly property int fit: below ? Math.min(2, levelIn(root.width - 32)) : besideDate
            readonly property bool hints: fit < 1
            readonly property bool folded: fit >= 2
            readonly property var pluginGroups: groupsOf(root.pluginActions)
            // Those of `order` below 0 (a favourite's, Live Photos') stand
            // as Pinhole's own buttons do, never folded into More ▾.
            readonly property var standing: pluginGroups.filter(group => group[0].order < 0)
            readonly property var folding: pluginGroups.filter(group => group[0].order >= 0)
            function widthAt(level) {
                const pad = 2 * Math.round(theme.fontSize * 0.833)
                const arrow = viewerMetrics.of("▾") + 2 * Math.round(theme.fontSize * 0.5)
                const key = name => level < 1 && name !== "" ? viewerMetrics.of(" " + name) : 0
                const words = text => viewerMetrics.of(text)
                // Each group's button, ▾ and the hairline before it.
                const groups = []
                const shownOf = list => list.map(group => group.filter(a => a.shown)).filter(group => group.length > 0)
                const button = group => words(group[0].text) + key(KeyNames.nameOf(group[0].key)) + pad + (group.length > 1 ? arrow : 0)
                for (const group of shownOf(standing))
                    groups.push(button(group))
                const shown = shownOf(folding)
                if (level < 2) {
                    for (const group of shown)
                        groups.push(button(group))
                } else if (shown.length > 0) {
                    groups.push(words(qsTr("More") + " ▾") + pad)
                }
                groups.push(Math.max(words(qsTr("Copy")) + (level < 1 ? words("  C") : 0), words("✓ " + qsTr("Copied"))) + pad + arrow)
                groups.push(words("?") + pad)
                groups.push(words("✕") + key("Esc") + pad)
                return 2 + groups.reduce((sum, w) => sum + w, 0) + 5 * (groups.length - 1)
            }
            LabelMetrics { id: viewerMetrics }

            // First the plugins' that stand: the heart (Favourites), Live.
            Repeater {
                model: actionBar.standing
                delegate: PluginGroup {
                    required property var modelData
                    items: modelData
                    keyLayer: "viewer"
                    hints: actionBar.hints
                }
            }
            Repeater {
                model: actionBar.folding
                delegate: PluginGroup {
                    required property var modelData
                    items: modelData
                    keyLayer: "viewer"
                    hints: actionBar.hints
                    visible: primary >= 0 && !actionBar.folded
                }
            }
            // Or all of them in one, where they do not fit.
            MoreGroup {
                visible: actionBar.folded && menuItems.length > 0
                groups: actionBar.folding
                keyLayer: "viewer"
            }
            // Out of the app: Copy answers with a tick on itself; Export as
            // JPEG or MP4, what opens anywhere, or the original.
            MenuGroup {
                menuKey: "Shift+C"
                menuText: qsTr("Copy")
                keyLayer: "viewer"
                section: "share"
                items: [
                    { text: qsTr("Export"), helpText: qsTr("Export JPEG"), key: "E", triggered: () => outgoing.exportTo([viewer.index], true) },
                    { text: qsTr("Export original"), key: "Shift+E", triggered: () => outgoing.exportTo([viewer.index], false) }
                ]
                CopyButton {
                    objectName: "viewerCopy"
                    flat: true
                    photos: () => [viewer.index]
                    key: "C"
                    keyLayer: "viewer"
                    section: "share"
                    hint: actionBar.hints ? "C" : ""
                }
            }
            // The keys and gestures, as the grid's ? button shows them.
            MenuGroup {
                ShellButton {
                    flat: true
                    text: "?"
                    hint: ""
                    action: root.helpAction
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
                    hint: actionBar.hints ? "Esc" : ""
                    onTapped: root.closeViewer()
                }
            }
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
        readonly property bool active: seekHover.hovered || seekArea.pressed || root.justSkipped
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
            keys: ["Shift+M"]
            keyLayer: "viewer"
            onTriggered: root.muted = !root.muted
        }
    }

    // The carousel: the library's thumbnails in a row under the photo (the
    // viewer's row, without the photos hidden from the grid), the shown one
    // in the middle, sliding along as the photos do. Dragging it
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
        // Where the row is, in places: the shown one's, plus how far the
        // photos still have to slide (all the way, for a long step).
        readonly property real position: viewer.place - viewer.stripOffset / viewer.pageWidth
        readonly property int slots: Math.ceil(width / cell) + 3
        readonly property int first: Math.floor(position - width / 2 / cell) - 1
        // The place at a point of the strip.
        function indexAt(x) { return Math.round(position + (x - width / 2) / cell) }
        // The stack the shown photo is in, by its card's photo, or -1.
        readonly property int stack: (root.grid.stacks, viewer.open ? root.grid.stackOf(viewer.index) : -1)
        function stackOf(i) { return i >= 0 ? (root.grid.stacks, root.grid.stackOf(i)) : -1 }
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
                // The place in [first, first + slots) that falls to this slot,
                // and its photo.
                readonly property int place: {
                    const n = carousel.slots
                    return carousel.first + ((index - carousel.first) % n + n) % n
                }
                readonly property int photo: (viewer.count, viewer.photoAt(place))
                // 1 at the middle, 0 a cell or more away.
                readonly property real nearness: Math.max(0, 1 - Math.abs(x + width / 2 - carousel.middle) / carousel.cell)
                readonly property int stack: visible ? carousel.stackOf(photo) : -1
                readonly property bool shownStack: stack >= 0 && stack === carousel.stack
                visible: photo >= 0
                x: carousel.width / 2 + (place - carousel.position) * carousel.cell - carousel.cell / 2
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
                    readonly property bool first: carousel.stackOf(viewer.photoAt(parent.place - 1)) !== parent.stack
                    readonly property bool last: carousel.stackOf(viewer.photoAt(parent.place + 1)) !== parent.stack
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
                pressIndex = viewer.place
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
        // The time, for a moment, as under the pointer.
        skipped.restart()
        return true
    }
    Timer { id: skipped; interval: 1500 }
    readonly property bool justSkipped: skipped.running

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
