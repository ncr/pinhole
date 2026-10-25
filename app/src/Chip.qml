import QtQuick

// A label over the photos, as Omarchy's shell draws its cards: the theme's
// background, nearly opaque, a hairline in the text colour, square corners
// (or the theme's own rounding).
Rectangle {
    id: chip
    property alias text: label.text
    property alias label: label
    property color textColor: theme.foreground

    // Omarchy's control metrics (shell.toml: control-height 28 and
    // control-padding-x 10 at a base size of 12), with the type scale.
    width: label.implicitWidth + 2 * Math.round(theme.fontSize * 0.833)
    height: label.lineCount > 1 ? label.implicitHeight + theme.fontSize : Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    border.width: 1
    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)

    UiText {
        id: label
        anchors.horizontalCenter: parent.horizontalCenter
        y: centredIn(chip.height)
        color: chip.textColor
    }
}
