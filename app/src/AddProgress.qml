import QtQuick
import Pinhole

// Files dropped on the window on their way into the library, at its foot:
// "Adding 3 files to iCloud…" with how far it has got, then "Added 3
// files" a moment; and what could not be added, a line each, until it is
// clicked away (or Escape, or the next drop).
Rectangle {
    id: card
    readonly property bool shown: incoming.status !== "" || incoming.problems.length > 0

    visible: shown
    width: Math.min(parent.width - 32, theme.fontSize * 40)
    height: content.implicitHeight + 2 * Math.round(theme.fontSize)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.94)
    border.width: 1
    border.color: incoming.failed > 0 && !incoming.busy
                  ? theme.red : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)

    // Put away on a click, once nothing is under way.
    HoverHandler { enabled: !incoming.busy; cursorShape: Qt.PointingHandCursor }
    TapHandler { enabled: !incoming.busy; onTapped: incoming.dismiss() }
    // Over the photos, not a hole in them; beneath its own tap.
    Shield { anchors.fill: parent; z: -1 }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Math.round(theme.fontSize * 1.143)
        anchors.rightMargin: Math.round(theme.fontSize * 1.143)
        spacing: Math.round(theme.fontSize * 0.714)

        Item {
            visible: incoming.status !== ""
            width: parent.width
            height: title.implicitHeight
            UiText {
                id: title
                width: parent.width - percent.implicitWidth - theme.fontSize
                text: incoming.status
                color: theme.brightForeground
                wrapMode: Text.WordWrap
            }
            UiText {
                id: percent
                anchors.right: parent.right
                visible: incoming.busy
                text: Math.round(incoming.progress * 100) + "%"
                font.features: { "tnum": 1 }
            }
        }
        Rectangle {
            visible: incoming.busy
            width: parent.width
            height: Math.max(4, Math.round(theme.fontSize * 0.3))
            radius: height / 2
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
            Rectangle {
                width: parent.width * Math.min(1, incoming.progress)
                height: parent.height
                radius: height / 2
                color: theme.accent
                Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
            }
        }
        UiText {
            visible: incoming.problemsTitle !== ""
            width: parent.width
            text: incoming.problemsTitle
            color: incoming.failed > 0 ? theme.red : theme.brightForeground
            wrapMode: Text.WordWrap
        }
        Repeater {
            // The first few; the count above says how many in all.
            model: incoming.problems.slice(0, 6)
            delegate: UiText {
                required property string modelData
                width: content.width
                text: modelData
                color: theme.darkForeground
                wrapMode: Text.WordWrap
            }
        }
        UiText {
            visible: incoming.problems.length > 6
            width: parent.width
            text: qsTr("and %n more", "", incoming.problems.length - 6)
            color: theme.darkForeground
        }
    }
}
