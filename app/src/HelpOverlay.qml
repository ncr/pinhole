import QtQuick
import Pinhole
import "keys.js" as KeyNames

// The keys and gestures, on ? or the ? button: made from the window's
// commands (Command.qml, keymap.h), so it says what Pinhole does and no
// more. Three tabs, the keyboard's, the mouse's and the touchpad's, opening
// on the one used last; as few words as will do, in as many columns as
// fit, scrolling only if the window is too short for them.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    // Keys plugins list without commands of their own: [{plugin, keys}].
    property var pluginSections: []

    // "keyboard", "mouse" or "touchpad".
    property string tab: "keyboard"
    onShownChanged: if (shown) tab = keymap.input
    readonly property var tabs: [
        { id: "keyboard", name: qsTr("Keyboard") },
        { id: "mouse", name: qsTr("Mouse") },
        { id: "touchpad", name: qsTr("Touchpad") }
    ]
    function turn(direction) {
        const at = tabs.findIndex(t => t.id === tab)
        tab = tabs[(at + direction + tabs.length) % tabs.length].id
        scroller.contentY = 0
    }

    // Pinhole's parts first, in this order, then each plugin's.
    readonly property var order: ["library", "select", "photo", "video", "share", "pinhole"]
    readonly property var titles: ({
        "library": qsTr("Library"),
        "select": qsTr("Select mode"),
        "photo": qsTr("Photo"),
        "video": qsTr("Video"),
        "share": qsTr("Share"),
        "pinhole": "Pinhole"
    })
    // Each part's rows, [how, what]: the keys, or how with the mouse or
    // the touchpad, as each command says. A plugin's own are under its
    // name, in the language, whatever section they give (but Pinhole's
    // parts), with what its `help` lists. Only while it shows: every
    // command made or gone (a plugin loading, a menu) would make it all
    // again, unseen, a quarter of a second as the window opens.
    readonly property var sections: {
        if (!visible)
            return []
        keymap.revision
        plugins.revision
        const rows = {}
        const add = (section, how, what) => {
            const list = rows[section] ?? (rows[section] = [])
            if (!list.some(r => r[0] === how && r[1] === what))
                list.push([how, what])
        }
        const sectionOf = c => {
            const plugin = order.indexOf(c.section) < 0 ? c.plugin() : ""
            return plugin !== "" ? "plugin:" + plugin : c.section
        }
        const commands = keymap.actions().slice().sort((a, b) => a.file < b.file ? -1 : a.file > b.file ? 1 : a.line - b.line)
        for (const c of commands) {
            if (!c.section || !c.text)
                continue
            if (tab === "keyboard") {
                if (c.keyHelp && c.keyName !== "")
                    add(sectionOf(c), c.keyName, c.text)
            } else if (c.pointerHelp) {
                const how = tab === "mouse" ? c.mouse : c.touchpad
                if (how)
                    add(sectionOf(c), how, c.text)
            }
        }
        if (tab === "keyboard")
            for (const s of pluginSections)
                for (const k of s.keys)
                    add("plugin:" + s.plugin, KeyNames.nameOf(k[0]), k[1])
        const titleOf = s => titles[s] ?? (s.startsWith("plugin:") ? plugins.at(s.slice(7)).name ?? s.slice(7) : s)
        const known = order.filter(s => rows[s] !== undefined)
        const others = Object.keys(rows).filter(s => order.indexOf(s) < 0)
            .sort((a, b) => titleOf(a).localeCompare(titleOf(b)))
        return known.concat(others).map(s => ({ title: titleOf(s), keys: rows[s] }))
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        name: "help.previousTab"
        keys: ["Left", "Shift+Tab", "Backtab"]
        keyLayer: "help"
        mouse: qsTr("Click a tab")
        touchpad: qsTr("Tap a tab")
        onTriggered: root.turn(-1)
    }
    Command {
        name: "help.nextTab"
        keys: ["Right", "Tab"]
        keyLayer: "help"
        mouse: qsTr("Click a tab")
        touchpad: qsTr("Tap a tab")
        onTriggered: root.turn(1)
    }
    Command {
        name: "help.up"
        keys: ["Up"]
        keyLayer: "help"
        autoRepeat: true
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: scroller.contentY = Math.max(0, scroller.contentY - 3 * theme.fontSize)
    }
    Command {
        name: "help.down"
        keys: ["Down"]
        keyLayer: "help"
        autoRepeat: true
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, scroller.contentY + 3 * theme.fontSize))
    }
    Command {
        name: "help.pageUp"
        keys: ["PgUp"]
        keyLayer: "help"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: scroller.contentY = Math.max(0, scroller.contentY - scroller.height * 0.9)
    }
    Command {
        name: "help.pageDown"
        keys: ["PgDown"]
        keyLayer: "help"
        mouse: qsTr("Wheel")
        touchpad: qsTr("Two fingers")
        onTriggered: scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, scroller.contentY + scroller.height * 0.9))
    }

    // As Omarchy's menus: a scrim of the theme's background at half, and a
    // card of it with a hairline in the text colour.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    // A click beside it puts it away; nothing there reaches the photos.
    Shield {
        anchors.fill: parent
        onClicked: root.shown = false
    }

    // Columns all alike. As many as fit at a width each part reads well
    // in, its keys up to 12 times the type size and its words up to 15 (the
    // mouse's and the touchpad's keys are words too, so they take fewer,
    // wider columns than the keyboard's); then all as wide as the window
    // leaves them, but no wider than the widest part needs. Each part has
    // its own keys' width. The parts down each column in order, as even as
    // they go (layout()).
    FontMetrics { id: metrics; font.family: theme.font; font.pixelSize: theme.fontSize }
    readonly property int gap: theme.fontSize
    readonly property int columnGap: 2 * gap
    function widest(texts) {
        let most = 0
        for (const text of texts)
            most = Math.max(most, metrics.advanceWidth(text))
        return Math.ceil(most)
    }
    // Each part's widest keys and widest words (its title's among them).
    readonly property var measures: sections.map(s => ({
        keys: widest(s.keys.map(k => k[0])),
        words: widest([s.title].concat(s.keys.map(k => k[1])))
    }))
    readonly property int padding: 2 * theme.fontSize
    readonly property int room: width - 32 - 2 * padding
    readonly property int columns: {
        let least = 1
        for (const m of measures)
            least = Math.max(least, Math.min(m.keys, 12 * theme.fontSize) + gap + Math.min(m.words, 15 * theme.fontSize))
        return Math.max(1, Math.min(sections.length, Math.floor((room + columnGap) / (least + columnGap))))
    }
    readonly property int columnWidth: {
        let most = 1
        for (const m of measures)
            most = Math.max(most, m.keys + gap + m.words)
        return Math.max(1, Math.min(most, Math.floor((room - (columns - 1) * columnGap) / columns)))
    }

    Rectangle {
        id: card
        objectName: "helpCard"
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, Math.max(board.width, head.implicitWidth) + 2 * root.padding)
        height: Math.min(parent.height - 32, head.height + theme.fontSize + board.height + 2 * root.padding)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true
        // Clicks inside stay inside.
        Shield { anchors.fill: parent }

        // The tabs, and what goes for every button.
        Column {
            id: head
            x: root.padding
            y: root.padding
            width: card.width - 2 * root.padding
            spacing: Math.round(theme.fontSize * 0.6)
            Row {
                spacing: 6
                Repeater {
                    model: root.tabs
                    delegate: ShellButton {
                        required property var modelData
                        flat: root.tab !== modelData.id
                        active: root.tab === modelData.id
                        text: modelData.name
                        hint: ""
                        onTapped: {
                            root.tab = modelData.id
                            scroller.contentY = 0
                        }
                    }
                }
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: theme.fontSize
                    text: "← →"
                    color: theme.darkForeground
                }
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                text: root.tab === "keyboard" ? qsTr("Each button shows its key beside it.")
                    : root.tab === "mouse" ? qsTr("Every button works with a click, as its key does.")
                    : qsTr("Every button works with a tap, as its key does. A tap with two fingers only stops what moves.")
            }
        }

        Flickable {
            id: scroller
            anchors.fill: parent
            anchors.margins: root.padding
            anchors.topMargin: head.y + head.height + theme.fontSize
            contentWidth: board.width
            contentHeight: board.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Item {
                id: board
                objectName: "helpBoard"
                width: used * root.columnWidth + (used - 1) * root.columnGap
                height: 0
                // Where each part stands, [{x, y}], and the columns they
                // take: by layout().
                property var places: []
                property int used: 1
                // The parts in order down the columns, each column no
                // longer than it must be for them all to fit in as many
                // columns as there are.
                function layout() {
                    const heights = []
                    for (let i = 0; i < parts.count; ++i) {
                        const part = parts.itemAt(i)
                        if (!part)
                            return
                        part.measure()
                        heights.push(Math.ceil(part.implicitHeight))
                    }
                    const space = root.columnGap
                    // Down the columns, a new one when a part would pass
                    // `most`: each part's place, and how many columns.
                    const fill = most => {
                        const places = []
                        let column = 0, y = 0
                        for (const h of heights) {
                            if (y > 0 && y + space + h > most) {
                                ++column
                                y = 0
                            }
                            const top = y > 0 ? y + space : 0
                            places.push({ x: column * (root.columnWidth + root.columnGap), y: top, bottom: top + h })
                            y = top + h
                        }
                        return { places: places, columns: column + 1 }
                    }
                    const total = heights.reduce((sum, h) => sum + h + space, 0)
                    let most = Math.max(0, ...heights)
                    let laid = fill(most)
                    while (laid.columns > root.columns && most < total) {
                        most += 4
                        laid = fill(most)
                    }
                    places = laid.places
                    used = laid.columns
                    height = Math.max(0, ...laid.places.map(p => p.bottom))
                }
                function relayout() { Qt.callLater(layout) }
                Connections {
                    target: root
                    function onColumnsChanged() { board.relayout() }
                    function onColumnWidthChanged() { board.relayout() }
                }
                Repeater {
                    id: parts
                    model: root.sections
                    onItemAdded: board.relayout()
                    onItemRemoved: board.relayout()
                    delegate: Column {
                        id: part
                        required property var modelData
                        required property int index
                        // Its keys' width, its own while its words fit
                        // beside them; if not, keys and words share the
                        // column as their widest would, the keys never
                        // less than four tenths (so a key's few symbols
                        // do not wrap for a long word), and both wrap.
                        readonly property int keyWidth: {
                            const m = root.measures[index] ?? { keys: 0, words: 0 }
                            const room = root.columnWidth - root.gap
                            return Math.min(m.keys, Math.max(room - m.words, Math.floor(room * 0.4),
                                                             Math.floor(room * m.keys / Math.max(1, m.keys + m.words))))
                        }
                        x: board.places[index]?.x ?? 0
                        y: board.places[index]?.y ?? 0
                        width: root.columnWidth
                        spacing: Math.round(theme.fontSize * 0.4)
                        onImplicitHeightChanged: board.relayout()
                        // Its height now, its rows' and then its own, not
                        // as the window next polishes them: the parts are
                        // made as the help shows (sections), and a window
                        // drawn at once (a grab, a workspace not shown)
                        // put them by their titles alone, over each other.
                        function measure() {
                            for (const row of children)
                                if (row.forceLayout)
                                    row.forceLayout()
                            forceLayout()
                        }
                        UiText {
                            x: part.keyWidth + root.gap
                            width: part.width - x
                            wrapMode: Text.Wrap
                            text: part.modelData.title
                            color: theme.accent
                            bottomPadding: Math.round(theme.fontSize * 0.25)
                        }
                        Repeater {
                            model: part.modelData.keys
                            delegate: Row {
                                required property var modelData
                                spacing: root.gap
                                UiText {
                                    width: part.keyWidth
                                    wrapMode: Text.Wrap
                                    text: parent.modelData[0]
                                    color: theme.brightForeground
                                    horizontalAlignment: Text.AlignRight
                                }
                                UiText {
                                    width: part.width - part.keyWidth - root.gap
                                    wrapMode: Text.Wrap
                                    text: parent.modelData[1]
                                    color: theme.darkForeground
                                }
                            }
                        }
                    }
                }
            }
        }
        // The wheel and the touchpad anywhere on the card scroll it.
        Scroller {
            anchors.fill: parent
            flickable: scroller
        }
    }
}
