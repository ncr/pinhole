import QtQuick
import Pinhole.Plugin
import "Mosaic.js" as Mosaic

// Mosaic: a photo made of the library's other photos, each tile a photo of
// the colour the picture has there (MosaicView.qml).
//
// A photo's colour is the average of its thumbnail, which the index keeps
// for the grid to draw until the thumbnail is in (library.item(i).avgColor).
// A thousand photos spread over the library's colours are the tiles
// (Mosaic.tiles), and colours are compared as the eye sees them (OKLab).
//
// The view, and the tiles, are made the first time it opens, and kept: most
// launches never show it.
PinholePlugin {
    id: plugin

    // The photos a mosaic is made of (Mosaic.tiles), by their ids, and the
    // closest of them to each colour as they are found (Mosaic.matcher):
    // chosen when the view opens, and again when it next opens after the
    // library was read again. What it shows stays as it is while it is
    // open. `seed` says which of each colour's photos: R, others.
    readonly property var none: ({ n: 0, ids: [], at: new Int32Array(0), rgb: new Uint8Array(0), lab: new Float32Array(0), revision: -1 })
    property var tiles: none
    property var matcher: null
    property bool stale: true
    property int seed: 0
    // Bumped when the library is read again: what is shown finds its
    // photos' new places by their ids.
    property int revision: 0
    function choose() {
        const library = pinhole.library
        const hidden = pinhole.hiddenCount > 0 ? i => pinhole.isHidden(i) : null
        const chosen = Mosaic.tiles(library.field("avgColor"), library.field("date"), library.field("id"), hidden, 1024, seed)
        // Where its photos are holds until the library is read again.
        chosen.revision = revision
        stale = false
        tiles = chosen
        matcher = Mosaic.matcher(chosen)
    }
    function ensure() {
        if (stale)
            choose()
    }
    function shuffle() {
        seed = seed + 1 + Math.floor(Math.random() * 1000)
        choose()
    }
    Connections {
        target: pinhole.library
        function onChanged() {
            plugin.stale = true
            ++plugin.revision
        }
    }
    // What the grid hides (Screenshots' hide mode, say) is no tile, the
    // next time the tiles are chosen.
    Connections {
        target: pinhole
        function onHiddenChanged() { plugin.stale = true }
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Mosaic")
            helpText: qsTr("A mosaic of the photo under the pointer")
            key: "H"
            shown: pinhole.library.count > 0
            onTriggered: mosaicView.open(pinhole.currentIndex())
        },
        PluginAction {
            id: viewerAction
            place: "viewer"
            text: qsTr("Mosaic")
            helpText: qsTr("A mosaic of this photo")
            key: "H"
            shown: pinhole.viewer.open && (pinhole.viewer.photo.avgColor ?? "") !== ""
            onTriggered: {
                // On a photo opened from a mosaic, which waits behind it
                // to come back: closed, and opened anew over this photo.
                if (mosaicView.shown && pinhole.away)
                    mosaicView.close()
                mosaicView.open(pinhole.viewer.index)
            }
        }
    ]

    // The tour's mosaic is of a favourite, if there is one: a photo its
    // person likes; else of the newest.
    function example() {
        const favourites = pinhole.library.field("favorite")
        for (let i = favourites.length - 1; i >= 0; --i)
            if (favourites[i] && !pinhole.isHidden(i))
                return i
        return pinhole.library.count - 1
    }
    tourReady: pinhole.library.count > 0
    tour: [
        {
            title: qsTr("A photo made of your photos"),
            text: qsTr("On a photo, H makes a mosaic of it: each tile is another of your photos, of the colour the picture has there."),
            open: () => example(),
            action: viewerAction
        },
        {
            title: qsTr("Bigger and smaller tiles"),
            text: qsTr("In the mosaic, the slider, + and −, or the wheel make the tiles bigger and smaller. Space flips to the original and back, and S saves the mosaic to Pictures."),
            action: gridAction
        }
    ]

    help: [
        ["H", qsTr("A mosaic of the photo")],
        ["Space", qsTr("Mosaic: the original, and back")],
        ["+ −", qsTr("Mosaic: bigger, smaller tiles")],
        ["← →", qsTr("Mosaic: the photo before, after")]
    ]

    settings: Component {
        SettingNote {
            width: parent ? parent.width : 0
            text: pinhole.library.count === 0 ? qsTr("No photos here yet.")
                : qsTr("H on a photo, or in the grid over one: its mosaic, made of a thousand of your photos.")
        }
    }

    // Made the first time it opens, and kept.
    PluginView {
        id: mosaicView
        source: "MosaicView.qml"
        toggleKey: "H"
    }

    // Its keys, here rather than in MosaicView.qml, so that they are in the
    // help before it is first made.
    Command {
        name: "mosaic.flip"
        keys: ["Space"]
        keyLayer: "plugin"
        usable: mosaicView.shown
        mouse: qsTr("The Original button")
        touchpad: qsTr("The Original button")
        onTriggered: mosaicView.item.toggle()
    }
    // = as well as +, as for the grid's zoom.
    Command {
        name: "mosaic.bigger"
        keys: ["="]
        keyLayer: "plugin"
        usable: mosaicView.shown && mosaicView.item.canGrow
        autoRepeat: true
        mouse: qsTr("Wheel")
        touchpad: qsTr("Swipe up or down")
        onTriggered: mosaicView.item.resize(1)
    }
    Command {
        name: "mosaic.before"
        keys: ["Left"]
        keyLayer: "plugin"
        usable: mosaicView.shown
        autoRepeat: true
        noMouse: qsTr("Close it and pick another photo")
        noTouchpad: qsTr("Close it and pick another photo")
        onTriggered: mosaicView.item.step(-1)
    }
    Command {
        name: "mosaic.after"
        keys: ["Right"]
        keyLayer: "plugin"
        usable: mosaicView.shown
        autoRepeat: true
        noMouse: qsTr("Close it and pick another photo")
        noTouchpad: qsTr("Close it and pick another photo")
        onTriggered: mosaicView.item.step(1)
    }
    Command {
        name: "mosaic.open"
        text: qsTr("Open the tile's photo")
        section: "Mosaic"
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        usable: mosaicView.shown
        mouse: qsTr("Click a tile")
        touchpad: qsTr("Tap a tile")
        onTriggered: mosaicView.item.openPointed()
    }
}
