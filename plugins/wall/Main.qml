import QtQuick
import Pinhole.Plugin
import "Colour.js" as Colour

// Rainbow wall: as many of the library's most colourful photos as fill the
// window, laid out by hue, a rainbow of one's own photos (WallView.qml,
// Wall.qml).
//
// A photo's colour is the average of its thumbnail, which the index keeps
// for the grid to draw until the thumbnail is in (library.item(i).avgColor).
// One colour for a whole photo is crude: a red coat on a grey street
// averages to grey. The wall is of photos that are mostly one colour
// (sunsets, the sea, a forest, a lit-up evening), and colours are placed
// as the eye sees them (Colour.js: OKLab).
//
// The wall itself is made the first time it opens, and kept: most launches
// never show it.
PinholePlugin {
    id: plugin

    // The library's colours (Colour.index), by the photos' ids: read when
    // the wall opens, from the library's columns at once, and while it is
    // closed some seconds after the library is read again, the changes
    // meanwhile with it (a library being built changes every few seconds),
    // so that W opens at once. What the wall shows stays as it is while it
    // is open, or a photo opened from it; the colours are read again once
    // it is closed.
    property var colours: null
    property bool stale: false
    // Bumped when the library is read again: what is shown finds its
    // photos' new places by their ids.
    property int revision: 0
    function ensure() {
        warming.stop()
        if (colours && !stale)
            return colours
        const library = pinhole.library
        const data = Colour.index(library.field("avgColor"), library.field("date"), library.field("id"))
        stale = false
        colours = Colour.withHidden(data, hiddenTest())
        return colours
    }
    readonly property bool showing: wallView.shown
    function warm() {
        if (stale && !showing && !warming.running)
            warming.start()
    }
    // Twelve seconds: On this day reads the library again at ten, and the
    // two in one frame would be a hitch twice as long.
    Timer {
        id: warming
        interval: 12000
        onTriggered: if (!plugin.showing) plugin.ensure()
    }
    onShowingChanged: warm()
    Connections {
        target: pinhole.library
        function onChanged() {
            plugin.stale = true
            ++plugin.revision
            plugin.warm()
        }
    }

    // What the grid hides (Screenshots' hide mode, say) is left off the
    // wall, as the grid leaves it out. Bumped when that changes, for what
    // is shown to look again.
    property int hiddenRevision: 0
    function hiddenTest() {
        return pinhole.hiddenCount > 0 ? i => pinhole.isHidden(i) : null
    }
    // After the library's own change, when it was read again too: the
    // colours' positions are then old, and they are read again anyway.
    Connections {
        target: pinhole
        function onHiddenChanged() { Qt.callLater(plugin.hiddenMoved) }
    }
    function hiddenMoved() {
        ++hiddenRevision
        if (colours && !stale)
            colours = Colour.withHidden(colours, hiddenTest())
        if (wallView.item)
            wallView.item.hiddenMoved()
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Rainbow wall")
            key: "W"
            shown: pinhole.library.count > 0
            onTriggered: wallView.open()
        }
    ]

    tourReady: pinhole.library.count > 0
    tour: [
        {
            title: qsTr("A rainbow wall"),
            text: qsTr("W lays your most colourful photos out by hue, light to dark: a rainbow of your own. Space makes another, and a click on a photo opens it."),
            action: gridAction
        }
    ]

    help: [
        ["W", qsTr("The rainbow wall")],
        ["Space", qsTr("Another rainbow wall")],
        ["+ −", qsTr("Wall: bigger, smaller photos")]
    ]

    settings: Component {
        SettingNote {
            width: parent ? parent.width : 0
            Component.onCompleted: if (pinhole.library.count > 0) plugin.ensure()
            text: pinhole.library.count === 0 ? qsTr("No photos here yet.")
                : plugin.colours === null ? qsTr("Reading the photos' colours…")
                : qsTr("%1 of %2 photos are colourful enough for the wall; the rest average to grey. W in the grid.")
                  .arg(Number(plugin.colours.colourful).toLocaleString(Qt.locale(), "f", 0))
                  .arg(Number(plugin.colours.count).toLocaleString(Qt.locale(), "f", 0))
        }
    }

    // Made the first time it opens, and kept.
    PluginView {
        id: wallView
        source: "WallView.qml"
        toggleKey: "W"
    }

    // Its key, here rather than in WallView.qml, so that it is in the help
    // before the wall is first made. = as well as +, as for the grid's
    // zoom.
    Command {
        name: "wall.bigger"
        keys: ["="]
        keyLayer: "plugin"
        usable: wallView.shown && wallView.item.canGrow
        mouse: qsTr("The + button")
        touchpad: qsTr("Pinch")
        onTriggered: wallView.item.grow()
    }
}
