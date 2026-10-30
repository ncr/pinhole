import QtQuick
import "keys.js" as KeyNames

// One thing Pinhole does, and every way to do it (keymap.h): its keys, and
// how it goes with a mouse and with a touchpad. The keys work while its
// `keyLayer` has them (the one on top: the grid, the viewer, a menu, a panel)
// and `when` allows; a ShellButton given it as its `action` presses it, and
// counts as the way there for a pointer. In the help, under its `section`,
// if it has one.
Item {
    id: action

    // For tests and the coverage check: "grid.open", "viewer.close".
    property string name
    // What it does, for the help: "Open", "Newest".
    property string text
    // Its part of the help: "library", "select", "photo", "video", "share",
    // "pinhole", a panel's ("sources", "storage", "signin", "tour"), or a
    // plugin's name. None: not in the help (a button's own key, shown on it).
    property string section
    // The keys that do it, as sequences ("Space", "Shift+Up"); the first is
    // the one named, unless `keysText` says it better ("1–9", "↑ ↓").
    property var keys: []
    property string keysText
    // How with a mouse, and with a touchpad, when not by a button it has:
    // "Wheel", "Pinch". A ShellButton showing it is way enough.
    property string mouse
    property string touchpad
    // Why there is none, for an action that cannot have one: typing a
    // password needs a keyboard. Counts as covered.
    property string noKeyboard
    property string noMouse
    property string noTouchpad
    // Whose keys these are: "grid", "viewer", "menu", "side", "addSource",
    // "firstRun", "signIn", "tour", "help", "plugin"; "chrome" for a
    // button's, on the grid or the viewer, whichever has them; "any" for
    // the window's own.
    property string keyLayer: "chrome"
    property bool when: true
    property bool autoRepeat: false
    // False for keys read elsewhere (the viewer's arrows, timed as they are
    // held): listed, but no shortcut of its own.
    property bool bound: true
    // Which of the help's tabs list it: the keys', and the mouse's and the
    // touchpad's (where its `mouse` and `touchpad` say how, briefly).
    property bool keyHelp: true
    property bool pointerHelp: true
    // Where it is written (keymap.h sets them), for the help's order.
    property string file
    property int line: 0
    // ShellButtons showing it now (they count themselves).
    property int buttons: 0

    // The key as a button or the help shows it: "⇧ E", "Esc".
    readonly property string keyName: keysText !== "" ? keysText : keys.length > 0 ? KeyNames.nameOf(keys[0]) : ""
    readonly property bool live: bound && when && (keyLayer === "any" || keymap.layer === keyLayer
        || (keyLayer === "chrome" && (keymap.layer === "grid" || keymap.layer === "viewer")))

    signal triggered()
    function trigger() {
        if (when)
            triggered()
    }

    visible: false
    Shortcut {
        enabled: action.live && action.keys.length > 0
        sequences: action.keys
        context: Qt.ApplicationShortcut
        autoRepeat: action.autoRepeat
        onActivated: action.triggered()
    }
    Component.onCompleted: keymap.add(action)
    Component.onDestruction: keymap.remove(action)
}
