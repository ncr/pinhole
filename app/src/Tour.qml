import QtQuick
import Pinhole

// A plugin's tour (PinholePlugin.tour): a few steps on a card, each over
// the photos and buttons as they are, the one it is about lit through a
// dimmed window. A step may name a photo (the grid shows it, its card lit),
// a photo to open full window, one of the plugin's actions (its button
// lit), or its settings (the side panel open on them, lit). → and Enter go on, ← back, Esc or a click beside the card ends it,
// after which it shows by itself no more.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160 } }

    required property var grid
    property var photoViewer: null
    required property var side

    property bool shown: false
    property string pluginId
    property string pluginName
    property var steps: []
    property int index: 0
    readonly property var step: steps[index] ?? ({})
    readonly property bool last: index === steps.length - 1

    // What the step lights, as it resolved on coming to it.
    property int photo: -1
    property var action: null
    property var panelPart: null
    // The tour opened the viewer, or the side panel, so it closes it again.
    // A viewer open before it (not one shrinking away) stays, back on the
    // photo it showed (ownPhoto) if a step showed another.
    property bool opened: false
    property bool openedSide: false
    property int ownPhoto: -1
    // Open as it will stay: not shrinking away, which takes a moment.
    readonly property bool viewerUp: viewer.open && !(photoViewer?.closing ?? false)
    function closeViewer() {
        // Before the viewer is made (Main.qml), nothing shows to shrink.
        if (photoViewer)
            photoViewer.closeViewer()
        else
            viewer.close()
    }

    function start(id, name, list) {
        pluginId = id
        pluginName = name
        // Its last step, where the side panel shows it again, unless one of
        // its own already goes there (API 15: no plugin says it itself).
        const own = Array.from(list)
        if (!own.some(step => step.settings === true))
            own.push({ title: qsTr("Settings"), text: qsTr("Here, in the side panel (Tab), this tour can be shown again."),
                       settings: true })
        steps = own
        jump = true
        shown = true
        enter(0)
    }
    function close() {
        if (!shown)
            return
        if (opened && viewerUp)
            closeViewer()
        else if (!opened && ownPhoto >= 0 && viewerUp && viewer.index !== ownPhoto)
            viewer.openAt(ownPhoto, grid.rectOf(ownPhoto))
        opened = false
        ownPhoto = -1
        if (openedSide)
            side.shown = false
        openedSide = false
        side.keptInView = ""
        panelPart = null
        shown = false
        plugins.setToured(pluginId)
    }
    function go(by) {
        if (index + by >= steps.length)
            close()
        else if (index + by >= 0)
            enter(index + by)
    }
    function enter(k) {
        index = k
        const s = steps[k]
        const value = v => typeof v === "function" ? v() : v
        const open = s.open !== undefined ? value(s.open) : -1
        photo = s.photo !== undefined ? value(s.photo) : -1
        action = s.action ?? null
        if (open >= 0) {
            if (!viewerUp || viewer.index !== open) {
                // Closed, or on its way: the tour's to close again.
                if (!viewerUp)
                    opened = true
                else if (!opened && ownPhoto < 0)
                    ownPhoto = viewer.index
                grid.reveal(open)
                viewer.openAt(open, grid.rectOf(open))
            }
        } else if (opened && viewerUp) {
            closeViewer()
            opened = false
        }
        if (photo >= 0 && open < 0)
            grid.reveal(photo)
        if (s.settings === true) {
            side.shown = true
            openedSide = true
            side.keptInView = pluginId
            panelPart = side.showPlugin(pluginId)
        } else {
            if (openedSide)
                side.shown = false
            openedSide = false
            side.keptInView = ""
            panelPart = null
        }
        fadeIn.restart()
        moving.restart()
    }

    // The lit rectangle, as it should be now: its part of the side panel,
    // the action's button where it shows, else the photo's card in the
    // grid, else none.
    function target() {
        if (panelPart) {
            const p = panelPart.mapToItem(root, 0, 0)
            return Qt.rect(p.x, p.y, panelPart.width, panelPart.height)
        }
        const b = action ? action.button : null
        if (b && b.visible && b.opacity > 0 && b.width > 0) {
            const p = b.mapToItem(root, 0, 0)
            return Qt.rect(p.x, p.y, b.width, b.height)
        }
        if (photo >= 0 && !(photoViewer?.shown ?? false)) {
            const r = grid.rectOf(photo)
            if (r.width > 0 && r.y + r.height > 0 && r.y < height)
                return r
        }
        return null
    }

    // The light and the card glide to where the step has them, following
    // what moves under them (the grid scrolling, the viewer opening) while
    // `moving` runs and until they are there; in the side panel, all along,
    // as it fills in.
    readonly property int pad: Math.round(theme.fontSize * 0.4)
    readonly property int margin: 16
    property bool jump: false
    property bool lit: false
    property real hx: width / 2
    property real hy: height / 2
    property real hw: 0
    property real hh: 0
    property real cx: (width - card.width) / 2
    property real cy: (height - card.height) / 2
    property bool arrived: true
    Timer { id: moving; interval: 1500 }
    onWidthChanged: moving.restart()
    onHeightChanged: moving.restart()
    // A button that shows only later (Info's map, once the photo's place
    // is read) is followed to when it does.
    readonly property bool actionShown: {
        const b = action ? action.button : null
        return !!b && b.visible && b.opacity > 0 && b.width > 0
    }
    onActionShownChanged: if (shown) moving.restart()
    // So is one that moves once there: the viewer's bar folding its
    // plugins into More ▾ as Info's map comes (in Polish, at 850 wide, the
    // bar under the long date), or wrapping under it. Looked at four times
    // a second while all is still, and followed again when it is not where
    // the light is.
    Timer {
        interval: 250
        repeat: true
        running: root.shown && root.arrived && !moving.running
        onTriggered: {
            const t = root.target()
            const away = t ? Math.abs(t.x - root.pad - root.hx) + Math.abs(t.y - root.pad - root.hy)
                             + Math.abs(t.width + 2 * root.pad - root.hw) + Math.abs(t.height + 2 * root.pad - root.hh)
                           : root.hw + root.hh
            if (away > 1)
                moving.restart()
        }
    }
    FrameAnimation {
        running: root.shown && (moving.running || !root.arrived || root.panelPart !== null)
        onTriggered: root.follow(frameTime)
    }
    function follow(dt) {
        const t = target()
        lit = t !== null
        // Without a target the light closes where it was.
        const want = t ? Qt.rect(t.x - pad, t.y - pad, t.width + 2 * pad, t.height + 2 * pad)
                       : Qt.rect(hx + hw / 2, hy + hh / 2, 0, 0)
        // The card right of what is lit, else below it, else above, else in
        // the middle.
        const gap = Math.round(theme.fontSize * 0.8)
        let x = (width - card.width) / 2, y = (height - card.height) / 2
        if (t) {
            if (want.x + want.width + gap + card.width <= width - margin) {
                x = want.x + want.width + gap
                y = Math.max(margin, Math.min(height - card.height - margin, want.y))
            } else {
                x = Math.max(margin, Math.min(width - card.width - margin, want.x + want.width / 2 - card.width / 2))
                if (want.y + want.height + gap + card.height <= height - margin)
                    y = want.y + want.height + gap
                else if (want.y - gap - card.height >= margin)
                    y = want.y - gap - card.height
            }
        }
        const k = jump ? 1 : 1 - Math.exp(-dt * 14)
        jump = false
        hx += (want.x - hx) * k
        hy += (want.y - hy) * k
        hw += (want.width - hw) * k
        hh += (want.height - hh) * k
        cx += (x - cx) * k
        cy += (y - cy) * k
        arrived = Math.abs(want.x - hx) + Math.abs(want.y - hy) + Math.abs(want.width - hw) + Math.abs(want.height - hh)
                  + Math.abs(x - cx) + Math.abs(y - cy) < 0.5
    }

    // The window dimmed, as under Omarchy's menus, but for the lit hole: a
    // frame so wide its outside never shows, its inside rounded as a card.
    readonly property real far: Math.max(width, height) * 2
    readonly property real corner: Math.min(4, theme.radius) + pad
    Rectangle {
        x: root.hx - root.far
        y: root.hy - root.far
        width: root.hw + 2 * root.far
        height: root.hh + 2 * root.far
        radius: root.far + Math.min(root.corner, root.hw / 2, root.hh / 2)
        color: "transparent"
        border.width: root.far
        border.color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.6)
    }
    Rectangle {
        x: root.hx
        y: root.hy
        width: root.hw
        height: root.hh
        radius: Math.min(root.corner, root.hw / 2, root.hh / 2)
        color: "transparent"
        border.color: theme.accent
        border.width: 2
        opacity: root.lit ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
    }

    // A click beside the card ends it; the photos under it take nothing.
    Shield {
        anchors.fill: parent
        onClicked: root.close()
    }

    Rectangle {
        id: card
        x: Math.round(root.cx)
        y: Math.round(root.cy)
        width: Math.min(Math.round(theme.fontSize * 26), root.width - 2 * root.margin)
        height: content.implicitHeight + 2 * padding
        readonly property int padding: Math.round(theme.fontSize * 1.2)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1

        Shield { anchors.fill: parent }

        Column {
            id: content
            x: card.padding
            y: card.padding
            width: card.width - 2 * card.padding
            spacing: Math.round(theme.fontSize * 0.5)
            NumberAnimation on opacity { id: fadeIn; from: 0; to: 1; duration: 180 }

            UiText {
                text: root.pluginName
                color: theme.accent
                font.pixelSize: Math.round(theme.fontSize * 0.857)
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.step.title ?? ""
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.286)
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.step.text ?? ""
                lineHeight: 1.15
            }
            Item { width: 1; height: Math.round(theme.fontSize * 0.3) }
            Item {
                width: parent.width
                height: next.height
                // Where in the tour, a dot a step.
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Math.round(theme.fontSize * 0.4)
                    visible: root.steps.length > 1
                    Repeater {
                        model: root.steps.length
                        delegate: Rectangle {
                            required property int index
                            width: Math.round(theme.fontSize * 0.45)
                            height: width
                            radius: width / 2
                            color: index === root.index ? theme.accent
                                 : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.3)
                            Behavior on color { ColorAnimation { duration: 160 } }
                        }
                    }
                }
                Row {
                    anchors.right: parent.right
                    spacing: 6
                    ShellButton {
                        visible: root.index > 0
                        flat: true
                        text: qsTr("Back")
                        action: backAction
                    }
                    ShellButton {
                        id: next
                        active: true
                        text: root.last ? qsTr("Done") : qsTr("Next")
                        action: nextAction
                    }
                }
            }
        }
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        id: nextAction
        name: "tour.next"
        keys: ["Right", "Return", "Enter", "Space"]
        keysText: root.last ? "⏎" : "→"
        keyLayer: "tour"
        onTriggered: root.go(1)
    }
    Command {
        id: backAction
        name: "tour.back"
        keys: ["Left"]
        keysText: "←"
        keyLayer: "tour"
        when: root.index > 0
        onTriggered: root.go(-1)
    }
    Command {
        name: "tour.close"
        keys: ["Escape"]
        keyLayer: "tour"
        mouse: qsTr("Click beside the card")
        touchpad: qsTr("Tap beside the card")
        onTriggered: root.close()
    }
}
