import QtQuick
import Pinhole
import QtQml
import QtQuick.Window
import "keys.js" as KeyNames
import "focus.js" as Focus

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
    property string menuText: primary >= 0 ? items[primary].text : qsTr("More options")
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
    property bool pointerInput: false
    property var returnFocus: null
    onOpenChanged: {
        if (open) {
            returnFocus = group.Window.window?.activeFocusItem
            Qt.callLater(focusCurrent)
        } else Qt.callLater(() => {
            if (["grid", "viewer"].includes(keymap.layer) && returnFocus?.visible && returnFocus.enabled)
                returnFocus.forceActiveFocus()
        })
    }
    onCurrentChanged: if (open && !pointerInput) Qt.callLater(focusCurrent)
    onPointerInputChanged: if (open && !pointerInput) Qt.callLater(focusCurrent)
    function focusCurrent() {
        if (!open) return
        const row = current >= 0 ? rows.itemAt(current) : null
        if (!pointerInput && row?.usable) row.forceActiveFocus(Qt.TabFocusReason)
        else menu.forceActiveFocus(Qt.MouseFocusReason)
    }
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
    Connections {
        target: group
        // MoreGroup and PluginGroup also react to visibility. Keep closing
        // independent of their handlers when a resize folds a group away.
        function onVisibleChanged() { if (!group.visible) group.open = false }
    }
    onMenuItemsChanged: {
        if (menuItems.length === 0) open = false
        else if (open && !pointerInput) {
            if (current >= menuItems.length) current = menuItems.findIndex(a => a.usable !== false)
            Qt.callLater(focusCurrent)
        }
    }

    // A word of its own at the right of its row ("iPhone"), else its key.
    function hintOf(action) { return (action.hint ?? "") !== "" ? action.hint : KeyNames.nameOf(action.key ?? "") }
    function press(action) {
        if (!action || action.usable === false) return
        open = false
        action.triggered()
    }
    function toggle(byKey) {
        if (open) {
            open = false
            return
        }
        pointerInput = !byKey
        const lit = menuItems.findIndex(a => a.active && a.usable !== false)
        current = byKey ? (lit >= 0 ? lit : menuItems.findIndex(a => a.usable !== false)) : -1
        open = true
    }
    // Open, by the keys, on `item`'s row (or the first).
    function openAt(item) {
        pointerInput = false
        current = Math.max(0, menuItems.indexOf(item))
        open = true
    }
    function step(direction) {
        pointerInput = false
        let at = current < 0 ? (direction > 0 ? -1 : menuItems.length) : current
        for (at += direction; at >= 0 && at < menuItems.length; at += direction) {
            if (menuItems[at].usable !== false) { current = at; return }
        }
    }
    // What its button could not do, said under it for a few seconds, the
    // button shaking (Complaint.qml); and gone again at once.
    function complain(text) {
        complaint.active = true
        complaint.item.say(text)
        complained()
    }
    signal complained()
    function hush() {
        if (complaint.item)
            complaint.item.hush()
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
            transform: Translate { id: slotNudge }
        }
        ShellButton {
            id: arrow
            objectName: group.objectName + "Arrow"
            visible: group.arrowShown && group.menuItems.length > 0
            flat: true
            inset: Math.round(theme.fontSize * 0.5)
            text: "▾"
            Accessible.name: group.menuText || qsTr("More options")
            Accessible.expandable: true
            Accessible.expanded: group.open
            hint: ""
            lit: group.open
            onTapped: group.toggle(false)
        }
    }

    Loader {
        id: complaint
        active: false
        sourceComponent: Complaint {
            button: slot
            nudged: slotNudge
            upward: group.upward
        }
    }

    MenuCard {
        id: menu
        title: group.menuText
        objectName: group.objectName !== "" ? group.objectName + "Card" : ""
        visible: group.open
        FocusFence.enabled: group.open
        Accessible.role: Accessible.PopupMenu
        Accessible.name: group.menuText || qsTr("More options")
        Accessible.ignored: !group.open
        Keys.onShortcutOverride: event => {
            if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                    && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) event.accepted = true
        }
        Keys.onPressed: event => {
            if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                    && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                Focus.move(menu, event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) ? -1 : 1, group.Window.window?.activeFocusItem)
                event.accepted = true
            }
        }
        // Clamp even a long plugin label or an upward menu to the window.
        readonly property point origin: {
            let x = 0, y = 0
            for (let item = group; item; item = item.parent) { x += item.x; y += item.y }
            return Qt.point(x, y)
        }
        x: Math.max(12 - origin.x, Math.min(row.width - width,
                (group.Window.window?.width ?? width + 24) - width - 12 - origin.x))
        readonly property real above: Math.max(0, origin.y - 20)
        readonly property real below: Math.max(0, (group.Window.window?.height ?? implicitHeight + 24) - origin.y - group.height - 20)
        readonly property bool opensUp: group.upward ? above >= Math.min(implicitHeight, below)
                                                               : below < implicitHeight && above > below
        maximumHeight: Math.max(1, opensUp ? above : below)
        y: opensUp ? -height - 8 : group.height + 8
        preferredWidth: Math.max(theme.fontSize * 18, rowWidth + 2 * padding)
        // The widest row's, from the words (as ShellButton.qml lays a
        // justified row out), so every row is as wide as the menu.
        readonly property real rowWidth: {
            let most = 0
            for (const a of group.menuItems) {
                const hint = group.hintOf(a)
                most = Math.max(most, Math.ceil(metrics.of(a.text)
                    + (hint !== "" ? 2 * theme.fontSize + metrics.of(hint) : 0)))
            }
            return most + 2 * Math.round(theme.fontSize * 0.833)
                + (group.menuItems.some(a => a.active) ? 2 * theme.fontSize : 0)
        }
        LabelMetrics { id: metrics }
        Repeater {
            id: rows
            // Closed groups do not instantiate any menu rows.
            model: group.open ? group.menuItems : []
            delegate: MenuRow {
                required property var modelData
                required property int index
                objectName: "menuOption_" + index
                lit: !group.pointerInput && group.current === index
                text: modelData.text
                hint: group.hintOf(modelData)
                usable: modelData.usable !== false
                selected: modelData.active === true
                reserveCheck: group.menuItems.some(a => a.active)
                onActiveFocusChanged: if (activeFocus && keymap.input === "keyboard") {
                    group.pointerInput = false
                    group.current = index
                }
                busy: modelData.busy === true
                busyDelay: modelData.busyDelay ?? 1000
                onTapped: group.press(modelData)
                onPointed: {
                    group.pointerInput = true
                    group.current = index
                    // Retire an old keyboard row; Enter now uses the row
                    // under the pointer rather than that stale native focus.
                    menu.forceActiveFocus(Qt.MouseFocusReason)
                }
                onHoveredChanged: {
                    modelData.hovered = hovered
                    if (!hovered && group.pointerInput && group.current === index) group.current = -1
                }
                Component.onDestruction: if (hovered) modelData.hovered = false
            }
        }
    }

    // A press outside the menu closes it, and goes no further.
    Shield {
        id: dismiss
        parent: group.Window.window ? group.Window.window.contentItem : group
        anchors.fill: parent
        z: 1000
        visible: group.open && group.visible
        containmentMask: QtObject {
            function contains(point: point): bool {
                for (const item of [menu, arrow, ...(group.arrowShown ? [] : [slot])]) {
                    const p = dismiss.mapToItem(item, point.x, point.y)
                    if (item.visible && p.x >= 0 && p.y >= 0 && p.x < item.width && p.y < item.height) return false
                }
                return true
            }
        }
        onClicked: group.open = false
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
        onTriggered: group.step(-1)
    }
    Command {
        name: "menu.down"
        keys: ["Down"]
        keyLayer: "menu"
        when: group.open
        autoRepeat: true
        buttons: 1
        onTriggered: group.step(1)
    }
    Command {
        name: "menu.press"
        keys: ["Return", "Enter"]
        keyLayer: "menu"
        when: group.open
        buttons: 1
        // Opened with a click, nothing is chosen yet: ⏎ takes the first row.
        onTriggered: group.press(group.menuItems[group.current >= 0 ? group.current : group.menuItems.findIndex(a => a.usable !== false)])
    }
}
