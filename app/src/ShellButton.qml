import QtQuick
import "keys.js" as KeyNames

// A button as Omarchy's shell draws one (shell.toml's controls): a faint fill
// and a hairline in the text colour, both stronger under the pointer and
// when pressed. `active` is the chosen state, filled with the accent.
// `flat` drops the hairline at rest, for rows inside a bar or menu.
//
// Its key beside the label, as Omarchy's menus show theirs: `key` presses
// it while it shows (a sequence: "C", "Shift+E", "Escape"); `hint` only
// shows one, for a key the window handles itself.
//
// `busy`: on its way, after `busyDelay` an arc turns in the key's place
// (in the label's, without one), as Spinner.qml turns.
Rectangle {
    id: control
    property string text
    property string key
    property string hint: KeyNames.nameOf(key)
    property bool active: false
    property bool flat: false
    // Over the photos rather than on a surface of the app: backed with the
    // theme's background, as the chips are, so it reads on any photo.
    property bool floating: false
    property bool usable: true
    property bool busy: false
    property int busyDelay: 1000
    signal tapped()

    readonly property color ink: theme.foreground
    // Omarchy's control metrics (control-height 28, control-padding-x 10
    // at a base size of 12), with the type scale.
    width: content.width + 2 * Math.round(theme.fontSize * 0.833)
    height: Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    opacity: usable ? 1 : 0.4
    color: active ? theme.accent
         : Qt.rgba(ink.r, ink.g, ink.b, tap.pressed ? 0.22 : hover.hovered && usable ? 0.08 : flat ? 0 : 0.04)
    border.width: active ? 0 : 1
    border.color: Qt.rgba(ink.r, ink.g, ink.b, hover.hovered && usable ? 0.25 : flat ? 0 : 0.4)

    // The arc in the key's place: the key stays, unseen, so the button
    // keeps its width.
    readonly property bool turning: turner.item !== null && turner.item.shown
    Item {
        id: content
        anchors.horizontalCenter: parent.horizontalCenter
        width: control.hint !== "" ? keyLabel.x + keyLabel.implicitWidth : label.implicitWidth
        height: control.height
        UiText {
            id: label
            y: centredIn(control.height)
            color: control.active ? theme.background : theme.foreground
            opacity: control.turning && control.hint === "" ? 0 : 1
            text: control.text
        }
        // A space after the label's ink: a Nerd Font icon is drawn wider
        // than its cell, which the label's width takes in but a space in
        // the same line would not, so the key stands as far from an icon's
        // edge as from a word's.
        TextMetrics { id: spaceMetrics; font: label.font; text: " " }
        UiText {
            id: keyLabel
            visible: control.hint !== ""
            x: Math.round(label.implicitWidth + spaceMetrics.advanceWidth)
            y: label.y
            // The key quieter than the label: the theme's dark text, or on
            // the accent, the background let through.
            color: control.turning ? "transparent"
                 : control.active ? Qt.tint(theme.accent, Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.6))
                 : theme.darkForeground
            text: control.hint
        }
    }
    Loader {
        id: turner
        active: control.busy
        readonly property real middle: control.hint !== "" ? content.x + keyLabel.x + keyLabel.implicitWidth / 2
                                                           : control.width / 2
        x: Math.round(middle - width / 2)
        sourceComponent: Spinner {
            bare: true
            busy: true
            delay: control.busyDelay
            height: control.height
            ink: label.color
        }
    }
    // Not while the window has its keys for something over it (the help).
    readonly property bool keysFree: Window.window === null || Window.window.buttonKeys !== false
    Shortcut {
        enabled: control.key !== "" && control.visible && control.usable && control.keysFree
        sequence: control.key
        context: Qt.ApplicationShortcut
        onActivated: control.tapped()
    }
    Rectangle {
        anchors.fill: parent
        z: -1
        visible: control.floating
        radius: parent.radius
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    }
    HoverHandler { id: hover; cursorShape: control.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { id: tap; onTapped: if (control.usable) control.tapped() }
}
