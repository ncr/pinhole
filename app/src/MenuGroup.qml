import QtQuick
import QtQml
import QtQuick.Window
import "keys.js" as KeyNames

// A group of actions in an ActionBar: its button, and beside it ▾ with the
// rest of the group in a menu as Omarchy's, a card of the theme's background
// with a hairline in the text colour, each row with its key at the right.
// Every key works with the menu closed; ⇧ with the button's key opens it,
// and there ↑ ↓ choose, ⏎ presses and Esc closes. A press anywhere else
// closes it too.
Item {
    id: group

    // The actions, each with `text` and as it has them `key`, `hint`,
    // `shown`, `usable`, `active`, `hovered` and `triggered()`: a
    // PluginAction, or an object alike.
    property var items: []
    // The one of `items` that is the button, or -1 for a button given as
    // the group's child (Live's, Copy's).
    property int primary: -1
    // Opens the menu.
    property string menuKey
    // The menu over the bar rather than under it, for a bar at the bottom.
    property bool upward: false
    property bool open: false
    default property alias button: slot.data

    readonly property bool isGroup: true
    readonly property var menuItems: items.filter((a, i) => i !== primary && a.shown !== false)
    // The row chosen with the keys, -1 for none.
    property int current: -1

    // A hairline before it, when anything shows before it in the bar.
    readonly property bool leading: {
        const siblings = parent ? parent.children : []
        for (let i = 0; i < siblings.length; ++i) {
            const c = siblings[i]
            if (c === group)
                return false
            if (c.visible && c.width > 0)
                return true
        }
        return false
    }

    width: row.width
    height: row.height
    onVisibleChanged: if (!visible) open = false
    onMenuItemsChanged: if (menuItems.length === 0) open = false

    function hintOf(action) { return action.hint ?? KeyNames.nameOf(action.key ?? "") }
    function press(action) {
        open = false
        if (action.usable !== false)
            action.triggered()
    }
    function toggle(byKey) {
        if (open) {
            open = false
            return
        }
        const lit = menuItems.findIndex(a => a.active)
        current = byKey ? Math.max(0, lit) : -1
        open = true
    }

    Row {
        id: row
        Item {
            visible: group.leading
            width: 5
            height: group.height
            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: Math.round(parent.height * 0.6)
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.2)
            }
        }
        Item {
            id: slot
            width: childrenRect.width
            height: childrenRect.height
        }
        ShellButton {
            id: arrow
            visible: group.menuItems.length > 0
            flat: true
            inset: Math.round(theme.fontSize * 0.5)
            text: "▾"
            hint: ""
            active: group.open
            onTapped: group.toggle(false)
        }
    }

    Rectangle {
        id: menu
        visible: group.open
        anchors.right: row.right
        y: group.upward ? -height - 7 : group.height + 7
        width: column.width + 2
        height: column.height + 2
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        // The widest row's, from the words (as ShellButton.qml lays a
        // justified row out), so every row is as wide as the menu.
        readonly property real rowWidth: {
            let most = 0
            for (const a of group.menuItems) {
                const hint = group.hintOf(a)
                most = Math.max(most, Math.ceil(metrics.advanceWidth(a.text)
                    + (hint !== "" ? 2 * theme.fontSize + metrics.advanceWidth(hint) : 0)))
            }
            return most + 2 * Math.round(theme.fontSize * 0.833)
        }
        FontMetrics { id: metrics; font.family: theme.font; font.pixelSize: theme.fontSize }
        Column {
            id: column
            x: 1
            y: 1
            Repeater {
                id: rows
                model: group.menuItems
                delegate: ShellButton {
                    required property var modelData
                    required property int index
                    width: Math.max(naturalWidth, menu.rowWidth)
                    flat: true
                    justified: true
                    lit: group.current === index
                    text: modelData.text
                    hint: group.hintOf(modelData)
                    usable: modelData.usable !== false
                    active: modelData.active === true
                    onTapped: group.press(modelData)
                    onHoveredChanged: {
                        modelData.hovered = hovered
                        if (hovered)
                            group.current = -1
                    }
                    Component.onDestruction: if (hovered) modelData.hovered = false
                }
            }
        }
    }

    // A press outside the menu closes it, and goes no further.
    MouseArea {
        parent: group.Window.window ? group.Window.window.contentItem : group
        anchors.fill: parent
        z: 1000
        visible: group.open && group.visible
        onPressed: mouse => {
            for (const item of [menu, arrow]) {
                const p = mapToItem(item, mouse.x, mouse.y)
                if (p.x >= 0 && p.y >= 0 && p.x < item.width && p.y < item.height) {
                    mouse.accepted = false
                    return
                }
            }
            group.open = false
        }
    }

    // Not while the window has its keys for something over it (the help).
    readonly property bool keysFree: group.visible && (Window.window === null || Window.window.buttonKeys !== false)
    Instantiator {
        model: group.items
        delegate: Shortcut {
            required property var modelData
            enabled: group.keysFree && (modelData.key ?? "") !== "" && modelData.shown !== false && modelData.usable !== false
            sequence: modelData.key ?? ""
            context: Qt.ApplicationShortcut
            onActivated: group.press(modelData)
        }
    }
    Shortcut {
        enabled: group.keysFree && group.menuKey !== "" && group.menuItems.length > 0
        sequence: group.menuKey
        context: Qt.ApplicationShortcut
        onActivated: group.toggle(true)
    }
    Shortcut {
        enabled: group.keysFree && group.open
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: group.open = false
    }
    Shortcut {
        enabled: group.keysFree && group.open
        sequence: "Up"
        context: Qt.ApplicationShortcut
        autoRepeat: true
        onActivated: group.current = Math.max(0, group.current - 1)
    }
    Shortcut {
        enabled: group.keysFree && group.open
        sequence: "Down"
        context: Qt.ApplicationShortcut
        autoRepeat: true
        onActivated: group.current = Math.min(group.menuItems.length - 1, group.current + 1)
    }
    Shortcut {
        enabled: group.keysFree && group.open && group.current >= 0
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        onActivated: group.press(group.menuItems[group.current])
    }
}
