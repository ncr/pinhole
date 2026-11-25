import QtQuick
import Pinhole.Plugin

// Photos of screens: what was photographed off a TV, a monitor, a laptop,
// a phone or a machine's display (a game, an error, a page with a code).
// They are photos, not screenshots (Screenshots has those, by what the
// library says of them), and nothing in the library says which they are:
// a worker pinholed runs (pinhole-screens) looks at every thumbnail on
// this computer and keeps what it found in the plugin's cache for the
// library, found.json, which is read here.
//
// ⇧ D shows them together (ScreensView.qml), and, as the side panel
// says, kept out of the grid (`hidden`), so every view that leaves out what
// the grid hides leaves them out too.
//
// The person's word is over the worker's: a photo said to be one, or not
// to be, is so at once, on every computer the library is shown on
// (pinhole.synced, "is\t<id>": true or false), and the worker learns from
// what was said, here and there alike.
PinholePlugin {
    id: plugin

    readonly property bool hiding: pinhole.settings.hide === true
    // What the worker found (ids), what the library says are screenshots
    // (never photos of a screen), and what the person said: { id: bool }.
    property var foundIds: []
    property var screenshotIds: []
    property var said: ({})
    // The photos of screens: their ids, and their library positions in
    // order.
    property var ids: []
    property var screens: []
    // Read from the worker's file for this library since it was shown.
    property bool read: false

    hidden: hiding ? screens : []

    // The worker, as pinholed runs it.
    readonly property var worker: pinhole.workers.find(worker => worker.name === "screens") || null
    readonly property var status: worker && worker.running && worker.said && worker.said.event === "status"
                                  ? worker.said : null
    readonly property bool looking: status !== null && (status.phase === "scanning" || status.phase === "models")

    function isScreen(id) { return ids.indexOf(id) >= 0 }

    // What was said, as it is synced now.
    function saidNow() {
        const values = pinhole.synced.values(), now = {}
        for (const key of Object.keys(values))
            if (key.startsWith("is\t") && typeof values[key] === "boolean")
                now[key.slice(3)] = values[key]
        return now
    }
    // The ids: found, less the screenshots and what was said not to be
    // one, with what was said to be one.
    function apply() {
        const no = new Set(screenshotIds)
        const all = new Set()
        for (const id of Object.keys(said)) {
            if (said[id])
                all.add(id)
            else
                no.add(id)
        }
        for (const id of foundIds)
            if (!no.has(id))
                all.add(id)
        take([...all])
        if (keptKey !== "" && read)
            keep(keptKey, ids)
    }
    function take(list) {
        const library = pinhole.library
        const placed = list.map(id => [library.indexOf(id), id]).filter(pair => pair[0] >= 0).sort((a, b) => a[0] - b[0])
        ids = placed.map(pair => pair[1])
        screens = placed.map(pair => pair[0])
    }

    // Said of photos (ids), `is` or not: at once, synced, and a step to
    // undo, each photo's key back to what it was.
    function say(photos, is) {
        const before = {}, after = {}
        for (const id of photos) {
            before[id] = said[id] === undefined ? null : said[id]
            after[id] = is
        }
        setSaid(after)
        pinhole.did(is ? qsTr("marking as a photo of a screen") : qsTr("marking as not a photo of a screen"),
                    () => setSaid(before), () => setSaid(after))
    }
    function setSaid(changes) {
        const now = Object.assign({}, said)
        for (const id of Object.keys(changes)) {
            pinhole.synced["is\t" + id] = changes[id]
            if (changes[id] === null)
                delete now[id]
            else
                now[id] = changes[id]
        }
        said = now
        apply()
    }
    Connections {
        target: pinhole.synced
        function onChanged() {
            const now = plugin.saidNow()
            if (JSON.stringify(now) === JSON.stringify(plugin.said))
                return
            plugin.said = now
            plugin.apply()
        }
    }

    function setHiding(hide) {
        if (hide === hiding)
            return
        keepHiding(hide)
        pinhole.did(hide ? qsTr("hiding photos of screens") : qsTr("showing photos of screens"),
                    () => keepHiding(!hide), () => keepHiding(hide))
    }
    function keepHiding(hide) {
        pinhole.settings.hide = hide
        if (hide && screens.length > 0)
            pinhole.notify(qsTr("%n photos of screens hidden", "", screens.length))
    }

    // What they were last, kept for each source's library in the cache, as
    // Screenshots keeps its own: read as the plugin loads, before the
    // grid's first frame, so that hidden ones never show for a moment and
    // go. Not for a library that does not say whose it is, nor more than
    // `keepAtMost`.
    readonly property int keepAtMost: 5000
    property string shown: ""
    property string keptKey: ""
    function loadKept() {
        const key = pinhole.library.key
        const library = key !== "" ? key : "folder " + pinhole.library.dir
        if (library === shown)
            return false
        shown = library
        keptKey = key !== "" ? "found:" + key : ""
        read = false
        foundIds = []
        screenshotIds = []
        said = saidNow()
        // What was kept, with what was said over it.
        const kept = new Set(keptKey !== "" ? Array.from(pinhole.cache[keptKey] || []) : [])
        for (const id of Object.keys(said)) {
            if (said[id])
                kept.add(id)
            else
                kept.delete(id)
        }
        take([...kept])
        return true
    }
    function keep(key, list) {
        pinhole.cache[key] = list.length <= keepAtMost ? list : null
        const sources = pinhole.library.keys()
        for (const other of pinhole.cache.keys())
            if (other.startsWith("found:") && other !== key && sources.indexOf(other.slice(6)) < 0
                    && pinhole.cache[other] !== undefined)
                pinhole.cache[other] = null
    }

    // The worker's file and the library's screenshots, asked together, away
    // from the window; asked while it reads, once more after.
    function find() {
        const showing = shown, dir = pinhole.library.dir
        if (pinhole.library.count === 0) {
            finder.cancel()
            took(null, [], showing, dir)
            return
        }
        finder.start(() => [
            pinhole.readJson(pinhole.cacheDir + "/found.json").catch(error => {
                // Not looked yet: the worker has not started, or is off.
                if (error.code === "not_found")
                    return null
                throw error
            }),
            pinhole.screenshots()
        ], answers => took(answers[0], answers[1], showing, dir))
    }
    function took(file, screenshots, showing, dir) {
        // Another library shown meanwhile: the last one's, neither kept as
        // this one's nor shown; this one is looked at next.
        if (showing !== shown || dir !== pinhole.library.dir) {
            find()
            return
        }
        screenshotIds = Array.from(screenshots)
        // No file yet: what was kept from before stays until there is.
        // (What readJson gives is a list, not always a JavaScript Array.)
        if (file && file.ids && typeof file.ids.length === "number") {
            foundIds = Array.from(file.ids).filter(id => typeof id === "string")
            read = true
        }
        apply()
    }
    Job {
        id: finder
        coalesce: true
        onFailed: error => pinhole.warn(error)
    }
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (!plugin.loadKept())
                plugin.take(plugin.ids)
            later.restart()
        }
    }
    // The worker got further, or learned from what was said.
    Connections {
        target: pinhole
        function onWorkerSaid(name, said) {
            if (name === "screens" && said.event === "found")
                later.restart()
        }
    }
    Component.onCompleted: {
        loadKept()
        find()
    }

    // The photos ticked: are they all ones?
    readonly property bool tickedAreScreens: (pinhole.library.revision, pinhole.selectionCount > 0
                                              && allScreens(pinhole.selection()))
    function allScreens(picked) {
        const set = new Set(ids)
        for (const i of picked)
            if (!set.has(pinhole.library.item(i).id))
                return false
        return true
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Screens")
            helpText: qsTr("Photos of screens")
            key: "Shift+D"
            shown: pinhole.library.count > 0 && (plugin.screens.length > 0 || plugin.looking)
            busy: plugin.looking && plugin.screens.length === 0
            onTriggered: screensView.open()
        },
        // While they are away, how many, and the way back.
        PluginAction {
            id: hiddenAction
            place: "grid"
            text: qsTr("%n photos of screens hidden", "", plugin.screens.length)
            helpText: qsTr("Show the photos of screens again")
            shown: plugin.hiding && plugin.screens.length > 0
            active: true
            onTriggered: plugin.setHiding(false)
        },
        // Of what is ticked: said to be one, or not. Not on a photo shown,
        // where a button on every photo would be one too many.
        PluginAction {
            place: "selection"
            text: qsTr("Photo of a screen")
            helpText: qsTr("Say the photos ticked are of a screen, or are not")
            usable: pinhole.selectionCount > 0
            active: plugin.tickedAreScreens
            onTriggered: plugin.say(photos, !plugin.tickedAreScreens)
        }
    ]

    tourReady: screens.length > 0
    tour: [
        {
            title: qsTr("Photos of screens"),
            text: qsTr("A game on the TV, an error on a monitor, a machine's display: photos taken of a screen are found on this computer and shown together here."),
            action: gridAction
        },
        {
            title: qsTr("Out of the grid, if you like"),
            text: qsTr("Here, in the side panel (Tab), they can be hidden from the grid, and from the wall, the mosaic and the photo frame with it. On a photo, More says whether it is one: what you say there is learned from, on your other computers too."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: Math.round(theme.fontSize * 0.571)
            SettingSwitch {
                text: qsTr("Hide them from the grid")
                checked: plugin.hiding
                onToggled: plugin.setHiding(!checked)
            }
            SettingNote {
                readonly property var status: plugin.status
                text: plugin.worker && plugin.worker.paused
                        ? qsTr("Looking for photos of screens waits while background work is paused in Settings.")
                    : status && status.phase === "models" ? qsTr("Fetching the model that tells a screen (14 MB, once)…")
                    : status && status.phase === "scanning"
                        ? qsTr("Looking at the photos: %1 of %2. %n photos of screens so far.", "", plugin.screens.length)
                              .arg(Number(status.done).toLocaleString(Qt.locale(), "f", 0))
                              .arg(Number(status.total).toLocaleString(Qt.locale(), "f", 0))
                    : status && status.phase === "runtime" ? qsTr("Needs Python 3 with OpenCV and NumPy (python-opencv).")
                    : status && status.phase === "error" ? qsTr("Could not look at the photos; trying again in a minute.")
                    : plugin.screens.length === 0 ? (plugin.read ? qsTr("No photos of screens here.") : qsTr("Looking for photos of screens…"))
                    : plugin.hiding ? qsTr("%n photos of screens hidden", "", plugin.screens.length)
                    : qsTr("%n photos of screens. ⇧ D shows them together.", "", plugin.screens.length)
            }
        }
    }

    // Its view, made the first time it opens.
    readonly property alias gallery: screensView
    PluginView {
        id: screensView
        source: "ScreensView.qml"
        toggleKey: "Shift+D"
    }
}
