import QtQuick
import "focus.js" as Focus

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
    Accessible.role: Accessible.Grouping
    Accessible.name: placeholder
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
        activeFocusOnTab: !field.done
        Accessible.name: field.placeholder
        Accessible.description: field.password ? qsTr("Password field") : ""
        Accessible.labelledBy: field
        onActiveFocusChanged: if (activeFocus) Focus.reveal(field)
        clip: true
        Keys.onShortcutOverride: event => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                    || ((event.modifiers & Qt.ControlModifier) && [Qt.Key_Z, Qt.Key_Y, Qt.Key_C, Qt.Key_X, Qt.Key_V, Qt.Key_A].includes(event.key)))
                event.accepted = true
        }
        onAccepted: field.accepted()
    }
    UiText {
        anchors.left: input.left
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text === "" && !input.preeditText
        anchors.right: input.right
        elide: Text.ElideRight
        text: field.placeholder
        color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.7)
        Accessible.ignored: true
    }
    UiText {
        id: tick
        anchors.right: parent.right
        anchors.rightMargin: Math.round(theme.fontSize * 0.833)
        anchors.verticalCenter: parent.verticalCenter
        text: field.done ? "✓" : ""
        color: theme.accent
        Accessible.ignored: true
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
