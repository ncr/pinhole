import QtQuick

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
    // Keys plugins list without commands of their own: [{title, keys}].
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
    // the touchpad, as each command says.
    readonly property var sections: {
        keymap.revision
        const rows = {}
        const add = (section, how, what) => {
            const list = rows[section] ?? (rows[section] = [])
            if (!list.some(r => r[0] === how && r[1] === what))
                list.push([how, what])
        }
        const commands = keymap.actions().slice().sort((a, b) => a.file < b.file ? -1 : a.file > b.file ? 1 : a.line - b.line)
        for (const c of commands) {
            if (!c.section || !c.text)
                continue
            if (tab === "keyboard") {
                if (c.keyHelp && c.keyName !== "")
                    add(c.section, c.keyName, c.text)
            } else if (c.pointerHelp) {
                const how = tab === "mouse" ? c.mouse : c.touchpad
                if (how)
                    add(c.section, how, c.text)
            }
        }
        if (tab === "keyboard")
            for (const s of pluginSections)
                for (const k of s.keys)
                    add(s.title, k[0], k[1])
        const known = order.filter(s => rows[s] !== undefined)
        const others = Object.keys(rows).filter(s => order.indexOf(s) < 0).sort()
        return known.concat(others).map(s => ({ title: titles[s] ?? s, keys: rows[s] }))
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
    MouseArea {
        anchors.fill: parent
        onClicked: root.shown = false
    }

    // A column as wide as its widest key and words, all columns alike.
    FontMetrics { id: metrics; font.family: theme.font; font.pixelSize: theme.fontSize }
    readonly property int gap: theme.fontSize
    function widest(texts) {
        let most = 0
        for (const text of texts)
            most = Math.max(most, metrics.advanceWidth(text))
        return Math.ceil(most)
    }
    readonly property int keyWidth: {
        const keys = []
        for (const section of sections)
            for (const key of section.keys)
                keys.push(key[0])
        return widest(keys)
    }
    readonly property int wordsWidth: {
        const words = []
        for (const section of sections) {
            words.push(section.title)
            for (const key of section.keys)
                words.push(key[1])
        }
        return widest(words)
    }
    readonly property int columnWidth: keyWidth + gap + wordsWidth
    readonly property int padding: 2 * theme.fontSize
    readonly property int columns: Math.max(1, Math.min(sections.length,
        Math.floor((width - 32 - 2 * padding + 2 * gap) / (columnWidth + 2 * gap))))

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, Math.max(flow.width, head.implicitWidth) + 2 * root.padding)
        height: Math.min(parent.height - 32, head.height + theme.fontSize + flow.height + 2 * root.padding)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true
        // Clicks inside stay inside.
        MouseArea { anchors.fill: parent }

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
            contentWidth: flow.width
            contentHeight: flow.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Flow {
                id: flow
                width: root.columns * root.columnWidth + (root.columns - 1) * 2 * root.gap
                spacing: 2 * root.gap
                Repeater {
                    model: root.sections
                    delegate: Column {
                        required property var modelData
                        width: root.columnWidth
                        spacing: Math.round(theme.fontSize * 0.4)
                        UiText {
                            x: root.keyWidth + root.gap
                            text: parent.modelData.title
                            color: theme.accent
                            bottomPadding: Math.round(theme.fontSize * 0.25)
                        }
                        Repeater {
                            model: parent.modelData.keys
                            delegate: Row {
                                required property var modelData
                                spacing: root.gap
                                UiText {
                                    width: root.keyWidth
                                    text: parent.modelData[0]
                                    color: theme.brightForeground
                                    horizontalAlignment: Text.AlignRight
                                }
                                UiText {
                                    text: parent.modelData[1]
                                    color: theme.darkForeground
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
