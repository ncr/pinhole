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
        const at = current >= 0 ? current : Math.max(0, options.findIndex(option => option.value === value))
        optionRows.itemAt(at)?.forceActiveFocus(Qt.TabFocusReason)
    }
    onOpenChanged: {
        if (open) {
            returnFocus = root.Window.window?.activeFocusItem
            Qt.callLater(focusCurrent)
        } else Qt.callLater(() => { if (returnFocus?.visible && returnFocus.enabled) returnFocus.forceActiveFocus() })
    }
    onCurrentChanged: if (open && !pointerInput) Qt.callLater(focusCurrent)
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
        onClicked: root.open = false
    }
    Card {
        id: card
        objectName: "orderMenuCard"
        width: Math.min(root.width - 24, Math.round(theme.fontSize * 25))
        height: content.height + 2 * padding
        readonly property int padding: Math.round(theme.fontSize * 0.6)
        border.color: Qt.tint(theme.background, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.55))
        // Open below the date, keeping it and the card inside the window.
        x: Math.max(12, Math.min(root.button ? root.button.x : 16, root.width - width - 12))
        y: Math.max(12, Math.min(root.button ? root.button.y + root.button.height + 8 : 16,
                                root.height - height - 12))
        Column {
            id: content
            x: card.padding
            y: card.padding
            width: card.width - 2 * x
            spacing: Math.round(theme.fontSize * 0.3)
            Item {
                width: parent.width
                height: Math.round(theme.fontSize * 1.8)
                UiText {
                    x: Math.round(theme.fontSize * 0.833)
                    y: centredIn(parent.height)
                    text: qsTr("Photo order")
                    font.pixelSize: Math.round(theme.fontSize * 0.8)
                    color: theme.foreground
                    opacity: 0.7
                }
                UiText {
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(theme.fontSize * 0.833)
                    y: centredIn(parent.height)
                    text: "Esc"
                    font.pixelSize: Math.round(theme.fontSize * 0.75)
                    color: theme.foreground
                    opacity: 0.45
                }
            }
            Repeater {
                id: optionRows
                model: root.options
                delegate: ControlSurface {
                    id: option
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.value === modelData.value
                    readonly property bool hot: root.current === index || activeFocus
                    readonly property bool pressed: mouse.pressed || keyboardPressed
                    readonly property int inset: Math.round(theme.fontSize * 0.833)
                    objectName: "orderOption_" + modelData.value
                    width: content.width
                    height: Math.round(theme.fontSize * 2.333)
                    activeFocusOnTab: true
                    onActivated: root.choose(index)
                    onActiveFocusChanged: if (activeFocus && keymap.input === "keyboard") {
                        root.pointerInput = false
                        root.current = index
                    }
                    property alias color: surface.color
                    property alias border: surface.border
                    property alias radius: surface.radius
                    Rectangle {
                        id: surface
                        anchors.fill: parent
                        radius: Math.min(4, theme.radius)
                        // Omarchy controls: pressed > hover/keyboard cursor >
                        // saved selection. The saved choice still has its check.
                        color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b,
                                       option.pressed ? 0.22 : option.hot ? 0.08 : option.selected ? 0.18 : 0)
                        border.width: 1
                        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b,
                                              option.hot ? 0.25 : 0)
                        Behavior on color { ColorAnimation { duration: 100 } }
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                    }
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.title
                    Accessible.description: modelData.detail
                    Accessible.checked: selected
                    Accessible.checkable: true
                    Accessible.focusable: true
                    Accessible.focused: activeFocus
                    Accessible.pressed: pressed
                    Accessible.onPressAction: root.choose(index)
                    UiText {
                        anchors.left: parent.left
                        anchors.leftMargin: option.inset
                        anchors.right: check.left
                        anchors.rightMargin: option.inset
                        y: centredIn(parent.height)
                        text: modelData.title
                        color: option.hot ? theme.accent : theme.foreground
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                    UiText {
                        id: check
                        objectName: "orderCheck_" + modelData.value
                        anchors.right: parent.right
                        anchors.rightMargin: option.inset
                        y: centredIn(parent.height)
                        text: "✓"
                        opacity: option.selected ? 1 : 0
                        color: theme.accent
                        Accessible.ignored: true
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        function point() {
                            root.pointerInput = true
                            root.current = option.index
                        }
                        onEntered: point()
                        onPositionChanged: point()
                        onPressed: point()
                        onExited: if (root.pointerInput && root.current === option.index) root.current = -1
                        onClicked: { option.forceActiveFocus(Qt.MouseFocusReason); root.choose(option.index) }
                    }
                }
            }
            // One explanation, for the row being considered (otherwise the
            // saved choice). Fixed height keeps the card still while browsing.
            Item {
                width: parent.width
                height: Math.round(theme.fontSize * 3.1)
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
    }

    Command {
        name: "grid.order"
        text: qsTr("Photo order")
        section: qsTr("Photo order")
        keys: ["Ctrl+Shift+O"]
        keyLayer: "grid"
        when: root.available
        buttons: 1
        onTriggered: root.toggle(true)
    }
    Command {
        name: "order.close"
        keys: ["Escape"]
        keyLayer: "menu"
        when: root.open
        buttons: 1
        onTriggered: root.open = false
    }
    Command {
        name: "order.up"
        keys: ["Up"]
        keyLayer: "menu"
        when: root.open
        autoRepeat: true
        buttons: 1
        onTriggered: { root.pointerInput = false; root.current = Math.max(0, root.current - 1) }
    }
    Command {
        name: "order.down"
        keys: ["Down"]
        keyLayer: "menu"
        when: root.open
        autoRepeat: true
        buttons: 1
        onTriggered: { root.pointerInput = false; root.current = Math.min(root.options.length - 1, root.current + 1) }
    }
    Command {
        name: "order.choose"
        keys: ["Return", "Enter"]
        keyLayer: "menu"
        when: root.open
        buttons: 1
        onTriggered: root.choose(root.current >= 0 ? root.current
                                  : Math.max(0, root.options.findIndex(option => option.value === root.value)))
    }
}
