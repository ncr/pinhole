import QtQuick
import Pinhole.Plugin

// One series on a screen of its own, to put it right: three of its photos
// side by side in the middle, as large as the window lets them be, the rest
// stacked behind them on either side as in a carousel, and the photos just
// before and after it, and in its gaps, faint beside them, to add. Each a click or a key: X takes one out,
// A adds one, Space shows one up close, U ungroups, G is its GIF or video
// (Studio, over this); Ctrl+Z undoes an edit. Escape goes back to the
// photo it was opened from. Shown in its PluginView (Main.qml), which
// start()s it on a series.
Item {
    id: editor
    objectName: "seriesEditor"
    // Its PluginView's, and the plugin's root through it.
    readonly property Item plugin: view.plugin
    readonly property bool shown: view.shown
    // Its keys, but for while Studio is over it.
    readonly property bool keys: view.onTop

    // The series, followed by one of its photos (its place among the
    // series moves as they change), and the photo the keys act on.
    property string anchor: ""
    property int current: -1
    // One photo up close.
    property bool looking: false
    readonly property int k: (plugin.revision, anchor !== "" ? plugin.of(pinhole.library.indexOf(anchor)) : -1)
    readonly property var positions: k >= 0 ? plugin.series[k] : []
    // An edit under way: the series may be gone for a moment.
    property bool editing: false
    onKChanged: if (shown && !editing && k < 0) view.close()

    // Photos the series could grow into: this many on each side, taken
    // within this long of it, and the ones in a gap of it up to this wide.
    readonly property int near: 4
    readonly property real reach: 10 * 60 * 1000
    readonly property int widestGap: 6

    // Every photo shown, in the library's order: {index, member, aspect,
    // thumbnail, id, date, apart (how far in time from the series, ms; 0
    // in it and in its gaps)}. The series and its gaps are the screen's;
    // the photos before and after it, along its foot.
    readonly property var tiles: {
        const p = positions
        if (p.length === 0)
            return []
        const count = pinhole.library.count
        const first = pinhole.library.item(p[0]).date, last = pinhole.library.item(p[p.length - 1]).date
        const free = i => plugin.of(i) < 0 && !pinhole.library.item(i).video
        const near = i => Math.abs(pinhole.library.item(i).date - (i < p[0] ? first : last)) <= reach
        const indexes = []
        const before = []
        for (let i = p[0] - 1; i >= 0 && before.length < editor.near && free(i) && near(i); --i)
            before.unshift(i)
        before.forEach(i => indexes.push(i))
        for (let n = 0; n < p.length; ++n) {
            indexes.push(p[n])
            if (n + 1 < p.length && p[n + 1] - p[n] - 1 <= widestGap)
                for (let i = p[n] + 1; i < p[n + 1]; ++i)
                    if (free(i))
                        indexes.push(i)
        }
        for (let i = p[p.length - 1] + 1, n = 0; i < count && n < editor.near && free(i) && near(i); ++i, ++n)
            indexes.push(i)
        const member = new Set(p)
        return indexes.map(i => {
            const item = pinhole.library.item(i)
            const aspect = item.width > 0 && item.height > 0 ? item.width / item.height : 4 / 3
            return { index: i, member: member.has(i), id: item.id, thumbnail: item.thumbnail, date: item.date,
                     aspect: Math.max(0.4, Math.min(3, aspect)),
                     apart: i < p[0] ? item.date - first : i > p[p.length - 1] ? item.date - last : 0 }
        })
    }
    readonly property var flow: tiles.filter(t => t.apart === 0)
    readonly property var before: tiles.filter(t => t.apart < 0)
    readonly property var after: tiles.filter(t => t.apart > 0)
    readonly property int at: tiles.findIndex(t => t.index === current)
    readonly property int flowAt: flow.findIndex(t => t.index === current)
    readonly property var currentTile: at >= 0 ? tiles[at] : null

    // The sharper pictures, by id, once fetched: kept while the window runs,
    // so a tile drawn again after an edit has its own at once.
    property var sharp: ({})
    function sharpen(tile) {
        if (sharp[tile.id] !== undefined)
            return
        const s = Object.assign({}, sharp)
        s[tile.id] = ""
        sharp = s
        pinhole.fetch(tile.id, "medium").then(path => {
            const now = Object.assign({}, editor.sharp)
            now[tile.id] = "image://file/" + encodeURIComponent(path)
            editor.sharp = now
        }, () => {}).catch(pinhole.warn)
    }

    // As its view opens on series `k`; `from`: the photo it opens on, in
    // the series or beside it.
    function start(k, from) {
        if (k < 0 || k >= plugin.series.length)
            return
        const p = plugin.series[k]
        anchor = plugin.idOf(p[0])
        current = from >= 0 ? from : p[0]
        looking = false
        // It opens with that photo in the middle, without sliding to it.
        sliding.enabled = false
        middle = flowAt
        sliding.enabled = true
    }
    function stop() {
        looking = false
    }
    // Esc up close: back to the series, the view staying open.
    function back() {
        if (!looking)
            return false
        looking = false
        return true
    }

    // The edits are the plugin's own; here the series is only followed
    // through them, by another of its photos when the one followed goes.
    function edit(change) {
        const ids = positions.map(plugin.idOf)
        editing = true
        change()
        editing = false
        if (k < 0) {
            const still = ids.find(id => plugin.of(pinhole.library.indexOf(id)) >= 0)
            if (still === undefined) {
                view.close()
                return
            }
            anchor = still
        }
    }
    function takeOut() {
        const tile = currentTile
        if (!tile || !tile.member)
            return
        edit(() => plugin.takeOut(tile.index))
    }
    function add() {
        const tile = currentTile
        if (!tile || tile.member)
            return
        edit(() => plugin.group([tile.index, positions[0]], qsTr("adding to a series")))
    }
    function ungroup() {
        edit(() => plugin.ungroup([positions[0]]))
        view.close()
    }
    function toStudio() {
        if (k >= 0)
            plugin.openStudio(k, false)
    }

    // ← → through the photos, up close too.
    function move(delta) {
        const next = Math.max(0, Math.min(tiles.length - 1, at + delta))
        if (next >= 0 && next < tiles.length)
            current = tiles[next].index
    }
    // The three in the middle, by the one among them in the middle: they
    // move only when the photo the keys are on goes past them.
    property int middle: 0
    function follow() {
        // On one before or after the series, its end that way.
        if (flowAt < 0) {
            const tile = tiles.find(t => t.index === current)
            if (tile)
                middle = tile.apart < 0 ? 0 : flow.length - 1
        } else if (flowAt > carousel.centre + 1)
            middle = flowAt - 1
        else if (flowAt < carousel.centre - 1)
            middle = flowAt + 1
    }
    onFlowAtChanged: follow()
    onTilesChanged: {
        if (tiles.length > 0 && at < 0) {
            // The photo the keys were on left the screen: the nearest that
            // stays.
            let nearest = tiles[0].index
            for (const t of tiles)
                if (Math.abs(t.index - current) < Math.abs(nearest - current))
                    nearest = t.index
            current = nearest
        }
    }

    // What a photo beside the series is to it: how long before or after,
    // or in a gap of it, not in it.
    function spoken(ms) {
        const s = Math.round(Math.abs(ms) / 1000)
        const span = s < 60 ? qsTr("%1 s").arg(s) : s < 3600 ? qsTr("%1 min").arg(Math.round(s / 60)) : qsTr("%1 h").arg(Math.round(s / 3600))
        return ms === 0 ? qsTr("Not in the series") : ms < 0 ? qsTr("%1 before").arg(span) : qsTr("%1 after").arg(span)
    }

    // Its keys, while it is on top (Command.qml); Esc is its view's.
    Command {
        name: "editor.look"
        text: qsTr("There: a photo up close")
        section: "Series"
        keys: ["Space", "Return", "Enter"]
        keysText: "Space"
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        onTriggered: editor.looking = !editor.looking
    }
    Command {
        name: "editor.previous"
        text: qsTr("Previous, next photo")
        section: "Series"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        usable: editor.keys
        autoRepeat: true
        mouse: qsTr("Wheel, or point")
        touchpad: qsTr("Two fingers, or point")
        onTriggered: editor.move(-1)
    }
    Command {
        name: "editor.next"
        keys: ["Right"]
        keyLayer: "plugin"
        usable: editor.keys
        autoRepeat: true
        mouse: qsTr("Wheel, or point")
        touchpad: qsTr("Two fingers, or point")
        onTriggered: editor.move(1)
    }
    Command {
        name: "editor.first"
        keys: ["Home"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: editor.move(-editor.tiles.length)
    }
    Command {
        name: "editor.last"
        keys: ["End"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: editor.move(editor.tiles.length)
    }
    Command {
        name: "editor.takeOut"
        text: qsTr("Out of its series")
        section: "Series"
        keys: ["X"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: editor.takeOut()
    }
    Command {
        name: "editor.add"
        text: qsTr("Into the series beside it")
        section: "Series"
        keys: ["A"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: editor.add()
    }
    Command {
        name: "editor.ungroup"
        text: qsTr("Ungroup")
        section: "Series"
        keys: ["U"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: editor.ungroup()
    }
    Command {
        name: "editor.studio"
        text: qsTr("Series: a GIF or a video")
        section: "Series"
        keys: ["G"]
        keyLayer: "plugin"
        usable: editor.keys
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: editor.toStudio()
    }

    // Nothing reaches the photos beneath; the wheel, either way, goes
    // through the series, a photo a notch, and a swipe a photo.
    Panel {
        steps: Qt.Horizontal | Qt.Vertical
        onStepped: step => editor.move(step)
    }

    // The series: when, and how many.
    Row {
        id: top
        x: 16
        y: 16
        spacing: 6
        Chip {
            readonly property var date: editor.positions.length > 0 ? new Date(pinhole.library.item(editor.positions[0]).date) : null
            text: date ? date.toLocaleDateString(Qt.locale(), Locale.LongFormat) + " <font color=\"" + theme.darkForeground + "\">· "
                         + date.toLocaleTimeString(Qt.locale(), Locale.ShortFormat) + "</font>" : ""
            textColor: theme.brightForeground
        }
        Chip {
            text: qsTr("Series of %1").arg(Number(editor.positions.length).toLocaleString(Qt.locale(), "f", 0))
            textColor: theme.darkForeground
        }
    }
    ShellButton {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        hint: "Esc"
        onTriggered: view.close()
    }

    // The series as a carousel: three photos side by side in the middle,
    // as big as they fit, the one the keys are on among them; the rest
    // stacked behind them on either side, smaller and dimmer the further
    // they are, sliding in as the keys or the wheel go on.
    Item {
        id: carousel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: top.bottom
        anchors.bottom: beside.top
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        visible: !editor.looking

        readonly property int count: editor.flow.length
        readonly property real centre: count < 3 ? (count - 1) / 2 : Math.max(1, Math.min(count - 2, editor.middle))
        property real slide: centre
        Behavior on slide {
            id: sliding
            NumberAnimation { id: slideAnimation; duration: 280; easing.type: Easing.OutCubic }
        }

        readonly property real gap: 10
        // The series' usual shape: a box each photo fits in.
        readonly property real aspect: {
            const a = editor.flow.filter(t => t.member).map(t => t.aspect).sort((x, y) => x - y)
            return a.length > 0 ? a[Math.floor(a.length / 2)] : 4 / 3
        }
        // Room on each side for the stacks, when there is more than the three.
        readonly property bool stacks: count > 3
        readonly property real box: Math.max(0, Math.min((width - (stacks ? 0.2 * width : 0) - 2 * gap) / 3, height * aspect))
        readonly property real tall: box / aspect
        readonly property real wing: stacks ? Math.min((width - 3 * box - 2 * gap) / 2, 0.45 * box) : 0

        // Where a photo `p` places from the middle one goes: within one of
        // it, side by side; past that, `d` deep in its stack, each a little
        // smaller, showing a little less of itself beyond the one before.
        function place(p, a) {
            const d = Math.max(0, Math.abs(p) - 1)
            const shrink = Math.pow(0.82, d)
            const step = box + gap
            const off = Math.abs(p) <= 1 ? Math.abs(p) * step
                                         : step + box / 2 + wing * (1 - Math.pow(0.55, d)) - shrink * box / 2
            const w0 = Math.min(box, tall * a)
            const w = w0 * shrink, h = w0 / a * shrink
            const middle = width / 2 + (p < 0 ? -off : off)
            return { x: middle - w / 2, y: (height - h) / 2, w: w, h: h, d: d }
        }

        Repeater {
            model: editor.flow
            delegate: Item {
                id: tile
                required property var modelData
                required property int index
                readonly property var r: carousel.place(index - carousel.slide, modelData.aspect)
                // One of the three in the middle, where the photo stops.
                readonly property bool middle: Math.abs(index - carousel.centre) <= 1
                readonly property bool isCurrent: editor.current === modelData.index
                x: r.x
                y: r.y
                width: r.w
                height: r.h
                z: -r.d
                visible: r.d < 5
                opacity: Math.min(1, 5 - r.d)
                // Bigger than the thumbnail is sharp: the medium size.
                readonly property bool large: height * Screen.devicePixelRatio > 360
                onLargeChanged: if (large) editor.sharpen(modelData)
                Component.onCompleted: if (large) editor.sharpen(modelData)

                Image {
                    anchors.fill: parent
                    source: tile.modelData.thumbnail
                    sourceSize.height: Math.min(480, Math.ceil(carousel.tall * Screen.devicePixelRatio))
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Image {
                    anchors.fill: parent
                    visible: tile.large
                    source: tile.large ? editor.sharp[tile.modelData.id] || "" : ""
                    sourceSize.height: Math.ceil(carousel.tall * Screen.devicePixelRatio)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                }
                // Dimmer deeper in a stack, and faint when not in the
                // series; opaque, so a stack never shows through itself.
                Rectangle {
                    anchors.fill: parent
                    color: theme.background
                    readonly property real depth: Math.min(0.7, tile.r.d * 0.28)
                    property real out: tile.modelData.member ? 0 : hover.hovered || tile.isCurrent ? 0.3 : 0.65
                    opacity: 1 - (1 - depth) * (1 - out)
                    Behavior on out { NumberAnimation { duration: 120 } }
                }
                // The photo the keys act on.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    color: "transparent"
                    radius: Math.min(4, theme.radius) + 2
                    border.color: theme.accent
                    border.width: 2
                    visible: tile.isCurrent
                }
                // In a gap of the series, not in it.
                Chip {
                    visible: tile.middle && !tile.modelData.member && tile.width > width + 16
                    x: 8
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 8
                    text: editor.spoken(tile.modelData.apart)
                    textColor: theme.darkForeground
                }
                // What a click on the photo's corner does, and its key.
                ShellButton {
                    visible: tile.middle && hover.hovered && tile.width > width + 16 && tile.height > height + 16
                    floating: true
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    text: tile.modelData.member ? qsTr("Take out") : qsTr("Add")
                    hint: tile.modelData.member ? "X" : "A"
                    onTriggered: {
                        editor.current = tile.modelData.index
                        tile.modelData.member ? editor.takeOut() : editor.add()
                    }
                }
                // The pointer picks one of the three; not while they slide
                // under it.
                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: if (hovered && tile.middle && !slideAnimation.running) editor.current = tile.modelData.index
                }
                // A click on one of the three shows it up close; on one in a
                // stack, brings it to the middle.
                TapHandler {
                    onTapped: {
                        editor.current = tile.modelData.index
                        if (tile.middle)
                            editor.looking = true
                    }
                }
            }
        }
    }

    // One photo up close, as big as fits; a click or Space goes back to
    // them all.
    Item {
        id: closeUp
        anchors.fill: carousel
        visible: editor.looking && editor.currentTile !== null
        readonly property var tile: editor.currentTile
        onTileChanged: if (tile && editor.looking) editor.sharpen(tile)
        onVisibleChanged: if (visible && tile) editor.sharpen(tile)
        Item {
            anchors.centerIn: parent
            width: closeUp.tile ? Math.min(parent.width, parent.height * closeUp.tile.aspect) : 0
            height: closeUp.tile ? width / closeUp.tile.aspect : 0
            Image {
                anchors.fill: parent
                source: closeUp.tile ? closeUp.tile.thumbnail : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Image {
                anchors.fill: parent
                source: closeUp.tile ? editor.sharp[closeUp.tile.id] || "" : ""
                sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 160 } }
            }
            Chip {
                visible: closeUp.tile !== null && !closeUp.tile.member
                x: 10
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 10
                text: closeUp.tile ? editor.spoken(closeUp.tile.apart) : ""
            }
        }
        TapHandler { onTapped: editor.looking = false }
    }

    // One photo beside it, along the foot.
    component Neighbour: Item {
        id: neighbour
        required property var modelData
        readonly property bool isCurrent: editor.current === modelData.index
        readonly property real picture: Math.round(beside.cell * Math.min(1.5, modelData.aspect))
        width: Math.max(picture, when.implicitWidth + 8)
        height: beside.height
        Image {
            id: thumb
            anchors.horizontalCenter: parent.horizontalCenter
            width: neighbour.picture
            height: beside.cell
            source: neighbour.modelData.thumbnail
            sourceSize.height: beside.cell * 2
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            opacity: hover.hovered || neighbour.isCurrent ? 1 : 0.45
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }
        Rectangle {
            width: thumb.width + 6
            height: beside.cell + 6
            x: thumb.x - 3
            y: -3
            color: "transparent"
            radius: Math.min(4, theme.radius) + 2
            border.color: theme.accent
            border.width: 2
            visible: neighbour.isCurrent
        }
        UiText {
            id: when
            anchors.horizontalCenter: parent.horizontalCenter
            y: beside.cell + 6
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            color: neighbour.isCurrent ? theme.foreground : theme.darkForeground
            text: editor.spoken(neighbour.modelData.apart)
        }
        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
            onHoveredChanged: if (hovered) editor.current = neighbour.modelData.index
        }
        TapHandler {
            onTapped: {
                editor.current = neighbour.modelData.index
                editor.looking = true
            }
        }
    }
    // The photos just before and after the series, small, each with how
    // far from it in time: what it could grow into.
    Item {
        id: beside
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: card.top
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: visible ? 14 : 0
        visible: editor.before.length + editor.after.length > 0
        height: visible ? cell + label + 6 : 0
        readonly property real cell: Math.round(theme.fontSize * 5.5)
        readonly property real label: Math.round(theme.fontSize * 1.6)
        Row {
            anchors.left: parent.left
            spacing: 8
            Repeater {
                model: editor.before
                delegate: Neighbour {}
            }
        }
        Row {
            anchors.right: parent.right
            spacing: 8
            Repeater {
                model: editor.after
                delegate: Neighbour {}
            }
        }
    }

    // What can be done, on the photo the keys are on and on the series:
    // as an Omarchy menu card, as in Studio.
    Rectangle {
        id: card
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: actions.width + 2
        height: actions.height + 2
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        Row {
            id: actions
            x: 1
            y: 1
            ShellButton {
                flat: true
                text: qsTr("Take out")
                hint: "X"
                usable: editor.currentTile !== null && editor.currentTile.member
                onTriggered: editor.takeOut()
            }
            ShellButton {
                flat: true
                text: qsTr("Add")
                hint: "A"
                usable: editor.currentTile !== null && !editor.currentTile.member
                onTriggered: editor.add()
            }
            ShellButton {
                flat: true
                text: editor.looking ? qsTr("All photos") : qsTr("Up close")
                hint: "Space"
                onTriggered: editor.looking = !editor.looking
            }
            Rectangle {
                width: 1
                height: parent.height
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
            }
            ShellButton {
                flat: true
                text: qsTr("Ungroup")
                hint: "U"
                onTriggered: editor.ungroup()
            }
            ShellButton {
                active: true
                text: qsTr("GIF or video")
                hint: "G"
                onTriggered: editor.toStudio()
            }
        }
    }
}
