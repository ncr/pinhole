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
    // Opens the menu; and what the help calls that.
    property string menuKey
    property string menuText
    // Its menu is opened by keys, the menuKey's or others' (More's, by
    // each folded group's): items without a key of their own are done
    // through it.
    property bool keyedMenu: menuKey !== ""
    // ▾ beside the button; none for a button that opens the menu itself.
    property bool arrowShown: true
    // The menu over the bar rather than under it, for a bar at the bottom.
    property bool upward: false
    // Whose keys its items' are, "grid" or "viewer" (Command.qml); and its
    // part of the help, for items that do not name one.
    property string keyLayer: "chrome"
    property string section
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
    // Open, by the keys, on `item`'s row (or the first).
    function openAt(item) {
        current = Math.max(0, menuItems.indexOf(item))
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
            visible: group.arrowShown && group.menuItems.length > 0
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

    // Each item's key, with the menu closed or open (while it is, the
    // menu's layer has the keys), and the menu's own: ⇧ with the button's
    // key opens it, ↑ ↓ choose, ⏎ presses, Esc closes.
    readonly property string itemLayer: group.open ? "menu" : group.keyLayer
    Instantiator {
        model: group.items
        delegate: Command {
            required property var modelData
            name: "menu." + (modelData.text ?? "")
            text: modelData.helpText ?? modelData.text ?? ""
            section: modelData.section ?? group.section
            keys: (modelData.key ?? "") !== "" ? [modelData.key] : []
            // Without a key of its own, by keys through the menu.
            noKeyboard: keys.length === 0 && group.keyedMenu ? qsTr("Through its menu") : ""
            keyLayer: group.itemLayer
            when: group.visible && modelData.shown !== false && modelData.usable !== false
            // In the menu, or the button: a pointer's way either way.
            buttons: 1
            onTriggered: group.press(modelData)
        }
    }
    Loader {
        active: group.menuKey !== ""
        sourceComponent: Command {
            name: "menu.open." + group.menuKey
            text: group.menuText
            section: group.menuText !== "" ? group.section : ""
            keys: [group.menuKey]
            keyLayer: group.keyLayer
            when: group.visible && group.menuItems.length > 0
            buttons: 1
            onTriggered: group.toggle(true)
        }
    }
    Command {
        name: "menu.close"
        keys: ["Escape"]
        keyLayer: "menu"
        when: group.open
        buttons: 1
        onTriggered: group.open = false
    }
    Command {
        name: "menu.up"
        keys: ["Up"]
        keyLayer: "menu"
        when: group.open
        autoRepeat: true
        buttons: 1
        onTriggered: group.current = Math.max(0, group.current - 1)
    }
    Command {
        name: "menu.down"
        keys: ["Down"]
        keyLayer: "menu"
        when: group.open
        autoRepeat: true
        buttons: 1
        onTriggered: group.current = Math.min(group.menuItems.length - 1, group.current + 1)
    }
    Command {
        name: "menu.press"
        keys: ["Return", "Enter"]
        keyLayer: "menu"
        when: group.open
        buttons: 1
        // Opened with a click, nothing is chosen yet: ⏎ takes the first row.
        onTriggered: group.press(group.menuItems[Math.max(0, group.current)])
    }
}
