import QtQuick
import Pinhole
import "focus.js" as Focus

// A switch for something on or off, as Omarchy's shell draws one: a track
// in the accent when on, the knob sliding across. Its key beside it, as the
// buttons show theirs.
ControlSurface {
    id: toggle
    property bool checked: false
    property string hint
    // Chosen with the keys: a ring around it, in the accent.
    property bool lit: false
    // Not now: dimmed, as a ShellButton is, and taps do nothing.
    activeFocusOnTab: usable
    Accessible.role: Accessible.CheckBox
    Accessible.name: {
        if (parent)
            for (const sibling of parent.children)
                if (sibling !== toggle && sibling.visible && sibling.text) return sibling.text
        return qsTr("Switch")
    }
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.focusable: usable
    Accessible.focused: activeFocus
    Accessible.pressed: usable && (tap.pressed || keyboardPressed)
    Accessible.onToggleAction: press()
    Accessible.onPressAction: press()
    onActivated: press()
    Connections {
        target: toggle
        function onActiveFocusChanged() { if (toggle.activeFocus) Focus.reveal(toggle) }
    }
    signal toggled()
    function press() {
        if (usable && enabled)
            toggled()
    }
    opacity: usable ? 1 : 0.4

    implicitWidth: track.width + (hintText.visible ? hintText.implicitWidth + Math.round(theme.fontSize * 0.6) : 0)
    implicitHeight: Math.round(theme.fontSize * 2.333)

    UiText {
        id: hintText
        visible: toggle.hint !== ""
        anchors.right: track.left
        anchors.rightMargin: Math.round(theme.fontSize * 0.6)
        y: centredIn(toggle.height)
        text: toggle.hint
        color: theme.darkForeground
    }
    Rectangle {
        id: track
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Math.round(theme.fontSize * 2.6)
        height: Math.round(theme.fontSize * 1.4)
        radius: height / 2
        color: toggle.checked ? Qt.tint(theme.accent, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, toggle.usable && (tap.pressed || toggle.keyboardPressed) ? 0.18 : hover.hovered && toggle.usable ? 0.08 : 0))
             : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, toggle.usable && (tap.pressed || toggle.keyboardPressed) ? 0.3 : hover.hovered && toggle.usable ? 0.22 : 0.14)
        Behavior on color { ColorAnimation { duration: 120 } }
        Rectangle {
            readonly property real inset: 3
            width: parent.height - 2 * inset
            height: width
            radius: width / 2
            y: inset
            x: toggle.checked ? parent.width - width - inset : inset
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            color: toggle.checked ? theme.background : theme.foreground
        }
    }
    Rectangle {
        visible: toggle.lit || (toggle.activeFocus && keymap.input === "keyboard")
        anchors.centerIn: track
        width: track.width + 6
        height: track.height + 6
        radius: height / 2
        color: "transparent"
        border.width: 2
        border.color: theme.accent
    }
    MouseArea {
        id: hover
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        readonly property bool hovered: containsMouse
        cursorShape: toggle.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler { id: tap; onTapped: { if (toggle.usable) toggle.forceActiveFocus(Qt.MouseFocusReason); toggle.press() } }
}
