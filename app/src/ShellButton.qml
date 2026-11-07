import QtQuick
import Pinhole
import "keys.js" as KeyNames
import "focus.js" as Focus

// A button as Omarchy's shell draws one (shell.toml's controls): a faint fill
// and a hairline in the text colour, both stronger under the pointer and
// when pressed. `active` is the chosen state, filled with the accent.
// `flat` drops the hairline at rest, for rows inside a bar or menu.
//
// Its key beside the label, as Omarchy's menus show theirs. Given an
// `action` (Command.qml), it presses that and shows its key; else `key`
// presses it while it shows (a sequence: "C", "Shift+E", "Escape"), on
// `keyLayer`'s keys, and `section` and `helpText` put it in the help. `hint`
// only shows a key, for one the window handles itself.
//
// `busy`: on its way, after `busyDelay` an arc turns in the key's place
// (in the label's, without one), as Spinner.qml turns.
//
// `justified`, for a row of a menu: the label at the left, the key at the
// right, as wide as the menu makes it; `lit`, chosen there with the keys,
// lit as under the pointer.
ControlSurface {
    id: control
    property string text
    property var action: null
    usable: !action || action.usable !== false
    property string key
    property string keyLayer: "chrome"
    property string section
    property string helpText: text
    property string hint: action ? action.keyName : KeyNames.nameOf(key)
    property bool active: false
    // The label's colour when not active: the theme's text, or the accent
    // for a ♥ that is filled.
    property color labelColor: theme.foreground
    property bool flat: false
    // Over the photos rather than on a surface of the app: backed with the
    // theme's background, as the chips are, so it reads on any photo.
    property bool floating: false
    property bool busy: false
    property int busyDelay: 1000
    property bool justified: false
    property bool lit: false
    property bool checkable: false
    property alias color: surface.color
    property alias radius: surface.radius
    property alias border: surface.border
    activeFocusOnTab: usable
    Accessible.role: Accessible.Button
    Accessible.name: {
        const label = action && action.text ? action.text : helpText !== "" ? helpText : text
        return ["✕", "×"].includes(label) ? qsTr("Close") : label === "▾" ? qsTr("More options") : label
    }
    Accessible.description: hint !== "" ? qsTr("Shortcut: %1").arg(hint) : ""
    Accessible.focusable: usable
    Accessible.focused: activeFocus
    Accessible.checkable: checkable
    Accessible.checked: checkable && active
    Accessible.pressed: usable && (tap.pressed || keyboardPressed)
    Accessible.onPressAction: press()
    onActivated: press()
    Connections {
        target: control
        function onActiveFocusChanged() { if (control.activeFocus) Focus.reveal(control) }
    }
    // Its space at each side of what it says.
    property int inset: Math.round(theme.fontSize * 0.833)
    // The pointer over it, while it shows.
    readonly property bool hovered: hover.hovered && visible
    // Pressed: a click, a tap, its key, as an action and a Command say it.
    signal triggered()
    // As a tap does: from the keys of a panel it is a stop in.
    function press() {
        if (!usable || !enabled)
            return
        if (action)
            action.trigger()
        triggered()
    }

    readonly property color ink: theme.foreground
    // Omarchy's control metrics (control-height 28, control-padding-x 10
    // at a base size of 12), with the type scale.
    readonly property real naturalWidth: (hint === "" ? label.implicitWidth
        : label.implicitWidth + (justified ? 2 * theme.fontSize : spaceMetrics.advanceWidth) + keyLabel.implicitWidth) + 2 * inset
    width: naturalWidth
    height: Math.round(theme.fontSize * 2.333)
    opacity: usable ? 1 : 0.4
    readonly property bool lighter: (hover.hovered || lit || (activeFocus && keymap.input === "keyboard")) && usable
    ControlBackground {
        id: surface
        anchors.fill: parent
        chosen: control.active
        strongSelection: true
        flat: control.flat
        hot: control.lighter
        down: control.usable && (tap.pressed || control.keyboardPressed)
        keyboardFocus: control.activeFocus && keymap.input === "keyboard"
    }

    // The arc in the key's place: the key stays, unseen, so the button
    // keeps its width.
    readonly property bool turning: turner.item !== null && turner.item.shown
    Item {
        id: content
        x: control.justified ? control.inset : Math.round((control.width - width) / 2)
        width: control.justified ? control.width - 2 * control.inset
             : control.hint !== "" ? keyLabel.x + keyLabel.implicitWidth : label.implicitWidth
        height: control.height
        UiText {
            id: label
            y: centredIn(control.height)
            color: control.active ? theme.background : control.labelColor
            opacity: control.turning && control.hint === "" ? 0 : 1
            text: control.text
            Accessible.ignored: true
        }
        // A space after the label's ink: a Nerd Font icon is drawn wider
        // than its cell, which the label's width takes in but a space in
        // the same line would not, so the key stands as far from an icon's
        // edge as from a word's.
        TextMetrics { id: spaceMetrics; font: label.font; text: " " }
        UiText {
            id: keyLabel
            visible: control.hint !== ""
            x: control.justified ? Math.round(content.width - implicitWidth)
                                 : Math.round(label.implicitWidth + spaceMetrics.advanceWidth)
            y: label.y
            // The key quieter than the label: the theme's dark text, or on
            // the accent, the background let through.
            color: control.turning ? "transparent"
                 : control.active ? Qt.tint(theme.accent, Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.6))
                 : theme.darkForeground
            text: control.hint
            Accessible.ignored: true
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
    // Its own key, when it has no action to press: a Command of its own.
    Loader {
        active: control.action === null && control.key !== ""
        sourceComponent: Command {
            name: "button." + (control.objectName !== "" ? control.objectName : control.helpText)
            text: control.helpText
            section: control.section
            keys: [control.key]
            keyLayer: control.keyLayer
            usable: control.visible && control.usable
            buttons: 1
            onTriggered: control.triggered()
        }
    }
    // The action's way for a pointer, while this shows it.
    property var counted: null
    function count() {
        if (counted === action)
            return
        if (counted)
            --counted.buttons
        counted = action
        if (counted)
            ++counted.buttons
    }
    onActionChanged: count()
    Component.onCompleted: count()
    Component.onDestruction: if (counted) --counted.buttons
    Rectangle {
        anchors.fill: parent
        z: -1
        visible: control.floating
        radius: parent.radius
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    }
    // Floating over the photos, all of it is the button's: the wheel or a
    // pinch on it, or a click while it cannot be used, reach nothing beneath.
    Shield {
        anchors.fill: parent
        z: -1
        enabled: control.floating
    }
    MouseArea {
        id: hover
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        readonly property bool hovered: containsMouse
        cursorShape: control.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler {
        id: tap
        // The press is the button's alone: a button over something that
        // takes taps itself (✕ on a photo) does not press that too. A
        // list it is in can still take a drag over.
        gesturePolicy: TapHandler.WithinBounds
        onTapped: {
            if (control.usable) control.forceActiveFocus(Qt.MouseFocusReason)
            control.press()
        }
    }

    // What pressing it could not do, in a few words under it for four
    // seconds, the button shaking, as a PluginAction's complain() says it
    // under its button (Complaint.qml).
    function complain(text) {
        complaint.active = true
        complaint.item.say(text)
    }
    transform: Translate { id: complaintNudge }
    Loader {
        id: complaint
        active: false
        z: 10
        sourceComponent: Complaint { button: control; nudged: complaintNudge }
    }
}
