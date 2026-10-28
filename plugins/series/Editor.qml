import QtQuick
import Pinhole.Plugin

// One series on a screen of its own, to put it right: every photo of it as
// large as the window lets them all be (scrolling once they would be too
// small to judge), with the photos just before and after it, and in its
// gaps, faint beside them, to add. Each a click or a key: X takes one out,
// A adds one, Space shows one up close, U ungroups, R makes it automatic
// again, G is its GIF or video (Studio, over this). Escape goes back to
// the photo it was opened from.
Item {
    id: editor
    required property var plugin
    required property var studio
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    // Its keys, but for while Studio is over it.
    readonly property bool keys: shown && !studio.shown

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
    onKChanged: if (shown && !editing && k < 0) close()

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
        pinhole.fetch(tile.index, "medium", (path, error) => {
            if (path === "")
                return
            const now = Object.assign({}, editor.sharp)
            now[tile.id] = "file://" + path
            editor.sharp = now
        })
    }

    // `from`: the photo it opens on, in the series or beside it.
    function openSeries(k, from) {
        if (k < 0 || k >= plugin.series.length)
            return
        const p = plugin.series[k]
        anchor = plugin.idOf(p[0])
        current = from >= 0 ? from : p[0]
        looking = false
        shown = true
        grid.showCurrent()
    }
    function close() {
        shown = false
        looking = false
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
                close()
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
        edit(() => plugin.group([tile.index, positions[0]]))
    }
    function ungroup() {
        edit(() => plugin.ungroup([positions[0]]))
        close()
    }
    // The series and the photos in its gaps, as the finder finds them.
    function automatic() { edit(() => plugin.automatic(flow.map(t => t.index))) }
    readonly property bool edited: (plugin.revision, flow.some(t => plugin.isEdited(t.index)))
    function toStudio() {
        if (k >= 0)
            studio.openSeries(k, false)
    }

    // ← → through the photos, ↑ ↓ a row; up close, ← → only.
    function move(delta) {
        const next = Math.max(0, Math.min(tiles.length - 1, at + delta))
        if (next >= 0 && next < tiles.length) {
            current = tiles[next].index
            grid.showCurrent()
        }
    }
    function moveRow(delta) {
        const r = grid.rects[flowAt]
        if (!r)
            return
        const middle = r.x + r.w / 2
        let best = -1, bestRow = -1, bestDistance = Infinity
        for (let n = 0; n < grid.rects.length; ++n) {
            const o = grid.rects[n]
            if (delta < 0 ? o.y >= r.y : o.y <= r.y)
                continue
            // The nearest row that way, then the photo nearest across.
            const row = Math.abs(o.y - r.y)
            if (bestRow >= 0 && row > bestRow + 0.5)
                continue
            const distance = Math.abs(o.x + o.w / 2 - middle)
            if (bestRow < 0 || row < bestRow - 0.5 || distance < bestDistance) {
                best = n
                bestRow = row
                bestDistance = distance
            }
        }
        if (best >= 0) {
            current = flow[best].index
            grid.showCurrent()
        }
    }
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

    Shortcut { enabled: editor.keys; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: editor.looking ? editor.looking = false : editor.close() }
    Shortcut { enabled: editor.keys; sequences: ["Space", "Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: editor.looking = !editor.looking }
    Shortcut { enabled: editor.keys; sequence: "Left"; context: Qt.ApplicationShortcut; autoRepeat: true; onActivated: editor.move(-1) }
    Shortcut { enabled: editor.keys; sequence: "Right"; context: Qt.ApplicationShortcut; autoRepeat: true; onActivated: editor.move(1) }
    Shortcut { enabled: editor.keys && !editor.looking; sequence: "Up"; context: Qt.ApplicationShortcut; onActivated: editor.moveRow(-1) }
    Shortcut { enabled: editor.keys && !editor.looking; sequence: "Down"; context: Qt.ApplicationShortcut; onActivated: editor.moveRow(1) }
    Shortcut { enabled: editor.keys; sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: editor.move(-editor.tiles.length) }
    Shortcut { enabled: editor.keys; sequence: "End"; context: Qt.ApplicationShortcut; onActivated: editor.move(editor.tiles.length) }
    Shortcut { enabled: editor.keys; sequence: "X"; context: Qt.ApplicationShortcut; onActivated: editor.takeOut() }
    Shortcut { enabled: editor.keys; sequence: "A"; context: Qt.ApplicationShortcut; onActivated: editor.add() }
    Shortcut { enabled: editor.keys; sequence: "U"; context: Qt.ApplicationShortcut; onActivated: editor.ungroup() }
    Shortcut { enabled: editor.keys && editor.edited; sequence: "R"; context: Qt.ApplicationShortcut; onActivated: editor.automatic() }
    Shortcut { enabled: editor.keys; sequence: "G"; context: Qt.ApplicationShortcut; onActivated: editor.toStudio() }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }
    // Clicks stay here, off the photo beneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }

    // The series: when, and how many.
    Row {
        id: top
        x: 16
        y: 16
        spacing: 6
        Chip {
            readonly property var date: editor.positions.length > 0 ? new Date(editor.pinhole.library.item(editor.positions[0]).date) : null
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
        onTapped: editor.close()
    }

    // The photos in justified rows, all of one height in a row, the rows
    // as tall as lets every photo show, down to a size still worth judging,
    // then scrolling.
    Flickable {
        id: grid
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: top.bottom
        anchors.bottom: beside.top
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        visible: !editor.looking
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, layout.height + 2 * edge)
        boundsBehavior: Flickable.StopAtBounds

        readonly property real gap: 6
        // Room around the rows for the mark of the photo the keys are on.
        readonly property real edge: 4
        readonly property real room: width - 2 * edge
        readonly property real fit: height - 2 * edge
        readonly property real smallest: Math.max(140, theme.fontSize * 11)
        // Rows at about a height `h`: each broken where its width comes
        // nearest the room there is, so a row grows a little or shrinks a little
        // to fill it, and the last at the height of those above. Their
        // rects, and how tall all.
        function rows(h) {
            const out = []
            let y = 0, start = 0, sum = 0, last = h
            const place = (from, to, height, full) => {
                let used = 0
                for (let n = from; n < to; ++n)
                    used += editor.flow[n].aspect * height
                used += gap * (to - from - 1)
                let x = full ? 0 : (room - used) / 2
                for (let n = from; n < to; ++n) {
                    const w = editor.flow[n].aspect * height
                    out.push({ x: x, y: y, w: w, h: height })
                    x += w + gap
                }
                y += height + gap
            }
            for (let n = 0; n < editor.flow.length; ++n) {
                const aspect = editor.flow[n].aspect
                const count = n - start + 1
                const over = (sum + aspect) * h + gap * (count - 1) - room
                if (over < 0) {
                    sum += aspect
                    continue
                }
                const under = room - sum * h - gap * (count - 2)
                if (count > 1 && under < over) {
                    last = (room - gap * (count - 2)) / sum
                    place(start, n, last, true)
                    start = n
                    sum = aspect
                } else {
                    last = (room - gap * (count - 1)) / (sum + aspect)
                    place(start, n + 1, last, true)
                    start = n + 1
                    sum = 0
                }
            }
            if (start < editor.flow.length)
                place(start, editor.flow.length, Math.min(last, h, (room - gap * (editor.flow.length - start - 1)) / sum), false)
            return { rects: out, height: Math.max(0, y - gap) }
        }
        readonly property var layout: {
            if (room <= 0 || fit <= 0 || editor.flow.length === 0)
                return { rects: [], height: 0 }
            let lo = smallest, hi = Math.max(smallest, fit)
            if (rows(hi).height <= fit)
                return rows(hi)
            if (rows(lo).height > fit)
                return rows(lo)
            for (let n = 0; n < 18; ++n) {
                const mid = (lo + hi) / 2
                if (rows(mid).height <= fit)
                    lo = mid
                else
                    hi = mid
            }
            return rows(lo)
        }
        readonly property var rects: layout.rects
        // Fitting, the rows stand in the middle.
        readonly property real inset: edge + Math.max(0, (fit - layout.height) / 2)

        function showCurrent() { Qt.callLater(() => {
            const r = rects[editor.flowAt]
            if (!r)
                return
            const y = inset + r.y
            if (y < contentY)
                contentY = Math.max(0, y - gap)
            else if (y + r.h > contentY + height)
                contentY = Math.min(contentHeight - height, y + r.h - height + gap)
        }) }

        Repeater {
            model: editor.flow
            delegate: Item {
                id: tile
                required property var modelData
                required property int index
                readonly property var r: grid.rects[index] || { x: 0, y: 0, w: 0, h: 0 }
                readonly property bool isCurrent: editor.current === modelData.index
                x: grid.edge + r.x
                y: grid.inset + r.y
                width: r.w
                height: r.h
                // Bigger than the thumbnail is sharp: the medium size.
                readonly property bool large: height * Screen.devicePixelRatio > 360
                onLargeChanged: if (large) editor.sharpen(modelData)
                Component.onCompleted: if (large) editor.sharpen(modelData)

                Item {
                    anchors.fill: parent
                    opacity: tile.modelData.member ? 1 : hover.hovered || tile.isCurrent ? 0.7 : 0.35
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                    Image {
                        anchors.fill: parent
                        source: tile.modelData.thumbnail
                        sourceSize.height: Math.min(480, Math.ceil(tile.height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Image {
                        id: sharpImage
                        anchors.fill: parent
                        visible: tile.large
                        source: tile.large ? editor.sharp[tile.modelData.id] || "" : ""
                        sourceSize.height: Math.ceil(tile.height * Screen.devicePixelRatio)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 160 } }
                    }
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
                    visible: !tile.modelData.member && tile.width > width + 16
                    x: 8
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 8
                    text: editor.spoken(tile.modelData.apart)
                textColor: theme.darkForeground
                }
                // What a click on the photo's corner does, and its key.
                ShellButton {
                    visible: hover.hovered && tile.width > width + 16 && tile.height > height + 16
                    floating: true
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    text: tile.modelData.member ? qsTr("Take out") : qsTr("Add")
                    hint: tile.modelData.member ? "X" : "A"
                    onTapped: {
                        editor.current = tile.modelData.index
                        tile.modelData.member ? editor.takeOut() : editor.add()
                    }
                }
                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: if (hovered) editor.current = tile.modelData.index
                }
                TapHandler {
                    onTapped: {
                        editor.current = tile.modelData.index
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
        anchors.fill: grid
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
                onTapped: editor.takeOut()
            }
            ShellButton {
                flat: true
                text: qsTr("Add")
                hint: "A"
                usable: editor.currentTile !== null && !editor.currentTile.member
                onTapped: editor.add()
            }
            ShellButton {
                flat: true
                text: editor.looking ? qsTr("All photos") : qsTr("Up close")
                hint: "Space"
                onTapped: editor.looking = !editor.looking
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
                onTapped: editor.ungroup()
            }
            ShellButton {
                flat: true
                visible: editor.edited
                text: qsTr("Automatic")
                hint: "R"
                onTapped: editor.automatic()
            }
            ShellButton {
                active: true
                text: qsTr("GIF or video")
                hint: "G"
                onTapped: editor.toStudio()
            }
        }
    }
}
