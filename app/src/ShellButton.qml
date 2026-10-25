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
    signal tapped()

    readonly property color ink: theme.foreground
    // Omarchy's control metrics (control-height 28, control-padding-x 10
    // at a base size of 12), with the type scale.
    width: label.implicitWidth + 2 * Math.round(theme.fontSize * 0.833)
    height: Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    opacity: usable ? 1 : 0.4
    color: active ? theme.accent
         : Qt.rgba(ink.r, ink.g, ink.b, tap.pressed ? 0.22 : hover.hovered && usable ? 0.08 : flat ? 0 : 0.04)
    border.width: active ? 0 : 1
    border.color: Qt.rgba(ink.r, ink.g, ink.b, hover.hovered && usable ? 0.25 : flat ? 0 : 0.4)

    UiText {
        id: label
        anchors.horizontalCenter: parent.horizontalCenter
        y: centredIn(control.height)
        color: control.active ? theme.background : theme.foreground
        // The key quieter than the label: the theme's dark text, or on the
        // accent, the background let through.
        readonly property color hintColor: control.active
            ? Qt.tint(theme.accent, Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.6))
            : theme.darkForeground
        text: control.text + (control.hint !== "" ? "  <font color=\"" + hintColor + "\">" + control.hint + "</font>" : "")
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
