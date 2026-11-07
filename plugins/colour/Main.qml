import QtQuick
import Pinhole.Plugin
import "Colour.js" as Colour

// Colours: photos found by their colour. Pick one on a wheel and the
// closest photos show beside it; on a photo, the ones closest to its
// colour; or the whole library laid out by hue, a rainbow wall of one's
// own photos (Finder.qml).
//
// A photo's colour is the average of its thumbnail, which the index keeps
// for the grid to draw until the thumbnail is in (library.item(i).avgColor).
// One colour for a whole photo is crude: a red coat on a grey street
// averages to grey. It finds photos that are mostly one colour (sunsets,
// the sea, a forest, a lit-up evening), and colours are compared as the
// eye sees them (Colour.js: OKLab).
//
// The finder itself, a wheel, a grid and a wall, is made the first time it
// opens, and kept: most launches never show it.
PinholePlugin {
    id: plugin
    // The finder once made (Finder.qml), null before.
    readonly property var finder: finderView.item

    // The library's colours (Colour.index), by the photos' ids: read when
    // the finder opens, from the library's columns at once, and while it is
    // closed some seconds after the library is read again, the changes
    // meanwhile with it (a library being built changes every few seconds),
    // so that H opens at once. What the finder shows stays as it is while
    // it is open, or a photo opened from it; the colours are read again
    // once it is closed.
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
    readonly property bool finding: finderView.shown
    function warm() {
        if (stale && !finding && !warming.running)
            warming.start()
    }
    // Twelve seconds: On this day reads the library again at ten, and the
    // two in one frame would be a hitch twice as long.
    Timer {
        id: warming
        interval: 12000
        onTriggered: if (!plugin.finding) plugin.ensure()
    }
    onFindingChanged: warm()
    Connections {
        target: pinhole.library
        function onChanged() {
            plugin.stale = true
            ++plugin.revision
            plugin.warm()
        }
    }

    // What the grid hides (Screenshots' hide mode, say) is left out of what
    // is found, the dots and the wall, as the grid leaves it out. Bumped
    // when that changes, for what is shown to look again.
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
        if (finder)
            finder.hiddenMoved()
    }

    // A photo's colour as [L, a, b], or null.
    function colourOf(index) {
        const c = index >= 0 ? pinhole.library.item(index).avgColor : ""
        return c ? Colour.lab(parseInt(c.slice(1), 16)) : null
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Colours")
            key: "H"
            shown: pinhole.library.count > 0
            onTriggered: {
                finderView.open()
                finderView.item.openFinder()
            }
        },
        PluginAction {
            id: viewerAction
            readonly property string colour: pinhole.viewer.open ? pinhole.viewer.photo.avgColor ?? "" : ""
            place: "viewer"
            text: qsTr("Same colour")
            key: "H"
            shown: colour !== ""
            onTriggered: {
                if (!plugin.colourOf(pinhole.viewer.index))
                    return
                finderView.open()
                finderView.item.like(pinhole.viewer.index)
            }
        }
    ]

    // The tour's "same colour" opens on a photo that shows it well
    // (Colour.example).
    function example() {
        const id = Colour.example(ensure())
        return id !== "" ? pinhole.library.indexOf(id) : -1
    }
    tourReady: pinhole.library.count > 0
    tour: [
        {
            title: qsTr("Find by colour"),
            text: qsTr("H opens the colour finder: pick a colour on the wheel and its lightness under it, and the photos closest to it show beside it. Each dot on the wheel is one of your photos."),
            action: gridAction
        },
        {
            title: qsTr("Same colour"),
            text: qsTr("On a photo, H shows the photos closest to its colour."),
            open: () => example(),
            action: viewerAction
        },
        {
            title: qsTr("A rainbow wall"),
            text: qsTr("In the finder, W lays your most colourful photos out by hue, light to dark: a rainbow of your own. Space makes another."),
            action: gridAction
        },
        {
            title: qsTr("How well it finds"),
            text: qsTr("It goes by each photo's average colour, so it finds photos that are mostly one colour: a sunset, the sea, a forest. A red coat on a grey street averages to grey."),
            settings: true
        }
    ]

    help: [
        ["H", qsTr("Find by colour")],
        ["H", qsTr("On a photo: those of its colour")],
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
                : qsTr("%1 of %2 photos have a colour of their own to find by; the rest average to grey. H in the grid, or on a photo.")
                  .arg(Number(plugin.colours.colourful).toLocaleString(Qt.locale(), "f", 0))
                  .arg(Number(plugin.colours.count).toLocaleString(Qt.locale(), "f", 0))
        }
    }

    // Made the first time it opens, and kept.
    PluginView {
        id: finderView
        // The Finder is given its view as it is made: its bindings read
        // the plugin through it from the first.
        content: Component { Finder { view: finderView } }
        toggleKey: "H"
    }

    // Its keys, here rather than in Finder.qml, so that they are in the
    // help before it is first made.
    Command {
        name: "colour.hue"
        text: qsTr("Hue round the wheel")
        section: "Colours"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderView.item.turn(10)
    }
    Command {
        name: "colour.hueBack"
        keys: ["Right"]
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderView.item.turn(-10)
    }
    Command {
        name: "colour.stronger"
        text: qsTr("Stronger, greyer")
        section: "Colours"
        keys: ["Up"]
        keysText: "↑ ↓"
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderView.item.strengthen(0.05)
    }
    Command {
        name: "colour.greyer"
        keys: ["Down"]
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the wheel")
        touchpad: qsTr("Tap the wheel")
        onTriggered: finderView.item.strengthen(-0.05)
    }
    Command {
        name: "colour.darker"
        text: qsTr("Darker, lighter")
        section: "Colours"
        keys: ["["]
        keysText: "[ ]"
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finderView.item.lighten(-0.04)
    }
    Command {
        name: "colour.lighter"
        keys: ["]"]
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find"
        autoRepeat: true
        mouse: qsTr("Click the strip")
        touchpad: qsTr("Tap the strip")
        onTriggered: finderView.item.lighten(0.04)
    }
    Command {
        name: "colour.first"
        text: qsTr("Open the closest")
        section: "Colours"
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "find" && finderView.item.results.length > 0
        mouse: qsTr("Click a photo")
        touchpad: qsTr("Tap a photo")
        onTriggered: finderView.item.open(finderView.item.results[0])
    }
    // = as well as +, as for the grid's zoom.
    Command {
        name: "colour.bigger"
        keys: ["="]
        keyLayer: "plugin"
        when: finderView.shown && finderView.item.mode === "wall" && finderView.item.canGrow
        mouse: qsTr("The + button")
        touchpad: qsTr("Pinch")
        onTriggered: finderView.item.grow()
    }
}
