import QtQuick
import QtQuick.Window
import Pinhole.Plugin
import "Colour.js" as Colour

// The colour finder, over the window: a colour picked on a wheel (its hue
// round it, how strong out from the grey middle) and its lightness on a
// strip under it, and the photos closest to it beside them, closest
// first. Each dot on the wheel is a photo, at its own colour: where the
// library's colours are. Or the rainbow wall (Wall.qml). A photo clicked
// opens full window; closing it comes back here.
Item {
    id: finder
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown && !pinhole.away ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // "find" or "wall".
    property string mode: "find"
    // The colour looked for, OKLab, and the photo it is from, if any (its
    // id, and where it is now).
    property real targetL: 0.62
    property real targetA: 0.09
    property real targetB: 0.07
    property string fromId: ""
    readonly property int from: (plugin.revision, fromId !== "" ? pinhole.library.indexOf(fromId) : -1)
    readonly property real targetC: Colour.chroma(targetA, targetB)
    // The hue, kept while the colour is grey, so going grey and back out
    // with the keys comes back to the hue it went in from.
    property real hue: 38
    readonly property real targetH: targetC >= 0.004 ? Colour.hue(targetA, targetB) : hue
    readonly property string targetHex: Colour.hex(targetL, targetA, targetB)
    // How far out the wheel goes: chroma at its rim. A photo's average is
    // rarely past 0.15, and most are under 0.05, so the wheel is drawn by
    // the square root of chroma, giving the middle the room.
    readonly property real rimChroma: 0.22
    readonly property int resultCount: 300
    // The photos found, closest first, by id.
    property var results: []

    function openFinder() {
        plugin.ensure()
        mode = "find"
        fromId = ""
        shown = true
        search()
        photos.positionViewAtBeginning()
    }
    // The photos closest to this one's colour.
    function like(index) {
        const c = plugin.colourOf(index)
        if (!c)
            return
        plugin.ensure()
        targetL = c[0]
        targetA = c[1]
        targetB = c[2]
        if (targetC >= 0.004)
            hue = targetH
        fromId = pinhole.library.item(index).id
        mode = "find"
        shown = true
        search()
        photos.positionViewAtBeginning()
    }
    function close() {
        shown = false
    }
    // A photo found, by its id.
    function open(id) {
        const index = pinhole.library.indexOf(id)
        if (index < 0)
            return
        // ← → through what was found here (the wall's photos, or those
        // nearest the colour); back here when it closes (onReturned).
        const among = (mode === "wall" ? wall.tiles.filter(tile => tile !== "") : results).filter(found => !pinhole.isHidden(found))
        pinhole.openPhotos(among.indexOf(id) >= 0 ? among : [id], id, { comeBack: true })
    }
    // The same photos found again (opened again on the same colour) leave
    // the grid as it is: a list given anew makes every tile again.
    function search() {
        const d = plugin.ensure()
        const found = Colour.nearest(d, targetL, targetA, targetB, resultCount, fromId)
        if (found.length !== results.length || found.some((id, i) => id !== results[i]))
            results = found
    }
    // The photos found that are still in the library; looked for again if
    // none are (another source shown, say).
    function keepFound() {
        const kept = results.filter(id => pinhole.library.indexOf(id) >= 0 && !pinhole.isHidden(id))
        if (kept.length === 0)
            search()
        else if (kept.length !== results.length)
            results = kept
    }
    // Photos hidden from the grid or shown again: found again, the scroll
    // kept. With the library read again since, its colours are not read
    // again until it next opens (as for any change): what is hidden now
    // only leaves.
    function hiddenMoved() {
        if (shown && mode === "find" && plugin.colours) {
            const found = plugin.stale ? results.filter(id => !pinhole.isHidden(id))
                : Colour.nearest(plugin.colours, targetL, targetA, targetB, resultCount, fromId)
            if (found.length !== results.length || found.some((id, i) => id !== results[i])) {
                const y = photos.contentY
                results = found
                photos.contentY = Math.max(0, Math.min(y, photos.contentHeight - photos.height))
            }
        }
        wall.hiddenMoved()
    }
    // Picked by hand: no longer a photo's.
    function pick(L, a, b) {
        targetL = Math.max(0.05, Math.min(0.98, L))
        targetA = a
        targetB = b
        if (Colour.chroma(a, b) >= 0.004)
            hue = Colour.hue(a, b)
        fromId = ""
        if (!searching.running)
            searching.start()
    }
    function pickPolar(C, h) {
        hue = (h + 360) % 360
        const p = Colour.fromPolar(targetL, Math.max(0, Math.min(rimChroma, C)), hue)
        pick(targetL, p[1], p[2])
    }
    // The keys' steps (FinderKeys.qml): round the wheel, out from its grey
    // middle or in, lighter or darker.
    function turn(degrees) { pickPolar(Math.max(targetC, 0.04), targetH + degrees) }
    function strengthen(step) {
        const r = Math.max(0, Math.min(1, Math.sqrt(targetC / rimChroma) + step))
        pickPolar(r * r * rimChroma, targetH)
    }
    function lighten(step) { pick(targetL + step, targetA, targetB) }
    // The wall's tiles a step bigger.
    readonly property bool canGrow: wall.density < wall.densities.length - 1
    function grow() {
        if (canGrow)
            wall.zoomBy(1)
    }
    // While a finger drags on the wheel or a key is held, the photos follow
    // some fifteen times a second (a search is some 10 ms), each time for
    // the colour as it is then.
    Timer { id: searching; interval: 60; onTriggered: finder.search() }

    Connections {
        target: finder.pinhole
        // As it was left: the photos found stay, by their ids, and the
        // library's colours are read again if it has changed, for what is
        // looked for next.
        function onReturned(photo) {
            finder.plugin.ensure()
            finder.keepFound()
            wall.keepFound()
        }
        // The plugin over it handed a photo over: closed for good.
        function onDismissed() { finder.close() }
    }

    // Nothing over it reaches the photos beneath.
    Panel {}

    Row {
        id: top
        x: 16
        y: 16
        spacing: 6
        ShellButton {
            text: qsTr("Find")
            active: finder.mode === "find"
            key: finder.mode === "wall" ? "W" : ""
            keyLayer: "plugin"
            hint: finder.mode === "wall" ? "W" : ""
            onTapped: {
                finder.mode = "find"
                finder.search()
            }
        }
        ShellButton {
            text: qsTr("Rainbow wall")
            active: finder.mode === "wall"
            key: finder.mode === "find" ? "W" : ""
            keyLayer: "plugin"
            hint: finder.mode === "find" ? "W" : ""
            onTapped: finder.mode = "wall"
        }
    }
    // The wall's own, at the right before ✕; under the row when the window
    // is too narrow for both.
    readonly property bool narrow: width < top.width + wallControls.width + closeButton.width + 60
    Row {
        id: wallControls
        visible: finder.mode === "wall"
        anchors.right: closeButton.left
        anchors.rightMargin: 6
        y: finder.narrow ? top.y + top.height + 8 : top.y
        spacing: 6
        ShellButton {
            text: qsTr("Another")
            key: "Space"
            keyLayer: "plugin"
            hint: "Space"
            onTapped: wall.another()
        }
        // The text is the key.
        ShellButton {
            text: "−"
            key: "-"
            keyLayer: "plugin"
            hint: ""
            usable: wall.density > 0
            onTapped: wall.zoomBy(-1)
        }
        ShellButton {
            text: "+"
            key: "+"
            keyLayer: "plugin"
            hint: ""
            usable: wall.density < wall.densities.length - 1
            onTapped: wall.zoomBy(1)
        }
    }
    // What is shown, where there is room for it.
    Chip {
        x: top.x + top.width + 6
        y: top.y
        visible: finder.plugin.colours !== null && x + width + 12 <= (wallControls.visible && !finder.narrow ? wallControls.x : closeButton.x)
        textColor: theme.darkForeground
        text: finder.mode === "wall" ? qsTr("%Ln of your most colourful photos, by hue", "", wall.shownCount)
            : finder.from >= 0 ? qsTr("Closest to this photo's colour") : qsTr("Closest to the colour picked")
    }
    ShellButton {
        id: closeButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        hint: "Esc"
        onTapped: finder.close()
    }

    // Finding: the wheel and the strip at the left, the photos beside.
    Item {
        id: findArea
        visible: finder.mode === "find"
        anchors.fill: parent
        anchors.topMargin: top.y + top.height + 16
        anchors.margins: 16

        Column {
            id: picker
            // Room for the ring at the wheel's foot.
            spacing: Math.round(theme.fontSize * 1.6)
            width: wheel.width

            Item {
                id: wheel
                readonly property real size: Math.min(findArea.height - 210, findArea.width * 0.38, 720)
                width: size
                height: size
                readonly property real radius: size / 2
                // Where a colour sits, and the colour at a point.
                function pointOf(a, b) {
                    const C = Colour.chroma(a, b), h = Colour.hue(a, b) * Math.PI / 180
                    const r = Math.sqrt(Math.min(1, C / finder.rimChroma)) * radius
                    return Qt.point(radius + r * Math.cos(h), radius - r * Math.sin(h))
                }
                function pickAt(x, y) {
                    const dx = (x - radius) / radius, dy = (radius - y) / radius
                    const r = Math.min(1, Math.sqrt(dx * dx + dy * dy))
                    finder.pickPolar(r * r * finder.rimChroma, Math.atan2(dy, dx) * 180 / Math.PI)
                }

                // The wheel at the colour's lightness, faint under the
                // dots: a colour past what a screen shows there drawn at
                // the most it can, fainter still. A wedge a degree, each
                // shaded from the grey middle out.
                Canvas {
                    id: disc
                    anchors.fill: parent
                    // Drawn again when the lightness has moved this much.
                    readonly property real lightness: Math.round(finder.targetL * 50) / 50
                    // Fainter once there are dots over it.
                    readonly property bool faint: finder.plugin.colours !== null
                    onLightnessChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onFaintChanged: requestPaint()
                    Connections {
                        target: theme
                        function onChanged() { disc.requestPaint() }
                    }
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const R = width / 2, L = lightness
                        if (R <= 0)
                            return
                        const bg = theme.background
                        const faint = disc.faint ? 0.45 : 1
                        const mix = (c, k) => "rgb(" + Math.round(c[0] * k + bg.r * 255 * (1 - k)) + ","
                            + Math.round(c[1] * k + bg.g * 255 * (1 - k)) + "," + Math.round(c[2] * k + bg.b * 255 * (1 - k)) + ")"
                        const stops = 24
                        for (let d = 0; d < 360; ++d) {
                            const max = Colour.maxChroma(L, d + 0.5)
                            const shade = ctx.createRadialGradient(R, R, 0, R, R, R)
                            for (let i = 0; i <= stops; ++i) {
                                const r = i / stops, C = r * r * finder.rimChroma
                                const p = Colour.fromPolar(L, Math.min(C, max), d + 0.5)
                                const past = Math.min(1, Math.max(0, C - max) / 0.04)
                                shade.addColorStop(r, mix(Colour.rgb(p[0], p[1], p[2]), (1 - 0.5 * past) * faint))
                            }
                            ctx.fillStyle = shade
                            ctx.beginPath()
                            ctx.moveTo(R, R)
                            ctx.arc(R, R, R, -(d + 1.6) * Math.PI / 180, -(d - 0.6) * Math.PI / 180, false)
                            ctx.closePath()
                            ctx.fill()
                        }
                    }
                }
                // The library's photos on the wheel, each at its own
                // colour: gathered into cells, a dot a cell, as big as
                // the cell has photos and in their colour.
                Canvas {
                    id: dots
                    anchors.fill: parent
                    onWidthChanged: requestPaint()
                    Connections {
                        target: finder.plugin
                        function onColoursChanged() { dots.requestPaint() }
                    }
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const data = finder.plugin.colours
                        const R = width / 2
                        if (!data || R <= 0)
                            return
                        const cells = Math.max(24, Math.round(width / 6))
                        const size = width / cells
                        const count = new Float64Array(cells * cells), sum = new Float64Array(cells * cells * 3)
                        for (let k = 0; k < data.n; ++k) {
                            if (!Colour.shown(data, k))
                                continue
                            const r = Math.sqrt(Math.min(1, data.C[k] / finder.rimChroma)) * R
                            const t = data.h[k] * Math.PI / 180
                            const x = Math.min(cells - 1, Math.floor((R + r * Math.cos(t)) / size))
                            const y = Math.min(cells - 1, Math.floor((R - r * Math.sin(t)) / size))
                            const c = Colour.rgb(data.L[k], data.a[k], data.b[k])
                            const o = y * cells + x
                            count[o] += 1
                            sum[3 * o] += c[0]
                            sum[3 * o + 1] += c[1]
                            sum[3 * o + 2] += c[2]
                        }
                        let most = 1
                        for (let o = 0; o < count.length; ++o)
                            most = Math.max(most, count[o])
                        for (let o = 0; o < count.length; ++o) {
                            const n = count[o]
                            if (n === 0)
                                continue
                            // By the log of how many: one photo shows, and
                            // the thousands in the grey middle do not drown
                            // the rest.
                            const share = Math.log(1 + n) / Math.log(1 + most)
                            const radius = size * (0.22 + 0.4 * share)
                            ctx.fillStyle = "rgb(" + Math.round(sum[3 * o] / n) + "," + Math.round(sum[3 * o + 1] / n) + ","
                                            + Math.round(sum[3 * o + 2] / n) + ")"
                            ctx.beginPath()
                            ctx.arc(((o % cells) + 0.5) * size, (Math.floor(o / cells) + 0.5) * size, radius, 0, 2 * Math.PI)
                            ctx.fill()
                        }
                    }
                }
                // The colour looked for.
                Rectangle {
                    readonly property point at: wheel.pointOf(finder.targetA, finder.targetB)
                    x: at.x - width / 2
                    y: at.y - height / 2
                    width: Math.round(theme.fontSize * 2.2)
                    height: width
                    radius: width / 2
                    color: finder.targetHex
                    border.width: 3
                    border.color: theme.brightForeground
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -1
                        radius: width / 2
                        color: "transparent"
                        border.width: 1
                        border.color: theme.background
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    onPressed: mouse => wheel.pickAt(mouse.x, mouse.y)
                    onPositionChanged: mouse => { if (pressed) wheel.pickAt(mouse.x, mouse.y) }
                }
            }

            // Lightness, dark to light, at the colour's hue and strength.
            Item {
                id: strip
                width: wheel.width
                height: Math.round(theme.fontSize * 2.333)
                readonly property real low: 0.12
                readonly property real high: 0.96
                Rectangle {
                    anchors.fill: parent
                    radius: Math.min(4, theme.radius)
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: strip.at(0) }
                        GradientStop { position: 0.2; color: strip.at(0.2) }
                        GradientStop { position: 0.4; color: strip.at(0.4) }
                        GradientStop { position: 0.6; color: strip.at(0.6) }
                        GradientStop { position: 0.8; color: strip.at(0.8) }
                        GradientStop { position: 1; color: strip.at(1) }
                    }
                }
                function at(t) {
                    return Colour.hexOf(low + t * (high - low), finder.targetC, finder.targetH)
                }
                Rectangle {
                    x: Math.max(0, Math.min(1, (finder.targetL - strip.low) / (strip.high - strip.low))) * strip.width - width / 2
                    y: -3
                    width: 6
                    height: strip.height + 6
                    radius: 3
                    color: "transparent"
                    border.width: 2
                    border.color: theme.brightForeground
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.SizeHorCursor
                    function set(x) {
                        const t = Math.max(0, Math.min(1, (x - 6) / strip.width))
                        finder.pick(strip.low + t * (strip.high - strip.low), finder.targetA, finder.targetB)
                    }
                    onPressed: mouse => set(mouse.x)
                    onPositionChanged: mouse => { if (pressed) set(mouse.x) }
                }
            }

            // What it looks for, and how well it can.
            Row {
                spacing: 10
                // The photo it is from, to open again.
                Rectangle {
                    visible: finder.from >= 0
                    width: Math.round(theme.fontSize * 4.5)
                    height: width
                    radius: Math.min(4, theme.radius)
                    color: finder.targetHex
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: finder.from >= 0 ? finder.pinhole.library.item(finder.from).thumbnail : ""
                        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: finder.open(finder.fromId) }
                }
                // The colour, when no photo shows it.
                Rectangle {
                    visible: finder.from < 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(theme.fontSize * 2.333)
                    height: width
                    radius: Math.min(4, theme.radius)
                    color: finder.targetHex
                    border.width: 1
                    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
                }
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    color: theme.brightForeground
                    text: finder.nameOf(Colour.hueName(finder.targetC, finder.targetH)) + "  <font color=\"" + theme.darkForeground + "\">"
                          + finder.targetHex + "</font>"
                }
            }
            UiText {
                width: wheel.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: finder.plugin.colours === null ? ""
                    : qsTr("Each photo is found by its average colour, a dot on the wheel at it. %1 of your %2 have a colour of their own; the rest are near the grey middle, found only by grey.")
                      .arg(Number(finder.plugin.colours.colourful).toLocaleString(Qt.locale(), "f", 0))
                      .arg(Number(finder.plugin.colours.count).toLocaleString(Qt.locale(), "f", 0))
            }
        }

        // The photos, closest first.
        ScrollGrid {
            id: photos
            anchors.left: picker.right
            anchors.leftMargin: 24
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            readonly property int across: Math.max(2, Math.round(width / 180))
            cellWidth: Math.floor(width / across)
            cellHeight: cellWidth
            model: finder.results
            delegate: Item {
                id: cell
                // A photo's id; where it is now, as the library changes.
                required property string modelData
                readonly property int position: (finder.plugin.revision, finder.pinhole.library.indexOf(modelData))
                readonly property var item: finder.pinhole.library.item(position)
                visible: position >= 0 && !(finder.plugin.hiddenRevision, finder.pinhole.isHidden(position))
                width: photos.cellWidth
                height: photos.cellHeight
                Rectangle {
                    id: tile
                    anchors.fill: parent
                    anchors.margins: 3
                    color: cell.item.avgColor || theme.darkBackground
                    radius: Math.min(4, theme.radius)
                    clip: true
                    scale: hover.hovered ? 1.04 : 1
                    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    Image {
                        anchors.fill: parent
                        source: cell.item.thumbnail ?? ""
                        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 180 } }
                    }
                    // The colour it was found by.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: Math.max(4, Math.round(parent.height * 0.06))
                        color: cell.item.avgColor || "transparent"
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: finder.open(cell.modelData) }
                }
            }
        }
    }

    function nameOf(name) {
        switch (name) {
        case "grey": return qsTr("Grey")
        case "pink": return qsTr("Pink")
        case "red": return qsTr("Red")
        case "orange": return qsTr("Orange")
        case "yellow": return qsTr("Yellow")
        case "green": return qsTr("Green")
        case "cyan": return qsTr("Cyan")
        case "blue": return qsTr("Blue")
        case "violet": return qsTr("Violet")
        case "magenta": return qsTr("Magenta")
        }
        return name
    }

    Wall {
        id: wall
        visible: finder.mode === "wall"
        anchors.fill: parent
        anchors.topMargin: (finder.narrow ? wallControls.y + wallControls.height : top.y + top.height) + 16
        anchors.margins: 16
        finder: finder
    }
}
