import QtQuick
import Pinhole

// A card: a box in the theme's background with a hairline round it, that
// lets nothing over it through to what is beneath (Shield). Its controls go
// in it. Over the photos for a moment (a sign-in, a question), on a dimmed
// Panel whose click puts it away:
//
//     Panel { color: Qt.rgba(0, 0, 0, 0.45); onTriggered: plugin.close() }
//     Card { anchors.centerIn: parent; width: 400; height: 300 }
Rectangle {
    radius: Math.min(8, theme.radius)
    color: theme.background
    border.width: 1
    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25)
    Shield { anchors.fill: parent; z: -1 }
}
