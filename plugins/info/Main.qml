import QtQuick
import Pinhole.Plugin
import "Details.js" as Details

// Info: a photo's details in a small panel over it, on I in the viewer,
// staying as one goes from photo to photo. What the index knows shows at
// once (the date, the original's size, dimensions and type, whether it has
// a place); the rest is the original's own, read when it is here: the
// camera, lens and exposure from its EXIF, a video's codec and length, and
// where it was taken, with the map. pinhole-info reads it (ImageMagick for
// photos, ffprobe for videos) and keeps what it read in the plugin's
// cache, so a photo seen once is instant after.
PinholePlugin {
    id: plugin
    objectName: "infoPlugin"

    // Open, kept from one time to the next; and open for the tour, which
    // closes with the viewer it opened.
    property bool shown: pinhole.setting("shown", false) === true
    property bool touring: false
    readonly property bool showing: (shown || touring) && pinhole.viewer.open
    function toggle() {
        shown = !shown
        pinhole.setSetting("shown", shown)
    }

    // The photo the panel is about, what the library says of it, and what
    // is known so far of its original.
    property string photoId
    property var item: ({})
    property var facts: ({})
    // Its original to come (asked for, or waited for a moment first), on
    // its way (the arc turns), and what went wrong, said, if it could not
    // be had.
    property bool coming: false
    property bool reading: false
    property string trouble
    // A video too big to fetch only for this, until asked (⇧I).
    property bool waiting: false
    property double waitingBytes: 0
    // What was read this time the window is open, by id: back to a photo
    // is instant without even the cache.
    property var known: ({})
    // A video's original comes by itself only this small: most are tens or
    // hundreds of MB, which would hold up the viewer's own photos from the
    // service and push them out of its cache.
    readonly property double smallVideo: 50 * 1000 * 1000
    readonly property string index: pinhole.library.dir + "/library.sqlite"

    readonly property int at: showing ? pinhole.viewer.index : -1
    onAtChanged: look()
    Connections {
        target: pinhole.library
        // Positions may have moved: the photo shown is looked up again.
        function onChanged() {
            plugin.photoId = ""
            plugin.look()
        }
    }
    Connections {
        target: pinhole.viewer
        function onOpenChanged() {
            if (!pinhole.viewer.open)
                plugin.touring = false
        }
    }

    // What is under way for the photo shown, all left behind when another
    // is: a download already asked for still comes to the service, but
    // nothing is read of it for a photo gone by.
    property var running: []
    function track(promise) {
        running.push(promise)
        return promise
    }
    function leave() {
        fetchSoon.stop()
        running.forEach(promise => promise.cancel())
        running = []
        pending = null
        coming = false
        reading = false
        waiting = false
    }
    // Something went wrong, said in the panel unless it was only left.
    function failed(id, error, what) {
        if (error.code === "cancelled")
            return
        // The service's (offline, not found) are said here only.
        if (error.code === "exit" || error.code === "start" || error.code === undefined)
            pinhole.warn(error)
        if (id === photoId) {
            coming = false
            reading = false
            trouble = what.arg(error.code === "exit" ? String(error.stderr || error.message).trim().split("\n")[0] : error.message || error.code)
        }
    }

    function look() {
        if (at < 0) {
            photoId = ""
            leave()
            return
        }
        const it = pinhole.library.item(at)
        if (!it.id || it.id === photoId)
            return
        leave()
        photoId = it.id
        item = it
        trouble = ""
        if (known[it.id]) {
            facts = known[it.id]
            return
        }
        facts = {}
        coming = true
        const id = it.id, video = it.video
        track(pinhole.run("pinhole-info", ["index", index, id, pinhole.cacheDir])).then(result => {
            if (id !== photoId)
                return
            const out = result.stdout
            const split = out.indexOf("\n--\n")
            const row = JSON.parse(out.slice(0, split))[0] || {}
            const cached = out.slice(split + 4).trim()
            const first = Details.fromIndex(row)
            if (cached !== "") {
                try {
                    take(id, Details.merged(first, JSON.parse(cached)))
                    return
                } catch (e) {
                    // Read again below.
                }
            }
            facts = first
            pending = { id: id, video: video, key: row.key || "", first: first }
            if (video && !(first.bytes <= smallVideo)) {
                coming = false
                waiting = true
                waitingBytes = first.bytes || 0
                return
            }
            // A second on it first: not for each photo stepped past.
            fetchSoon.restart()
        }).catch(error => failed(id, error, qsTr("The index could not be read: %1")))
    }
    property var pending: null
    Timer {
        id: fetchSoon
        interval: 1000
        onTriggered: plugin.read(plugin.pending)
    }
    // The original, from the service, and what pinhole-info reads of it;
    // the facts kept in the cache, as they are for the photo even when it
    // is no longer shown.
    function read(job) {
        if (!job || job.id !== photoId)
            return
        waiting = false
        coming = true
        reading = true
        track(pinhole.fetch(job.id, "original")).then(path => {
            track(pinhole.run("pinhole-info", ["read", job.video ? "video" : "image", path])).then(result => {
                const read = Details.fromOriginal(result.stdout)
                pinhole.run("pinhole-info", ["keep", pinhole.cacheDir, job.id, job.key], { input: JSON.stringify(read) + "\n" })
                    .catch(pinhole.warn)
                take(job.id, Details.merged(job.first, read))
            }).catch(error => failed(job.id, error, qsTr("The original could not be read: %1")))
        }, error => failed(job.id, error, qsTr("The original could not be fetched: %1"))).catch(pinhole.warn)
    }
    function take(id, all) {
        known[id] = all
        if (id === photoId) {
            facts = all
            coming = false
            reading = false
        }
    }

    // What the panel says, a row each.
    function takenText() {
        let date = null, offset = ""
        const m = facts.taken ? facts.taken.local.match(/^(\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+)/) : null
        if (m) {
            date = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6]))
            offset = facts.taken.offset
        } else if (item.date) {
            date = new Date(item.date)
        }
        if (!date)
            return ""
        // The viewer has the long date at the top left: here the short
        // month and no weekday, to the second, and the clock's time zone.
        const locale = Qt.locale()
        const day = locale.dateFormat(Locale.LongFormat).replace(/d{4}\W*/, "").replace("MMMM", "MMM").trim()
        const time = locale.timeFormat(Locale.ShortFormat).replace("mm", "mm:ss")
        return date.toLocaleDateString(locale, day) + ", " + date.toLocaleTimeString(locale, time) + (offset ? "\u00a0·\u00a0" + utc(offset) : "")
    }
    // "+02:00" as UTC+2, "+05:30" as UTC+5:30.
    function utc(offset) {
        const m = offset.match(/^([+-])(\d\d):?(\d\d)$/)
        if (!m)
            return ""
        const hours = Number(m[2]), minutes = Number(m[3])
        return hours === 0 && minutes === 0 ? "UTC" : "UTC" + m[1] + hours + (minutes > 0 ? ":" + m[3] : "")
    }
    function focalText() {
        if (!facts.focal)
            return ""
        const focal = Details.decimal(facts.focal, 1) + " mm"
        return facts.focal35 ? qsTr("%1 · %2 mm in 35 mm").arg(focal).arg(Math.round(facts.focal35)) : focal
    }
    function exposureText() {
        const parts = []
        if (facts.aperture)
            parts.push("ƒ/" + Details.decimal(facts.aperture, 1))
        if (facts.shutter)
            parts.push(Details.shutterText(facts.shutter))
        if (facts.iso)
            parts.push("ISO " + Math.round(facts.iso))
        return parts.join(" · ")
    }
    function sizeText() {
        const parts = []
        if (facts.width && facts.height) {
            parts.push(facts.width + " × " + facts.height)
            if (!item.video)
                parts.push(Details.decimal(facts.width * facts.height / 1e6, 1) + " MP")
        }
        if (facts.bytes)
            parts.push(pinhole.sizeText(facts.bytes))
        return parts.join(" · ")
    }
    function formatText() {
        const parts = []
        if (facts.format)
            parts.push(Details.isRaw(facts.format) ? qsTr("%1 (RAW)").arg(facts.format) : facts.format)
        if (item.live)
            parts.push(qsTr("Live Photo"))
        if (item.video) {
            if (facts.codec)
                parts.push(facts.hdr ? facts.codec + ", " + facts.hdr : facts.codec)
            if (facts.fps)
                parts.push(Details.fpsText(facts.fps))
            if (facts.audio)
                parts.push(facts.audio)
            if (facts.duration)
                parts.push(Details.clock(facts.duration))
        }
        return parts.join(" · ")
    }
    readonly property bool hasPlace: facts.latitude !== undefined
    function placeText() {
        if (hasPlace)
            return Details.coordinates(facts) + (facts.altitude !== undefined ? " · " + Math.round(facts.altitude) + " m" : "")
        if (facts.hasLocation)
            return coming ? qsTr("Yes; where, once the original is here") : qsTr("Yes")
        return coming ? "" : qsTr("None")
    }
    readonly property var rows: (facts, item, coming, [
        [qsTr("Taken"), takenText()],
        [qsTr("Camera"), facts.camera || ""],
        [qsTr("Lens"), facts.lens || ""],
        [qsTr("Focal length"), focalText()],
        [qsTr("Exposure"), exposureText()],
        [qsTr("Size"), sizeText()],
        [qsTr("Format"), formatText()],
        [qsTr("Location"), placeText()]
    ].filter(row => row[1] !== ""))
    function openMap() {
        if (hasPlace)
            Qt.openUrlExternally(Details.mapUrl(facts))
    }

    actions: [
        PluginAction {
            id: infoAction
            place: "viewer"
            text: qsTr("Info")
            key: "I"
            active: plugin.shown
            onTriggered: plugin.toggle()
        }
    ]
    help: [
        ["I", qsTr("A photo's details")],
        ["W", qsTr("Where, on the map")],
        ["⇧ I", qsTr("A big video's details")]
    ]

    // The photo under the pointer in the grid, shown full window with the
    // panel open over it; then the photo with a place taken nearest it,
    // looked for meanwhile, the map's button lit.
    property string placeId
    function findPlace(index) {
        placeId = ""
        const date = pinhole.library.item(index).date
        if (date === undefined)
            return
        pinhole.run("pinhole-info", ["located", plugin.index, String(Math.round(date))])
            .then(result => placeId = result.stdout.trim()).catch(pinhole.warn)
    }
    QtObject {
        id: mapLight
        property Item button: mapButton
    }
    tour: [
        {
            title: qsTr("Details"),
            text: qsTr("On a photo, I shows what its original says: when it was taken, the camera, lens and exposure, its size and format. It stays open from photo to photo."),
            open: () => {
                plugin.touring = true
                const index = pinhole.currentIndex()
                plugin.findPlace(index)
                return index
            },
            action: infoAction
        },
        {
            title: qsTr("Where"),
            text: qsTr("A photo that knows where it was taken shows the place, and W opens it on OpenStreetMap."),
            open: () => {
                plugin.touring = true
                const index = plugin.placeId !== "" ? pinhole.library.indexOf(plugin.placeId) : -1
                return index >= 0 ? index : pinhole.currentIndex()
            },
            action: mapLight
        }
    ]

    // At the bottom left, over the photo's corner, not its middle: above the
    // strip and a video's bar, clear of the date at the top left and the
    // arrows halfway down.
    Rectangle {
        id: panel
        objectName: "infoPanel"
        readonly property int padding: Math.round(theme.fontSize * 0.833)
        // Smaller than the chips' type, as the side panel's notes are.
        readonly property int textSize: Math.round(theme.fontSize * 0.857)
        readonly property real valueWidth: Math.min(textSize * 20, Math.max(textSize * 10, plugin.width * 0.25))
        x: 16
        y: plugin.height - 60 - (plugin.item.video ? 56 : 0) - 16 - height
        width: content.implicitWidth + 2 * padding
        height: content.implicitHeight + 2 * padding
        radius: Math.min(4, theme.radius)
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
        border.width: 1
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
        visible: opacity > 0
        opacity: plugin.showing ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        // The photos under it take no clicks meant for it.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

        Column {
            id: content
            x: panel.padding
            y: panel.padding
            spacing: Math.round(theme.fontSize * 0.5)
            // Its name, and the arc while the original is on its way.
            Row {
                spacing: Math.round(theme.fontSize * 0.5)
                UiText {
                    text: plugin.item.filename || ""
                    color: theme.brightForeground
                    width: Math.min(implicitWidth, panel.valueWidth)
                    elide: Text.ElideMiddle
                    anchors.verticalCenter: parent.verticalCenter
                }
                Spinner {
                    objectName: "infoSpinner"
                    bare: true
                    busy: plugin.reading
                    delay: 200
                    height: Math.round(theme.fontSize * 1.4)
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Grid {
                columns: 2
                columnSpacing: panel.textSize
                rowSpacing: Math.round(panel.textSize * 0.2)
                Repeater {
                    model: plugin.rows.reduce((all, row) => all.concat([{ text: row[0], label: true }, { text: row[1], label: false }]), [])
                    delegate: UiText {
                        required property var modelData
                        text: modelData.text
                        color: modelData.label ? theme.darkForeground : theme.foreground
                        font.pixelSize: panel.textSize
                        width: modelData.label ? implicitWidth : Math.min(implicitWidth, panel.valueWidth)
                        wrapMode: Text.WordWrap
                    }
                }
            }
            UiText {
                visible: plugin.trouble !== ""
                width: Math.min(implicitWidth, panel.valueWidth + panel.textSize * 6)
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                font.pixelSize: panel.textSize
                text: plugin.trouble
            }
            Row {
                spacing: 8
                visible: plugin.hasPlace || plugin.waiting
                ShellButton {
                    id: mapButton
                    objectName: "infoMap"
                    visible: plugin.hasPlace
                    text: qsTr("Open map")
                    key: "W"
                    keyLayer: "viewer"
                    onTapped: plugin.openMap()
                }
                // A long video's original is a big download for a few
                // lines: only when asked.
                ShellButton {
                    visible: plugin.waiting
                    text: plugin.waitingBytes > 0 ? qsTr("Read the original · %1").arg(pinhole.sizeText(plugin.waitingBytes))
                                                  : qsTr("Read the original")
                    key: "Shift+I"
                    keyLayer: "viewer"
                    onTapped: plugin.readWaiting()
                }
            }
        }
    }
    function readWaiting() {
        if (waiting && pending && pending.id === photoId)
            read(pending)
    }
}
