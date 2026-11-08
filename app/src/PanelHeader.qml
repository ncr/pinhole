import QtQuick

// The title bar of a plugin's view over the whole window, as the
// map's, search's and People's: its `title` in the larger size, a quieter
// `detail` after it, the `tools` (buttons, a field) at the right, then ✕
// with its key, which `closed()` says. Items given as its children go
// before the title (a ‹ back button); `progress`, 0 to 1, draws a line
// along its foot while it is under 1 (-1 for none).
Item {
    id: header
    property string title
    property string detail
    // The Command that closes the view: ✕ presses it and shows its key.
    property QtObject closeCommand: null
    // The PluginView it is in, if any: ✕ closes that, from any level of
    // it (Esc may mean back there, its back()).
    readonly property Item pluginView: {
        for (let at = header.parent; at; at = at.parent)
            if (at.closeCommand !== undefined && at.toggleKey !== undefined)
                return at
        return null
    }
    // ✕'s key without a closeCommand: "" where Esc means back, not close.
    property string closeHint: "Esc"
    // A control's height here, for what stands beside the buttons.
    readonly property real controlHeight: close.height
    // What a field in place of the title (search's question) may take.
    readonly property real room: width - 24 - 12 - toolbar.width - 6 - close.width - (toolbar.width ? 6 : 0)
    property real progress: -1
    property color progressColor: theme.accent
    default property alias leading: before.data
    property alias tools: toolbar.data
    signal closed()
    width: parent ? parent.width : 0
    height: close.height + 16

    Row {
        id: before
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12
    }
    Row {
        anchors.left: before.right
        anchors.leftMargin: before.width > 0 ? 12 : 2
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12
        UiText {
            id: titleText
            width: Math.min(implicitWidth, header.width - before.width - toolbar.width - detailText.width - 60)
            elide: Text.ElideRight
            font.pixelSize: Math.round(theme.fontSize * 1.35)
            color: theme.brightForeground
            text: header.title
        }
        UiText {
            id: detailText
            anchors.verticalCenter: titleText.verticalCenter
            visible: text !== ""
            color: theme.darkForeground
            text: header.detail
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        Row {
            id: toolbar
            spacing: 6
        }
        ShellButton {
            id: close
            objectName: "panelClose"
            text: "✕"
            action: header.closeCommand
            hint: header.closeCommand ? header.closeCommand.keyName : header.closeHint
            onTriggered: {
                if (header.closeCommand)
                    return
                if (header.pluginView)
                    header.pluginView.close()
                header.closed()
            }
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 2
        visible: header.progress >= 0 && header.progress < 1
        color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
        Rectangle {
            height: parent.height
            width: parent.width * Math.max(0, header.progress)
            color: header.progressColor
            Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
        }
    }
}
