import QtQuick
import Pinhole
import "focus.js" as Focus

// A saved preference, rather than a list of actions: the selected radio
// stays distinct from the row under the pointer or chosen with the keys.
Item {
    id: root
    property Item button
    property bool available: true
    property bool open: false
    property string value: "smart"
    property int current: -1
    property bool pointerInput: false
    property var returnFocus: null
    Accessible.role: Accessible.PopupMenu
    Accessible.name: qsTr("Photo order")
    Accessible.ignored: !visible
    FocusFence.enabled: visible
    function focusCurrent() {
        if (!open) return
        if (pointerInput) card.forceActiveFocus(Qt.MouseFocusReason)
        else optionRows.itemAt(current >= 0 ? current : Math.max(0, options.findIndex(option => option.value === value)))?.forceActiveFocus(Qt.TabFocusReason)
    }
    onOpenChanged: {
        if (open) {
            returnFocus = root.Window.window?.activeFocusItem
            Qt.callLater(focusCurrent)
        } else Qt.callLater(() => { if (returnFocus?.visible && returnFocus.enabled) returnFocus.forceActiveFocus() })
    }
    onCurrentChanged: if (open && !pointerInput) Qt.callLater(focusCurrent)
    onPointerInputChanged: if (open && !pointerInput) Qt.callLater(focusCurrent)
    Keys.onShortcutOverride: event => {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) event.accepted = true
    }
    Keys.onPressed: event => {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            Focus.move(root, event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) ? -1 : 1, root.Window.window?.activeFocusItem)
            event.accepted = true
        }
    }
    readonly property var options: [
        { value: "smart", title: qsTr("Automatic"),
          detail: qsTr("New additions for 30 days, then by date taken.") },
        { value: "taken", title: qsTr("Date taken"),
          detail: qsTr("Place every photo at the moment it was taken.") },
        { value: "added", title: qsTr("Date added"),
          detail: qsTr("Keep photos together by when they arrived.") }
    ]
    signal chosen(string mode)

    anchors.fill: parent
    visible: available && open
    onAvailableChanged: if (!available) open = false
    function toggle(byKey) {
        pointerInput = !byKey
        current = byKey ? Math.max(0, options.findIndex(option => option.value === value)) : -1
        open = !open
    }
    function choose(index) {
        chosen(options[index].value)
        open = false
    }

    // Closing consumes the press; wheels and pinches never reach the grid.
    Panel {
        color: "transparent"
        onTriggered: root.open = false
    }
    MenuCard {
        id: card
        objectName: "orderMenuCard"
        title: qsTr("Photo order")
        // Open below the date, keeping it and the card inside the window.
        x: Math.max(12, Math.min(root.button ? root.button.x : 16, root.width - width - 12))
        y: Math.max(12, Math.min(root.button ? root.button.y + root.button.height + 8 : 16,
                                root.height - height - 12))
        Repeater {
            id: optionRows
            model: root.options
            delegate: MenuRow {
                required property var modelData
                required property int index
                objectName: "orderOption_" + modelData.value
                text: modelData.title
                selected: root.value === modelData.value
                checkable: true
                lit: !root.pointerInput && root.current === index
                Accessible.role: Accessible.RadioButton
                Accessible.description: modelData.detail
                onActiveFocusChanged: if (activeFocus && keymap.input === "keyboard") {
                    root.pointerInput = false
                    root.current = index
                }
                onPointed: { root.pointerInput = true; root.current = index; card.forceActiveFocus(Qt.MouseFocusReason) }
                onHoveredChanged: if (!hovered && root.pointerInput && root.current === index) root.current = -1
                onTapped: root.choose(index)
            }
        }
        // One explanation, for the row being considered (otherwise the
        // saved choice). Fixed height keeps the card still while browsing.
        Item {
            id: details
            width: parent.width
            readonly property real textHeight: {
                let most = 0
                for (let i = 0; i < detailMeasures.count; ++i)
                    most = Math.max(most, detailMeasures.itemAt(i)?.implicitHeight ?? 0)
                return most
            }
            height: Math.max(Math.round(theme.fontSize * 3.1), explanation.y + textHeight + card.gap)
            Repeater {
                id: detailMeasures
                model: root.options
                delegate: UiText {
                    required property var modelData
                    visible: false
                    width: explanation.width
                    text: modelData.detail
                    font.pixelSize: explanation.font.pixelSize
                    wrapMode: Text.WordWrap
                    lineHeight: explanation.lineHeight
                }
            }
            Rectangle {
                width: parent.width - 2 * explanation.x
                x: explanation.x
                height: 1
                y: Math.round(theme.fontSize * 0.3)
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.15)
            }
            UiText {
                id: explanation
                objectName: "orderExplanation"
                x: Math.round(theme.fontSize * 0.833)
                y: Math.round(theme.fontSize * 0.8)
                width: parent.width - 2 * x
                readonly property int at: root.current >= 0 ? root.current
                                           : Math.max(0, root.options.findIndex(option => option.value === root.value))
                text: root.options[at].detail
                font.pixelSize: Math.round(theme.fontSize * 0.85)
                wrapMode: Text.WordWrap
                lineHeight: 1.15
                color: theme.foreground
                opacity: 0.7
            }
        }
    }

    Command {
        name: "grid.order"
        text: qsTr("Photo order")
        section: qsTr("Photo order")
        keys: ["Ctrl+Shift+O"]
        keyLayer: "grid"
        usable: root.available
        buttons: 1
        onTriggered: root.toggle(true)
    }
    Command {
        name: "order.close"
        keys: ["Escape"]
        keyLayer: "menu"
        usable: root.open
        buttons: 1
        onTriggered: root.open = false
    }
    Command {
        name: "order.up"
        keys: ["Up"]
        keyLayer: "menu"
        usable: root.open
        autoRepeat: true
        buttons: 1
        onTriggered: { root.pointerInput = false; root.current = Math.max(0, root.current - 1) }
    }
    Command {
        name: "order.down"
        keys: ["Down"]
        keyLayer: "menu"
        usable: root.open
        autoRepeat: true
        buttons: 1
        onTriggered: { root.pointerInput = false; root.current = Math.min(root.options.length - 1, root.current + 1) }
    }
    Command {
        name: "order.choose"
        keys: ["Return", "Enter"]
        keyLayer: "menu"
        usable: root.open
        buttons: 1
        onTriggered: root.choose(root.current >= 0 ? root.current
                                  : Math.max(0, root.options.findIndex(option => option.value === root.value)))
    }
}
