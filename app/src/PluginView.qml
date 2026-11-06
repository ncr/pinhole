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
// with comeBack shows (pinhole.away).
//
// What it shows is `source`, a QML file of the plugin's, or `content`, a
// Component; `item` is that, once made. In it, `pinhole` is the plugin's,
// as anywhere in the plugin's files, and a `view` property, if it declares
// one (`property var view`), is this view: `view.shown`, `view.close()`.
// A PanelHeader in it closes it with ✕.
Item {
    id: view
    // A QML file of the plugin's, relative to its main file ("Calendar.qml").
    property url source
    // Or what to show, written in place.
    property Component content
    // Made the first time it opens (true), or at once.
    property bool lazy: true
    // Open now.
    readonly property bool shown: _open
    // The key that opens it (its action's), which closes it too, beside
    // Esc.
    property string toggleKey
    // Esc's words in the help.
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
    signal opened()
    signal closed()

    property bool _open: false
    property bool _made: !lazy

    function open() {
        if (_open)
            return
        _made = true
        _open = true
        opened()
    }
    function close() {
        if (!_open)
            return
        _open = false
        closed()
    }
    function toggle() {
        if (_open)
            close()
        else
            open()
    }

    anchors.fill: parent
    opacity: _open ? 1 : 0
    visible: opacity > 0 && !(pinhole && pinhole.away)
    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    // Counted in the plugin's root, whose `covering` follows.
    on_OpenChanged: if (plugin) plugin.openViews += _open ? 1 : -1
    Component.onDestruction: if (plugin && _open) plugin.openViews -= 1

    Loader {
        id: loader
        anchors.fill: parent
        active: view._made
        sourceComponent: view.content
        // Relative to the plugin's own file (its root's, with the query
        // that tells one load of it from the next), not to this one.
        source: view.content || String(view.source) === "" ? ""
              : view.plugin ? Qt.resolvedUrl(view.source, view.plugin) : view.source
        onLoaded: if ("view" in item) item.view = view
    }
    Command {
        id: closer
        name: "view.close"
        text: view.closeText
        section: "plugin"
        keys: view.toggleKey !== "" ? ["Escape", view.toggleKey] : ["Escape"]
        keyLayer: "plugin"
        when: view._open
        onTriggered: view.close()
    }
    Connections {
        target: view.pinhole
        ignoreUnknownSignals: true
        function onDismissed() { view.close() }
    }
}
