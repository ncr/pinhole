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
    // eases to fit it. A text that comes while one is changing waits for
    // it, the latest one then following: changes faster than that show at
    // that pace rather than each restarting the fade, which flickered.
    property bool morphing: false
    // The text inside, centred; or from the left, the chip's right edge
    // moving as it changes.
    property bool centred: true

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

    // What the label shows: the text, or on its way to it.
    property string shownText
    function change() {
        leaving.text = shownText
        shownText = text
        morph.restart()
    }
    onTextChanged: {
        if (!chip.morphing || shownText === "" || text === "") {
            morph.stop()
            label.opacity = 1
            leaving.opacity = 0
            shownText = text
        } else if (!morph.running) {
            change()
        }
    }
    Component.onCompleted: shownText = text

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
            // On whole pixels, so the type stays crisp.
            x: chip.centred ? Math.round((parent.width - implicitWidth) / 2) : chip.padding - parent.x
            y: centredIn(chip.height) - parent.y
            color: chip.textColor
            text: chip.shownText
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
        onFinished: {
            if (chip.shownText !== chip.text)
                chip.change()
        }
    }
}
