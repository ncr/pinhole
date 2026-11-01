import QtQuick
import QtQuick.Window

// A Live Photo's motion and effects over it, inside the photo shown full
// window (PinholePlugin.viewerLayer), so they zoom, pan, grow from the cell
// and shrink back with it. Made for each photo shown, and gone with it.
//
// A loop or a bounce plays round and round as the photo shows; L plays the
// motion as the effect has it: once for Live and a long exposure, from the
// start for a loop or a bounce. A long exposure shows its frames as one. A
// photo set on the iPhone as a long exposure is one in the library too (its
// full size, which Pinhole shows): shown as anything else, its original
// goes over it.
Item {
    id: photoLayer
    objectName: "liveLayer"
    // Pinhole's: the photo, and whether it is still shown.
    property int photo: -1
    property bool shown: false
    // The plugin (Main.qml), and its way into Pinhole.
    property var live
    property var pinhole
    anchors.fill: parent

    readonly property var item: pinhole.library.item(photo)
    readonly property bool isLive: item.live === true
    readonly property string saved: live.savedOf(item)
    readonly property string effect: isLive ? live.effectOf(item) : "live"
    // A loop and a bounce go round until another effect or photo.
    readonly property bool looping: loops(effect)
    // What the effect plays: the loop or the bounce, else the motion itself.
    readonly property string motionKind: motionOf(effect)
    // As functions too, for what runs as the effect changes: before the
    // two above have followed it.
    function loops(effect) { return effect === "loop" || effect === "bounce" }
    function motionOf(effect) { return loops(effect) ? effect : "live" }

    // The motion asked for with L and not here yet: asked once, however
    // often L is pressed meanwhile; and which.
    property bool asking: false
    property string askedKind: ""
    // The motion here, a URL to play (the service's while it still comes),
    // and which it is.
    property string source: ""
    property string sourceKind: ""
    // The long exposure, and the original under it, once here.
    property string longExposure: ""
    property string original: ""
    // The effect that could not be had.
    property string failed: ""
    // Playing, and streaming in, not yet moving.
    readonly property bool playing: motion !== null && motion.shows
    readonly property bool buffering: playing && source.startsWith("http") && motion.stalled
    // The plugin's player while this layer has it (LiveMotion.qml): lent
    // to the photo shown, in this layer, so it zooms and grows with it.
    property var motion: null
    // An effect being fetched or made (a bounce or a long exposure made
    // from the motion takes a few seconds): Pinhole's spinner turns.
    readonly property bool loading: shown && isLive && failed !== effect
        && (looping ? sourceKind !== effect : effect === "long_exposure" && longExposure === "")

    // Made: the effect followed from here on (as it is made, `effect`
    // settles as `live` is given).
    property bool ready: false
    // Each fetch on its way, by what it is for.
    property var motionFetch: null
    property var longFetch: null
    property var originalFetch: null

    function play() {
        if (!isLive || !shown || asking)
            return
        // Already here: from the start again.
        if (source !== "" && sourceKind === motionOf(effect)) {
            start()
            return
        }
        // Another motion playing (a loop, L pressed on Live chosen since)
        // is not the one asked for.
        stop()
        source = ""
        sourceKind = ""
        asking = true
        askedKind = motionOf(effect)
        fetchMotion()
    }
    function fetchMotion() {
        const kind = motionOf(effect)
        if (motionFetch)
            motionFetch.cancel()
        motionFetch = pinhole.fetch(photo, kind, { stream: true })
        motionFetch.then(url => {
            // Another effect chosen meanwhile: not this motion.
            if (kind !== photoLayer.motionOf(photoLayer.effect))
                return
            photoLayer.source = url
            photoLayer.sourceKind = kind
            photoLayer.asking = false
            photoLayer.start()
        }, error => {
            if (error.code === "cancelled" || kind !== photoLayer.motionOf(photoLayer.effect))
                return
            // The motion of the effect before (a loop, Bounce chosen) is not
            // this one: none plays, and the button is not lit for it.
            if (photoLayer.sourceKind !== kind)
                photoLayer.stop()
            // Asked for with L: the button says why not.
            if (photoLayer.asking) {
                photoLayer.asking = false
                photoLayer.live.complain(photoLayer.whyNot(error.code))
                return
            }
            photoLayer.failed = kind
            photoLayer.live.complain(qsTr("Could not show this effect"))
        }).catch(pinhole.warn)
    }
    function start() {
        if (!motion) {
            motion = live.lendMotion(motionPlace)
            motion.looping = Qt.binding(() => photoLayer.looping)
        }
        const player = motion.player
        player.source = source
        motion.output.visible = true
        player.position = 0
        player.play()
    }
    function stop() {
        if (!motion)
            return
        motion.output.visible = false
        motion.player.stop()
    }
    // The player back to the plugin, stopped.
    function giveBack() {
        stop()
        if (motion && live)
            live.takeBack(motion, motionPlace)
        motion = null
    }
    // What the effect needs fetched: a loop or a bounce to play, a long
    // exposure to show, and the original under another effect of a photo
    // set as a long exposure.
    function showEffect() {
        if (!isLive || !shown)
            return
        if (loops(effect) && sourceKind !== effect)
            fetchMotion()
        // Chosen again, a loop or a bounce here already plays again.
        else if (loops(effect) && !playing)
            start()
        else if (effect === "long_exposure" && longExposure === "" && !longFetch)
            fetchLong()
        if (saved === "long_exposure" && effect !== "long_exposure" && original === "" && !originalFetch)
            fetchOriginal()
    }
    function fetchLong() {
        longFetch = pinhole.fetch(photo, "long_exposure")
        longFetch.then(path => photoLayer.longExposure = photoLayer.imageOf(path), error => {
            photoLayer.longFetch = null
            if (error.code === "cancelled")
                return
            photoLayer.failed = "long_exposure"
            if (photoLayer.effect === "long_exposure")
                photoLayer.live.complain(qsTr("Could not show this effect"))
        }).catch(pinhole.warn)
    }
    function fetchOriginal() {
        originalFetch = pinhole.fetch(photo, "original")
        originalFetch.then(path => photoLayer.original = photoLayer.imageOf(path), error => {
            photoLayer.originalFetch = null
            if (error.code !== "cancelled")
                pinhole.warn(error)
        }).catch(pinhole.warn)
    }
    function imageOf(path) { return "image://photo/" + encodeURIComponent(path) }

    // Why the motion did not come, in so many words.
    function whyNot(code) {
        const key = pinhole.library.key
        const kind = key.split(":")[0]
        const name = kind === "icloud" ? "iCloud" : key.substring(key.indexOf(":") + 1).split("/").pop()
        // The window's line to the service, for whether it is offline (the
        // service says so before it answers).
        const health = typeof service !== "undefined" && service ? service.health : ""
        if (code === "needs_sign_in")
            return qsTr("Not signed in to %1").arg(name)
        if (code === "not_found")
            return qsTr("This Live Photo has no motion")
        if (code === "no_service" || health === "no_service")
            return qsTr("The Pinhole service is not running")
        if (health === "offline")
            return kind === "icloud" ? qsTr("Offline, and the motion is not on this computer")
                                     : qsTr("%1 is not there. Is its disk connected?").arg(name)
        return qsTr("Could not load the motion from %1").arg(name)
    }

    onEffectChanged: {
        if (!ready)
            return
        failed = ""
        // The motion asked for with L is not the one this effect plays.
        if (asking && askedKind !== motionOf(effect)) {
            asking = false
            if (motionFetch)
                motionFetch.cancel()
            motionFetch = null
        }
        // Not a loop or a bounce, or another one than plays: what plays
        // stops (a loop does not go on under Bounce while it comes).
        if (!loops(effect) || sourceKind !== effect)
            stop()
        showEffect()
    }
    onShownChanged: {
        if (shown)
            return
        giveBack()
        asking = false
        for (const f of [motionFetch, longFetch, originalFetch])
            if (f)
                f.cancel()
        if (live.liveLayer === photoLayer)
            live.liveLayer = null
    }
    Component.onCompleted: {
        ready = true
        if (!isLive || !shown)
            return
        live.liveLayer = photoLayer
        showEffect()
    }
    Component.onDestruction: {
        giveBack()
        if (live && live.liveLayer === photoLayer)
            live.liveLayer = null
    }

    // Shown as another effect, a photo set as a long exposure: its
    // original, over the long exposure Pinhole shows; up close, at its full
    // size, as Pinhole's own is.
    Image {
        id: originalImage
        anchors.fill: parent
        visible: photoLayer.saved === "long_exposure" && photoLayer.effect !== "long_exposure"
        source: photoLayer.original
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize: Qt.size(photoLayer.Window.width * Screen.devicePixelRatio, photoLayer.Window.height * Screen.devicePixelRatio)
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
    Image {
        anchors.fill: parent
        visible: originalImage.visible
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        mipmap: true
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        readonly property bool wanted: visible && originalImage.status === Image.Ready
            && (photoLayer.width > photoLayer.Window.width * 1.3 || photoLayer.height > photoLayer.Window.height * 1.3)
        onWantedChanged: if (wanted && source.toString() === "") source = originalImage.source
    }
    // A long exposure: its frames as one.
    Image {
        anchors.fill: parent
        source: photoLayer.effect === "long_exposure" ? photoLayer.longExposure : ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize: Qt.size(photoLayer.Window.width * Screen.devicePixelRatio, photoLayer.Window.height * Screen.devicePixelRatio)
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
    // Where the motion plays, over the rest.
    Item {
        id: motionPlace
        anchors.fill: parent
    }
}
