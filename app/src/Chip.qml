import QtQuick

// A label over the photos, as Omarchy's shell draws its cards: the theme's
// background, nearly opaque, a hairline in the text colour, square corners
// (or the theme's own rounding).
Rectangle {
    id: chip
    property string text
    property alias label: label
    property color textColor: theme.foreground
    // While true, a new text cross-fades with the old and the chip's width
    // eases to fit it, as the grid's levels cross-fade while zooming.
    property bool morphing: false

    // Omarchy's control metrics (shell.toml: control-height 28 and
    // control-padding-x 10 at a base size of 12), with the type scale.
    width: label.implicitWidth + 2 * padding
    height: label.lineCount > 1 ? label.implicitHeight + theme.fontSize : Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    border.width: 1
    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)

    Behavior on width {
        enabled: chip.morphing
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
    }

    property string shownText
    onTextChanged: {
        if (chip.morphing && shownText !== "" && text !== "") {
            leaving.text = shownText
            morph.restart()
        }
        shownText = text
    }

    readonly property int padding: Math.round(theme.fontSize * 0.833)

    // Inside the hairline: the old text, wider, stays in while the chip
    // narrows.
    Item {
        x: chip.border.width
        y: chip.border.width
        width: chip.width - 2 * x
        height: chip.height - 2 * y
        clip: morph.running

        UiText {
            id: label
            // Morphing, both texts start at the left, where the chip does,
            // and its right edge moves.
            anchors.horizontalCenter: chip.morphing ? undefined : parent.horizontalCenter
            x: chip.padding - parent.x
            y: centredIn(chip.height) - parent.y
            color: chip.textColor
            text: chip.text
        }
        // The text going, in the same place and type.
        UiText {
            id: leaving
            x: label.x
            y: label.y
            color: chip.textColor
            font: label.font
            opacity: 0
        }
    }
    // The old text mostly gone before the new comes up, so the two hardly
    // overlap.
    ParallelAnimation {
        id: morph
        NumberAnimation { target: leaving; property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.OutQuad }
        SequentialAnimation {
            PropertyAction { target: label; property: "opacity"; value: 0 }
            PauseAnimation { duration: 60 }
            NumberAnimation { target: label; property: "opacity"; to: 1; duration: 150; easing.type: Easing.OutQuad }
        }
    }
}
