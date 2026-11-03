import QtQuick
import QtQuick.Window
import "focus.js" as Focus

// The same heading, padding and bounded, scrollable surface for every menu.
Card {
    id: card
    default property alias content: body.data
    property string title
    property real preferredWidth: Math.round(theme.fontSize * 25)
    property real maximumHeight: (Window.window?.height ?? implicitHeight + 24) - 24
    readonly property int padding: Math.round(theme.fontSize * 0.6)
    readonly property int gap: Math.round(theme.fontSize * 0.3)
    readonly property real bodyWidth: width - 2 * padding
    width: Math.min(preferredWidth, (Window.window?.width ?? preferredWidth + 24) - 24)
    implicitHeight: header.height + gap + body.implicitHeight + 2 * padding
    height: Math.min(implicitHeight, maximumHeight)
    radius: Math.min(4, theme.radius)
    border.color: Qt.tint(theme.background, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.55))
    function revealFocus() {
        const focused = card.Window.window?.activeFocusItem
        for (let item = focused; item; item = item.parent)
            if (item === card) { Focus.reveal(focused); return }
    }
    onHeightChanged: Qt.callLater(revealFocus)
    Item {
        id: header
        x: card.padding
        y: card.padding
        width: card.bodyWidth
        height: Math.round(theme.fontSize * 1.8)
        UiText {
            x: Math.round(theme.fontSize * 0.833)
            y: centredIn(parent.height)
            width: parent.width - 3 * x - escapeHint.width
            text: card.title
            elide: Text.ElideRight
            font.pixelSize: Math.round(theme.fontSize * 0.8)
            color: theme.foreground
            opacity: 0.7
        }
        UiText {
            id: escapeHint
            anchors.right: parent.right
            anchors.rightMargin: Math.round(theme.fontSize * 0.833)
            y: centredIn(parent.height)
            text: "Esc"
            font.pixelSize: Math.round(theme.fontSize * 0.75)
            color: theme.foreground
            opacity: 0.45
        }
    }
    Flickable {
        x: card.padding
        y: header.y + header.height + card.gap
        width: card.bodyWidth
        height: Math.max(0, card.height - y - card.padding)
        contentHeight: body.implicitHeight
        onContentHeightChanged: Qt.callLater(card.revealFocus)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Column {
            id: body
            width: card.bodyWidth
            spacing: card.gap
        }
    }
}
