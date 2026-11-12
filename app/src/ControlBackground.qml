import QtQuick

// Shared Omarchy control states. A saved menu choice is quieter than a
// selected button; hover and press always remain visible over either.
Rectangle {
    id: surface
    property bool chosen: false
    property bool strongSelection: false
    property bool flat: false
    property bool hot: false
    property bool down: false
    property bool keyboardFocus: false
    readonly property color ink: theme.foreground
    radius: Math.min(4, theme.radius)
    color: chosen && strongSelection
         ? Qt.tint(theme.accent, Qt.rgba(ink.r, ink.g, ink.b, down ? 0.18 : hot ? 0.08 : 0))
         : Qt.rgba(ink.r, ink.g, ink.b, down ? 0.22 : hot ? 0.08 : chosen ? 0.18 : flat ? 0 : 0.04)
    border.width: chosen && strongSelection ? 0 : 1
    border.color: Qt.rgba(ink.r, ink.g, ink.b, hot ? 0.25 : flat ? 0 : 0.4)
    Behavior on color { ColorAnimation { duration: 100 } }
    Behavior on border.color { ColorAnimation { duration: 100 } }
    Rectangle {
        // Inside its own edge: in a bar or a menu it stays within the card.
        anchors.fill: parent
        radius: surface.radius
        color: "transparent"
        border.width: 2
        border.color: theme.accent
        visible: surface.keyboardFocus
    }
}
