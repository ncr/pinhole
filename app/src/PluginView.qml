import QtQuick

// A view of the plugin's own over the photos (API 13): what a calendar, a
// gallery or a studio is drawn in. Put it in the plugin's root and open it
// from an action:
//
//     PluginView { id: calendar; source: "Calendar.qml"; toggleKey: "Y" }
//     PluginAction { place: "grid"; text: qsTr("Calendar"); key: "Y"; onTriggered: calendar.open() }
//
// While it is open the plugin covers the photos (its root's `covering`,
// unless the plugin binds that itself), and it fades in and out. It is
// made the first time it opens and kept after, so a launch that never
// shows it pays nothing; `lazy: false` makes it at once. It closes on Esc
// and on its `toggleKey`, when a photo is handed over from it (pinhole's
// dismissed()), and is hidden, still open, while a photo opened from it
// with comeBack shows (pinhole.away). One opened while that photo shows is
// over it, covering it, as People's W names who is in it.
//
// What it shows is `source`, a QML file of the plugin's, or `content`, a
// Component; `item` is that, once made. In a `source` file, `view` is this
// view from its first binding on (`view.shown`, `view.close()`,
// `view.plugin`, the plugin's root), and `pinhole` the plugin's, as
// anywhere in its files; declare neither. A PanelHeader in it closes it
// with ✕.
//
// `open(…)` hands what it is given to a `start(…)` the item has, before
// it shows (an Apple ID, the photos to send); `close()` calls its `stop()`.
// `keep: false` lets go of the item as it closes, to be made afresh.
//
// Esc goes to what it shows first: a function `back()` there that
// returns true has taken it (a close-up left, a making stopped), and the
// view stays open. Two of a plugin's views open at once (a studio over an
// editor) stack: only the last opened has its keys, and Esc closes it
// alone. `make()` makes what it shows without opening it.
Item {
    id: view
    // A QML file of the plugin's, relative to its main file ("Calendar.qml").
    property url source
    // Or what to show, written in place.
    property Component content
    // Made the first time it opens (true), or at once.
    property bool lazy: true
    // Kept once made (true), or let go as it closes.
    property bool keep: true
    // Open now.
    readonly property bool shown: _open
    // The key that opens it (its action's), which closes it too, beside
    // Esc.
    property string toggleKey
    // Esc's words in the help; `closeCommand.mouse` and `.touchpad` say
    // how else it closes ("Click"), as a Command's.
    property string closeText: qsTr("Close")
    // What `source` or `content` made; null before it first opens.
    readonly property Item item: loader.item
    // Its Esc: what a PanelHeader's ✕ presses.
    readonly property alias closeCommand: closer
    // The plugin's root and what Pinhole gives it.
    readonly property Item plugin: {
        for (let at = view.parent; at; at = at.parent)
            if (at.openViews !== undefined && at.pinhole !== undefined)
                return at
        return null
    }
    readonly property var pinhole: plugin ? plugin.pinhole : null
    // The last opened of the plugin's open views: the one with the keys.
    readonly property bool onTop: _open && (!plugin || plugin.viewStack[plugin.viewStack.length - 1] === view)
    signal opened()
    signal closed()

    property bool _open: false
    property bool _made: !lazy
    // Opened over a photo the plugin opened to come back from: shown.
    readonly property bool openedAway: _open && _awayOpened
    property bool _awayOpened: false

    function open(...given) {
        _made = true
        const shown = loader.item
        if (shown && typeof shown.start === "function")
            shown.start(...given)
        if (_open)
            return
        _awayOpened = !!(pinhole && pinhole.away)
        _open = true
        opened()
    }
    function close() {
        if (!_open)
            return
        _open = false
        const shown = loader.item
        if (shown && typeof shown.stop === "function")
            shown.stop()
        closed()
        if (!keep)
            _made = !lazy
    }
    function toggle() {
        if (_open)
            close()
        else
            open()
    }
    // What it shows, made now if it was not, without opening it.
    function make() {
        _made = true
        return loader.item
    }
    // Esc: back in what it shows first (its back()), else closed.
    function back() {
        const shown = loader.item
        if (shown && typeof shown.back === "function" && shown.back() === true)
            return
        close()
    }

    anchors.fill: parent
    opacity: _open ? 1 : 0
    visible: opacity > 0 && (!(pinhole && pinhole.away) || openedAway)
    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    // On the plugin root's stack of open views, whose `covering` follows.
    on_OpenChanged: {
        if (!plugin)
            return
        const others = plugin.viewStack.filter(v => v !== view)
        plugin.viewStack = _open ? others.concat([view]) : others
    }
    Component.onDestruction: if (plugin && _open) plugin.viewStack = plugin.viewStack.filter(v => v !== view)

    Loader {
        id: loader
        anchors.fill: parent
        active: view._made
        sourceComponent: view.content
        // Relative to the plugin's own file (its root's, with the query
        // that tells one load of it from the next), not to this one.
        source: view.content || String(view.source) === "" ? ""
              : view.plugin ? Qt.resolvedUrl(view.source, view.plugin) : view.source
        // A file of an older plugin that declares `view` has it too.
        onLoaded: if ("view" in item && item.view !== view) item.view = view
    }
    Command {
        id: closer
        name: "view.close"
        text: view.closeText
        section: "plugin"
        keys: ["Escape"]
        keyLayer: "plugin"
        when: view.onTop
        onTriggered: view.back()
    }
    // Its own key closes it at once (the action lists that key).
    Command {
        name: "view.toggle"
        keys: view.toggleKey !== "" ? [view.toggleKey] : []
        keyHelp: false
        noMouse: "its action's button"
        noTouchpad: "its action's button"
        keyLayer: "plugin"
        when: view.onTop && view.toggleKey !== ""
        onTriggered: view.close()
    }
    Connections {
        target: view.pinhole
        ignoreUnknownSignals: true
        function onDismissed() { view.close() }
    }
}
