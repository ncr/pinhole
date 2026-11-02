import QtQuick
import Pinhole.Plugin

// Live Photos: the mark on them in the grid, and over a Live Photo shown
// full window its motion on L and its effects as on the iPhone on ⇧ L:
// Live, Loop, Bounce and Long exposure, the one set on the iPhone saying
// so. What a source reports of a photo (that it is one, its effect, its
// motion) is the library's; the effects a source has not got the service
// makes. LiveLayer.qml plays them over the photo.
PinholePlugin {
    id: plugin

    // The photo shown full window, if it is a Live Photo: its library item.
    readonly property var item: {
        const viewer = pinhole.viewer
        if (!viewer.open || viewer.index < 0)
            return null
        const it = pinhole.library.item(viewer.index)
        return it.live === true ? it : null
    }
    readonly property var effects: ["live", "loop", "bounce", "long_exposure"]
    // As set on the iPhone, and as shown: another chosen here holds for as
    // long as Pinhole runs, never in the library. By photo id.
    property var chosen: ({})
    function savedOf(it) { return it && it.effect ? it.effect : "live" }
    function effectOf(it) { return it ? chosen[it.id] ?? savedOf(it) : "live" }
    readonly property string saved: savedOf(item)
    readonly property string effect: effectOf(item)
    function choose(effect) {
        if (!item || effects.indexOf(effect) < 0 || effect === plugin.effect)
            return
        const next = Object.assign({}, chosen)
        if (effect === saved)
            delete next[item.id]
        else
            next[item.id] = effect
        chosen = next
    }
    function nameOf(effect) {
        switch (effect) {
        case "loop": return qsTr("Loop")
        case "bounce": return qsTr("Bounce")
        case "long_exposure": return qsTr("Long exposure")
        default: return qsTr("Live")
        }
    }

    // The layer over the Live Photo shown now, while it is (LiveLayer.qml).
    property var liveLayer: null
    // The motion is SDR, drawn over the photo: on a display that shows the
    // photo brighter or more colourful (docs/hdr.md), it is kept at SDR
    // meanwhile, so the motion starts and ends on the same picture.
    holdSdr: liveLayer !== null && liveLayer.playing
    // One player for the motion, as the viewer had: made the first time a
    // motion plays (Qt Multimedia is read then, not as Pinhole starts),
    // and lent to the layer of the photo shown, inside it.
    Loader {
        id: motion
        active: false
        visible: false
        source: "LiveMotion.qml"
    }
    Connections {
        target: motion.item
        function onFailed() { plugin.complain(qsTr("Could not play the motion")) }
    }
    function lendMotion(place) {
        motion.active = true
        const m = motion.item
        m.parent = place
        m.anchors.fill = place
        return m
    }
    function takeBack(m, place) {
        if (m.parent !== place)
            return
        m.anchors.fill = undefined
        m.parent = motion
    }
    // What could not be had, under the Live button.
    function complain(text) { play.complain(text) }

    marks: [
        PluginMark {
            name: "live"
            text: qsTr("Live Photos")
            icon: "live"
            colour: "yellow"
            order: 30
            field: "live"
            // Off until turned on: nearly every iPhone photo is one.
            defaultShown: false
        }
    ]

    // The button plays the motion as the effect shows it: once for Live and
    // a long exposure, from the start for a loop or a bounce. Lit from the
    // press until the motion has played; while it is on its way (asked for,
    // or a stream before its first frames) its key turns. Its menu, on ⇧ L,
    // has the four effects.
    readonly property PluginAction playAction: play
    actions: [
        PluginAction {
            id: play
            place: "viewer"
            group: "live"
            order: -10
            text: plugin.nameOf(plugin.effect)
            key: "L"
            shown: plugin.item !== null
            active: plugin.liveLayer !== null && (plugin.liveLayer.asking || plugin.liveLayer.playing)
            busy: plugin.liveLayer !== null && (plugin.liveLayer.asking || plugin.liveLayer.buffering)
            busyDelay: 300
            onTriggered: if (plugin.liveLayer) plugin.liveLayer.play()
        },
        PluginAction {
            place: "viewer"
            group: "live"
            order: -10
            text: plugin.nameOf("live")
            shown: plugin.item !== null
            active: plugin.effect === "live"
            onTriggered: plugin.choose("live")
        },
        PluginAction {
            place: "viewer"
            group: "live"
            order: -10
            text: plugin.nameOf("loop")
            hint: plugin.saved === "loop" ? "iPhone" : ""
            shown: plugin.item !== null
            active: plugin.effect === "loop"
            onTriggered: plugin.choose("loop")
        },
        PluginAction {
            place: "viewer"
            group: "live"
            order: -10
            text: plugin.nameOf("bounce")
            hint: plugin.saved === "bounce" ? "iPhone" : ""
            shown: plugin.item !== null
            active: plugin.effect === "bounce"
            onTriggered: plugin.choose("bounce")
        },
        PluginAction {
            place: "viewer"
            group: "live"
            order: -10
            text: plugin.nameOf("long_exposure")
            hint: plugin.saved === "long_exposure" ? "iPhone" : ""
            shown: plugin.item !== null
            active: plugin.effect === "long_exposure"
            onTriggered: plugin.choose("long_exposure")
        }
    ]
    help: [["L", qsTr("Play the Live Photo")], ["Shift+L", qsTr("Live effects")]]

    viewerLayer: LiveLayer {
        live: plugin
        pinhole: plugin.pinhole
    }
}
