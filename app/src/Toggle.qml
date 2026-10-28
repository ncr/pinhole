import QtQuick

// A switch for something on or off, as Omarchy's shell draws one: a track
// in the accent when on, the knob sliding across. Its key beside it, as the
// buttons show theirs.
Item {
    id: toggle
    property bool checked: false
    property string hint
    signal toggled()

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
        color: toggle.checked ? theme.accent
             : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, hover.hovered ? 0.22 : 0.14)
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
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: toggle.toggled() }
}
