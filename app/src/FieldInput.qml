import QtQuick

// A line to type into, as the rest of the shell draws its controls: a
// hairline that lights in the accent while typing, the hint in the muted
// text colour until something is typed, and a tick once it is settled.
Rectangle {
    id: field
    property alias text: input.text
    property alias input: input
    property string placeholder
    property bool password: false
    property bool done: false
    signal accepted()

    width: parent ? parent.width : 300
    height: Math.round(theme.fontSize * 2.857)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, input.activeFocus ? 0.06 : 0.03)
    border.width: 1
    border.color: input.activeFocus ? theme.accent
                : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, done ? 0.18 : 0.4)

    TextInput {
        id: input
        anchors.left: parent.left
        anchors.right: tick.left
        anchors.leftMargin: Math.round(theme.fontSize * 0.833)
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        font.family: theme.font
        font.pixelSize: theme.fontSize
        color: field.done ? theme.darkForeground : theme.brightForeground
        selectionColor: theme.selection
        selectedTextColor: theme.brightForeground
        echoMode: field.password ? TextInput.Password : TextInput.Normal
        passwordCharacter: "•"
        readOnly: field.done
        clip: true
        onAccepted: field.accepted()
    }
    UiText {
        anchors.left: input.left
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text === "" && !input.preeditText
        text: field.placeholder
        color: theme.muted
    }
    UiText {
        id: tick
        anchors.right: parent.right
        anchors.rightMargin: Math.round(theme.fontSize * 0.833)
        anchors.verticalCenter: parent.verticalCenter
        text: field.done ? "✓" : ""
        color: theme.accent
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.IBeamCursor
        onPressed: mouse => {
            input.forceActiveFocus()
            mouse.accepted = false
        }
    }
}
