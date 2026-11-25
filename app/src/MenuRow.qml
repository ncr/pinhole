import QtQuick
import Pinhole
import "focus.js" as Focus

// Internal menu row, shared by action menus and saved radio preferences.
ControlSurface {
    id: row
    property string text
    property string hint
    // A word after its text, dimmer: where it is from ("Mosaic").
    property string detail
    property bool selected: false
    property bool checkable: false
    property bool reserveCheck: checkable
    property bool lit: false
    property bool busy: false
    property int busyDelay: 1000
    readonly property bool turning: indicator.item !== null && indicator.item.shown
    readonly property bool hovered: mouse.containsMouse && visible
    readonly property bool hot: usable && (hovered || lit)
    readonly property bool pressed: usable && (mouse.pressed || keyboardPressed)
    readonly property int inset: Math.round(theme.fontSize * 0.833)
    property alias color: surface.color
    property alias border: surface.border
    property alias radius: surface.radius
    activeFocusOnTab: usable
    width: parent.width
    height: Math.round(theme.fontSize * 2.333)
    opacity: usable ? 1 : 0.4
    Accessible.role: Accessible.MenuItem
    Accessible.name: text
    Accessible.description: hint !== "" ? qsTr("Shortcut: %1").arg(hint) : ""
    Accessible.focusable: usable
    Accessible.focused: activeFocus
    Accessible.selected: selected
    Accessible.checkable: checkable
    Accessible.checked: checkable && selected
    Accessible.pressed: pressed
    Accessible.onPressAction: press()
    signal tapped()
    signal pointed()
    function press() { if (usable && enabled) tapped() }
    onActivated: press()
    Connections {
        target: row
        function onActiveFocusChanged() { if (row.activeFocus) Focus.reveal(row) }
    }
    ControlBackground {
        id: surface
        anchors.fill: parent
        flat: true
        chosen: row.selected
        hot: row.hot
        down: row.pressed
    }
    UiText {
        id: label
        anchors.left: parent.left
        anchors.leftMargin: row.inset
        // As wide as the row leaves it; with a detail, no wider than it is.
        width: Math.max(0, Math.min(row.detail !== "" ? implicitWidth : Infinity, trailing.x - row.inset - x))
        y: centredIn(parent.height)
        text: row.text
        color: row.hot || row.selected ? theme.accent : theme.foreground
        elide: Text.ElideRight
        Accessible.ignored: true
    }
    UiText {
        visible: row.detail !== "" && width > theme.fontSize
        anchors.left: label.right
        anchors.leftMargin: Math.round(theme.fontSize * 0.6)
        anchors.right: trailing.left
        anchors.rightMargin: row.inset
        y: centredIn(parent.height)
        text: row.detail
        color: theme.darkForeground
        elide: Text.ElideRight
        Accessible.ignored: true
    }
    Row {
        id: trailing
        anchors.right: parent.right
        anchors.rightMargin: row.inset
        height: parent.height
        spacing: Math.round(theme.fontSize * 0.6)
        UiText {
            visible: row.hint !== ""
            y: centredIn(parent.height)
            text: row.hint
            color: theme.darkForeground
            opacity: row.turning ? 0 : 1
            Accessible.ignored: true
        }
        UiText {
            objectName: row.objectName + "Check"
            visible: row.reserveCheck || row.selected
            y: centredIn(parent.height)
            text: "✓"
            opacity: row.selected && !row.turning ? 1 : 0
            color: theme.accent
            Accessible.ignored: true
        }
    }
    Loader {
        id: indicator
        active: row.busy
        anchors.right: parent.right
        anchors.rightMargin: row.inset
        sourceComponent: Spinner { bare: true; busy: true; delay: row.busyDelay; height: row.height; ink: theme.accent }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: row.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onEntered: row.pointed()
        onPositionChanged: row.pointed()
        onPressed: row.pointed()
        onClicked: { if (row.usable) row.forceActiveFocus(Qt.MouseFocusReason); row.press() }
    }
}
