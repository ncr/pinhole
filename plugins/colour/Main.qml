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
    readonly property var finder: view.item
    covering: finder !== null && finder.shown
    function made() {
        view.active = true
        return view.item
    }

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
    readonly property bool finding: finder !== null && (finder.shown || finder.returning)
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
            onTriggered: plugin.made().openFinder()
        },
        PluginAction {
            id: viewerAction
            readonly property string colour: pinhole.viewer.open ? pinhole.library.item(pinhole.viewer.index).avgColor ?? "" : ""
            place: "viewer"
            text: qsTr("Same colour")
            key: "H"
            shown: colour !== ""
            onTriggered: plugin.made().like(pinhole.viewer.index)
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
        UiText {
            width: parent ? parent.width : 0
            wrapMode: Text.WordWrap
            color: theme.foreground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            Component.onCompleted: if (pinhole.library.count > 0) plugin.ensure()
            text: pinhole.library.count === 0 ? qsTr("No photos here yet.")
                : plugin.colours === null ? qsTr("Reading the photos' colours…")
                : qsTr("%1 of %2 photos have a colour of their own to find by; the rest average to grey. H in the grid, or on a photo.")
                  .arg(Number(plugin.colours.colourful).toLocaleString(Qt.locale(), "f", 0))
                  .arg(Number(plugin.colours.count).toLocaleString(Qt.locale(), "f", 0))
        }
    }

    Loader {
        id: view
        anchors.fill: parent
        active: false
        // Its `plugin` is this one, the Loader's parent: within the Finder,
        // `plugin` would be its own property.
        sourceComponent: Finder { plugin: view.parent }
    }
    FinderKeys { finder: plugin.finder }
}
