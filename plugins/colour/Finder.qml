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
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // "find" or "wall".
    property string mode: "find"
    // The colour looked for, OKLab, and the photo it is from, if any.
    property real targetL: 0.62
    property real targetA: 0.09
    property real targetB: 0.07
    property int from: -1
    readonly property real targetC: Colour.chroma(targetA, targetB)
    readonly property real targetH: Colour.hue(targetA, targetB)
    readonly property string targetHex: Colour.hex(targetL, targetA, targetB)
    // How far out the wheel goes: chroma at its rim. A photo's average is
    // rarely past 0.15, and most are under 0.05, so the wheel is drawn by
    // the square root of chroma, giving the middle the room.
    readonly property real rimChroma: 0.22
    readonly property int resultCount: 300
    property var results: []
    // A photo opened from here: shown again when it closes.
    property bool returning: false

    function openFinder() {
        plugin.ensure()
        mode = "find"
        from = -1
        returning = false
        shown = true
        search()
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
        from = index
        mode = "find"
        returning = false
        shown = true
        search()
    }
    function close() {
        shown = false
        returning = false
    }
    function open(index) {
        if (index < 0)
            return
        returning = true
        shown = false
        pinhole.openPhoto(index)
    }
    function search() {
        const d = plugin.ensure()
        results = Colour.nearest(d, targetL, targetA, targetB, resultCount, from)
    }
    // Picked by hand: no longer a photo's.
    function pick(L, a, b) {
        targetL = Math.max(0.05, Math.min(0.98, L))
        targetA = a
        targetB = b
        from = -1
        searching.restart()
    }
    function pickPolar(C, h) {
        const p = Colour.fromPolar(targetL, Math.max(0, Math.min(rimChroma, C)), (h + 360) % 360)
        pick(targetL, p[1], p[2])
    }
    // While a finger drags on the wheel, the photos follow a few times a
    // second rather than at every move.
    Timer { id: searching; interval: 60; onTriggered: finder.search() }

    Connections {
        target: finder.pinhole.viewer
        function onOpenChanged() {
            if (!finder.pinhole.viewer.open && finder.returning) {
                finder.returning = false
                finder.shown = true
            }
        }
    }
    Connections {
        target: finder.plugin
        function onColoursChanged() {
            if (finder.shown && finder.plugin.colours)
                finder.search()
        }
    }

    Command {
        name: "colour.close"
        keys: ["Escape"]
        keyLayer: "plugin"
        when: finder.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: finder.close()
    }
    Command {
        name: "colour.close.h"
        keys: ["H"]
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: finder.close()
    }
    Command {
        name: "colour.hue"
        text: qsTr("Hue round the wheel")
        section: "Colours"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finder.pickPolar(Math.max(finder.targetC, 0.04), finder.targetH + 10)
    }
    Command {
        name: "colour.hueBack"
        keys: ["Right"]
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finder.pickPolar(Math.max(finder.targetC, 0.04), finder.targetH - 10)
    }
    Command {
        name: "colour.stronger"
        text: qsTr("Stronger, greyer")
        section: "Colours"
        keys: ["Up"]
        keysText: "↑ ↓"
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: {
            const r = Math.min(1, Math.sqrt(finder.targetC / finder.rimChroma) + 0.05)
            finder.pickPolar(r * r * finder.rimChroma, finder.targetH)
        }
    }
    Command {
        name: "colour.greyer"
        keys: ["Down"]
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: {
            const r = Math.max(0, Math.sqrt(finder.targetC / finder.rimChroma) - 0.05)
            finder.pickPolar(r * r * finder.rimChroma, finder.targetH)
        }
    }
    Command {
        name: "colour.darker"
        text: qsTr("Darker, lighter")
        section: "Colours"
        keys: ["["]
        keysText: "[ ]"
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finder.pick(finder.targetL - 0.04, finder.targetA, finder.targetB)
    }
    Command {
        name: "colour.lighter"
        keys: ["]"]
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finder.pick(finder.targetL + 0.04, finder.targetA, finder.targetB)
    }
    Command {
        name: "colour.first"
        text: qsTr("Open the closest")
        section: "Colours"
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        when: finder.shown && finder.mode === "find" && finder.results.length > 0
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        onTriggered: finder.open(finder.results[0])
    }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }
    // Clicks stay here, off the photos beneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }

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
        Loader {
            active: finder.mode === "wall"
            sourceComponent: Row {
                spacing: 6
                ShellButton {
                    text: qsTr("Another")
                    key: "Space"
                    keyLayer: "plugin"
                    hint: "Space"
                    onTapped: wall.another()
                }
                ShellButton {
                    text: "−"
                    key: "-"
                    keyLayer: "plugin"
                    hint: "-"
                    usable: wall.density > 0
                    onTapped: wall.density -= 1
                }
                ShellButton {
                    text: "+"
                    key: "+"
                    keyLayer: "plugin"
                    hint: "+"
                    usable: wall.density < wall.densities.length - 1
                    onTapped: wall.density += 1
                }
            }
        }
    }
    // What is shown, where there is room for it.
    Chip {
        x: top.x + top.width + 6
        y: top.y
        visible: finder.plugin.colours !== null && x + width + 12 <= closeButton.x
        textColor: theme.darkForeground
        text: finder.mode === "wall" ? qsTr("%1 of your most colourful photos, by hue").arg(Number(wall.shownCount).toLocaleString(Qt.locale(), "f", 0))
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
                    onLightnessChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    Connections {
                        target: finder.plugin
                        function onColoursChanged() { disc.requestPaint() }
                    }
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
                        const faint = finder.plugin.colours ? 0.45 : 1
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
                    TapHandler { onTapped: finder.open(finder.from) }
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
                      .arg(Number(finder.plugin.colourful).toLocaleString(Qt.locale(), "f", 0))
                      .arg(Number(finder.plugin.colours.n).toLocaleString(Qt.locale(), "f", 0))
            }
        }

        // The photos, closest first.
        GridView {
            id: photos
            anchors.left: picker.right
            anchors.leftMargin: 24
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            clip: true
            readonly property int across: Math.max(2, Math.round(width / 180))
            cellWidth: Math.floor(width / across)
            cellHeight: cellWidth
            model: finder.results
            boundsBehavior: Flickable.StopAtBounds
            delegate: Item {
                id: cell
                required property int modelData
                required property int index
                readonly property var item: finder.pinhole.library.item(modelData)
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
                        source: cell.item.thumbnail
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
        anchors.topMargin: top.y + top.height + 16
        anchors.margins: 16
        finder: finder
    }
}
