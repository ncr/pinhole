import QtQuick

// A label over the photos, as Omarchy's shell draws its cards: the theme's
// background, nearly opaque, a hairline in the text colour, square corners
// (or the theme's own rounding).
Rectangle {
    id: chip
    property string text
    property alias label: label
    property color textColor: theme.foreground
    // While true, a new text morphs from the old: what the two share at the
    // start stays, what they share at the end slides to its new place, and
    // only what differs between cross-fades ("24–28 July" to "25–28 July"
    // changes the one digit). The chip's width eases to fit. Texts coming
    // faster than that (scrolling quickly past the days) change at once,
    // as a counter does: morphing one after another never settled and
    // flickered.
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
        enabled: chip.morphing && !chip.ticking
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
    }

    // What the label shows: the text, or on its way to it.
    property string shownText
    // A change in parts: the shared start, the part going and the part
    // coming, the shared end; and how far it has gone, 0 to 1.
    property string morphStart
    property string morphGoing
    property string morphComing
    property string morphEnd
    property real morphed: 1
    function change() {
        const from = shownText, to = text
        let start = 0
        while (start < from.length && start < to.length && from[start] === to[start])
            ++start
        let end = 0
        while (end < from.length - start && end < to.length - start &&
               from[from.length - 1 - end] === to[to.length - 1 - end])
            ++end
        morphStart = to.slice(0, start)
        morphGoing = from.slice(start, from.length - end)
        morphComing = to.slice(start, to.length - end)
        morphEnd = to.slice(to.length - end)
        shownText = to
        morph.restart()
    }
    onTextChanged: {
        if (!chip.morphing || shownText === "" || text === "" || morph.running || quick.running) {
            ticking = chip.morphing && (morph.running || quick.running)
            morph.stop()
            morphed = 1
            shownText = text
        } else {
            change()
        }
        quick.restart()
    }
    // Since the last change: within this, the next is quick.
    Timer { id: quick; interval: 250; onTriggered: chip.ticking = false }
    // Changing at once, the chip's edge too, so it hugs the text.
    property bool ticking: false
    Component.onCompleted: shownText = text

    readonly property int padding: Math.round(theme.fontSize * 0.833)

    // Inside the hairline: a wider old text stays in while the chip narrows.
    Item {
        x: chip.border.width
        y: chip.border.width
        width: chip.width - 2 * x
        height: chip.height - 2 * y
        clip: chip.morphing

        UiText {
            id: label
            // On whole pixels, so the type stays crisp.
            x: chip.centred ? Math.round((parent.width - implicitWidth) / 2) : chip.padding - parent.x
            y: centredIn(chip.height) - parent.y
            color: chip.textColor
            text: chip.shownText
            visible: chip.morphed >= 1
        }
        // While changing, the label in its parts, each where it falls in the
        // whole text, the end moving from where it was to where it goes.
        Item {
            visible: chip.morphed < 1
            UiText {
                x: label.x
                y: label.y
                color: chip.textColor
                font: label.font
                text: chip.morphStart
            }
            UiText {
                x: label.x + metrics.advanceWidth(chip.morphStart)
                y: label.y
                color: chip.textColor
                font: label.font
                text: chip.morphGoing
                opacity: 1 - Math.min(1, chip.morphed / 0.5)
            }
            UiText {
                x: label.x + metrics.advanceWidth(chip.morphStart)
                y: label.y
                color: chip.textColor
                font: label.font
                text: chip.morphComing
                opacity: Math.max(0, (chip.morphed - 0.3) / 0.7)
            }
            UiText {
                readonly property real from: metrics.advanceWidth(chip.morphStart + chip.morphGoing)
                readonly property real to: metrics.advanceWidth(chip.morphStart + chip.morphComing)
                x: label.x + from + (to - from) * chip.slid
                y: label.y
                color: chip.textColor
                font: label.font
                text: chip.morphEnd
            }
        }
    }
    FontMetrics { id: metrics; font: label.font }
    // The end slides as the chip's edge does (out cubic).
    readonly property real slid: 1 - Math.pow(1 - morphed, 3)
    NumberAnimation {
        id: morph
        target: chip
        property: "morphed"
        from: 0
        to: 1
        duration: 200
    }
}
