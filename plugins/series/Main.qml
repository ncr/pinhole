import QtQuick
import Pinhole.Plugin

// Series: photos of one subject taken close together, the way a moment
// worth a photo rarely gets just one, shown in the grid as one card, and
// made into a short GIF or video to send (Studio.qml). Finding them is
// pinhole-series's (finder/), which keeps what it learns of each photo in
// the plugin's cache, so only new photos take it any time.
//
// What it finds can be put right by hand: a photo taken out of a series or
// added to one, a series ungrouped, photos grouped; on a photo, or with the
// whole series on a screen of its own (Editor.qml). Those edits are kept
// by the photos' ids, over what the finder finds, and each is a step of
// Pinhole's history, to undo with Ctrl Z.
PinholePlugin {
    id: plugin
    // Its two views: a series to put right, and its clip, over that.
    property alias editor: editorView
    property alias studio: studioView
    // Series `k` to put right, on its photo `from`.
    function openEditor(k, from) {
        if (k >= 0 && k < series.length)
            editorView.open(k, from)
    }
    function openStudio(k, fromGrid) {
        if (k >= 0 && k < series.length)
            studioView.open(k, fromGrid)
    }

    // Every series, in the library's order: the positions of its shots.
    property var series: []
    // A photo's series by its position, for those in one.
    property var seriesAt: ({})
    // What the finder found, each series its photos' ids.
    property var foundIds: []
    // The edits: series made or changed by hand (ids), and photos kept out
    // of any series. The user's own work of the library shown, so synced
    // with it (pinhole.synced: its "manual" and "apart") to every computer
    // it is shown on, each library's apart: a folder's photos are named by
    // their paths, and one folder's edits must not group another's photos
    // of the same names.
    property var edits: ({ manual: [], apart: [] })
    // The library they are of (its key).
    property string editsKey: ""
    // Bumped when the series or the edits change, for bindings to follow.
    property int revision: 0
    property bool found: false
    // Each series one card in the grid.
    stacks: series

    // Installed beside this file; in a checkout, where cargo builds it.
    readonly property string finder: pinhole.exists(pinhole.dir + "/pinhole-series")
                                     ? "pinhole-series" : pinhole.dir + "/finder/target/release/pinhole-series"

    // Asked again while it looks: once more after, with the library then.
    function find() {
        if (pinhole.library.count === 0 && !finding.busy)
            return
        finding.start(() => pinhole.library.count === 0 ? null
                           : pinhole.run(finder, ["find", pinhole.library.dir, pinhole.cacheDir]))
    }
    Job {
        id: finding
        coalesce: true
        onDone: result => { if (result) plugin.take(result.stdout) }
        onFailed: error => pinhole.warn(error)
    }
    // The finder's series, each its ids.
    function take(out) {
        foundIds = JSON.parse(out)
        found = true
        apply()
    }
    // The series as shown: those made by hand, then the finder's without
    // the photos in those or kept apart, each as library positions.
    function apply() {
        const positions = ids => ids.map(id => pinhole.library.indexOf(id)).filter(i => i >= 0).sort((a, b) => a - b)
        const taken = new Set(edits.apart)
        const list = []
        for (const ids of edits.manual) {
            const shots = positions(ids)
            if (shots.length >= 2) {
                list.push(shots)
                ids.forEach(id => taken.add(id))
            }
        }
        for (const ids of foundIds) {
            const whole = positions(ids)
            const shots = positions(ids.filter(id => !taken.has(id)))
            // The finder's own at three or more; one put right by hand may
            // keep two.
            if (whole.length >= 3 && shots.length >= 2)
                list.push(shots)
        }
        list.sort((a, b) => a[0] - b[0])
        const at = {}
        list.forEach((shots, k) => shots.forEach(i => at[i] = k))
        series = list
        seriesAt = at
        ++revision
    }

    // Edits, by position; each kept at once.
    function idOf(index) { return pinhole.library.item(index).id }
    function idsOf(k) { return series[k].map(idOf) }
    // A photo's ids with its whole series', if it is in one; and of
    // several photos (any list, the grid's selection too).
    function withSeries(index) { return of(index) >= 0 ? idsOf(of(index)) : [idOf(index)] }
    function allWithSeries(indexes) {
        const ids = new Set()
        for (let i = 0; i < indexes.length; ++i)
            withSeries(indexes[i]).forEach(id => ids.add(id))
        return [...ids]
    }
    // An edit, `name` in the history: undone, the edits go back to what
    // they were before it (the history is the library's: another shown
    // starts it afresh).
    function keep(manual, apart, name) {
        const before = edits, after = { manual: manual, apart: apart }
        setEdits(after)
        pinhole.did(name, () => setEdits(before), () => setEdits(after))
    }
    function setEdits(state) {
        pinhole.synced.manual = state.manual
        pinhole.synced.apart = state.apart
        edits = state
        apply()
    }
    function syncedEdits() {
        return { manual: pinhole.synced.manual || [], apart: pinhole.synced.apart || [] }
    }
    // The edits of the library shown, when another is: true then.
    function loadEdits() {
        const key = pinhole.library.key
        if (key === editsKey)
            return false
        // Another library: what the finder found was the last one's.
        if (editsKey !== "")
            foundIds = []
        editsKey = key
        edits = syncedEdits()
        if (found)
            apply()
        return true
    }
    // Changed on another computer (or in another window): as they are now.
    Connections {
        target: pinhole.synced
        function onChanged() {
            const now = plugin.syncedEdits()
            if (JSON.stringify(now) === JSON.stringify(plugin.edits))
                return
            plugin.edits = now
            if (plugin.found)
                plugin.apply()
        }
    }
    // The series made by hand, without any that holds one of `ids`.
    function manualWithout(ids) {
        const set = new Set(ids)
        return edits.manual.filter(series => !series.some(id => set.has(id)))
    }
    // One series of all these photos, and the series they are in.
    function group(indexes, name = qsTr("grouping")) {
        const ids = allWithSeries(indexes)
        if (ids.length < 2)
            return
        const set = new Set(ids)
        keep(manualWithout(ids).concat([ids]), edits.apart.filter(id => !set.has(id)), name)
    }
    function takeOut(index) {
        const k = of(index)
        if (k < 0)
            return
        const id = idOf(index)
        const all = idsOf(k)
        const rest = all.filter(other => other !== id)
        keep(manualWithout(all).concat(rest.length >= 2 ? [rest] : []), edits.apart.concat([id]), qsTr("taking out of a series"))
    }
    function ungroup(indexes) {
        const ids = allWithSeries(Array.from(indexes).filter(i => of(i) >= 0))
        keep(manualWithout(ids), [...new Set(edits.apart.concat(ids))], qsTr("ungrouping"))
    }
    // The series right beside a photo not in one, the nearer in time when
    // there is one on each side; -1 if none.
    function beside(index) {
        if (of(index) >= 0)
            return -1
        const before = index > 0 ? of(index - 1) : -1
        const after = index + 1 < pinhole.library.count ? of(index + 1) : -1
        if (before < 0 || after < 0)
            return before >= 0 ? before : after
        const date = i => pinhole.library.item(i).date
        return date(index) - date(index - 1) <= date(index + 1) - date(index) ? before : after
    }
    function of(index) {
        const k = seriesAt[index]
        return k === undefined ? -1 : k
    }
    // The series a photo is in, else the nearest one in the library.
    function near(index) {
        if (series.length === 0)
            return -1
        if (of(index) >= 0)
            return of(index)
        let lo = 0, hi = series.length - 1
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (series[mid][0] <= index)
                lo = mid
            else
                hi = mid - 1
        }
        const before = series[lo], after = series[lo + 1]
        if (after && Math.abs(after[0] - index) < Math.abs(index - before[before.length - 1]))
            return lo + 1
        return lo
    }

    // The library changed: the same series where their photos are now, at
    // once (it keeps their ids), then a look again once it has settled.
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (!plugin.loadEdits() && plugin.found)
                plugin.apply()
            later.restart()
        }
    }
    Component.onCompleted: {
        loadEdits()
        find()
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Series")
            key: "G"
            shown: plugin.series.length > 0
            onTriggered: plugin.openStudio(plugin.near(pinhole.currentIndex()), true)
        },
        // A photo shown full window: its series, and putting it right.
        PluginAction {
            id: seriesAction
            place: "viewer"
            group: "series"
            text: plugin.shownSeries >= 0 ? qsTr("Series of %1").arg(plugin.series[plugin.shownSeries].length) : ""
            key: "G"
            shown: plugin.shownSeries >= 0
            onTriggered: plugin.openEditor(plugin.shownSeries, pinhole.viewer.index)
        },
        PluginAction {
            place: "viewer"
            group: "series"
            text: qsTr("Take out")
            key: "X"
            shown: plugin.shownSeries >= 0
            onTriggered: plugin.takeOut(pinhole.viewer.index)
        },
        PluginAction {
            place: "viewer"
            group: "series"
            text: qsTr("Ungroup")
            key: "U"
            shown: plugin.shownSeries >= 0
            onTriggered: plugin.ungroup([pinhole.viewer.index])
        },
        PluginAction {
            id: addAction
            readonly property int k: (plugin.revision, pinhole.viewer.open ? plugin.beside(pinhole.viewer.index) : -1)
            // The series it would make, shown while the pointer is on it.
            readonly property var joined: k >= 0 ? plugin.series[k].concat([pinhole.viewer.index]).sort((a, b) => a - b) : []
            place: "viewer"
            group: "series"
            text: qsTr("Add to series")
            key: "A"
            shown: k >= 0
            onTriggered: plugin.group([pinhole.viewer.index, plugin.series[k][0]], qsTr("adding to a series"))
        },
        // What is ticked in the grid.
        PluginAction {
            place: "selection"
            group: "series"
            text: qsTr("Group")
            key: "G"
            usable: pinhole.selectionCount >= 2
            onTriggered: plugin.group(pinhole.selection())
        },
        PluginAction {
            place: "selection"
            group: "series"
            text: qsTr("Ungroup")
            key: "U"
            shown: plugin.picked.some(i => plugin.of(i) >= 0)
            onTriggered: plugin.ungroup(pinhole.selection())
        }
    ]
    // The series of the photo shown full window, or -1.
    readonly property int shownSeries: (revision, pinhole.viewer.open ? of(pinhole.viewer.index) : -1)
    // The photos ticked in the grid.
    readonly property var picked: (revision, pinhole.selectionCount, Array.from(pinhole.selection()))

    // What it does, on a series of one's own: shown the first time it is
    // on, once there is a series to show, and from the side panel.
    tourReady: found && series.length > 0
    // The series the tour is on.
    property int touring: -1
    tour: [
        {
            title: qsTr("Taken close together"),
            text: qsTr("Photos of one subject taken close together are a series: one card in the grid, marked in its corner."),
            photo: () => {
                touring = near(pinhole.currentIndex())
                return touring >= 0 ? series[touring][0] : -1
            }
        },
        {
            title: qsTr("GIF or video"),
            text: qsTr("G makes a series a short clip to send: pick a motion, a pace and a look, then ⏎ for a GIF or ⇧\u00a0⏎ for a video."),
            action: gridAction
        },
        {
            title: qsTr("Put it right"),
            text: qsTr("On a photo of a series, X takes it out, U ungroups and G shows the series whole; on one beside a series, A adds it. In select mode, G groups what is ticked. Ctrl\u00a0Z undoes any of it."),
            open: () => touring >= 0 && touring < series.length ? series[touring][0] : -1,
            action: seriesAction
        }
    ]

    help: [
        ["X", qsTr("Out of its series")],
        ["A", qsTr("Into the series beside it")],
        ["U", qsTr("Ungroup")],
        ["G", qsTr("Group what is ticked")],
        ["G", qsTr("Its series, to put right")]
    ]

    settings: Component {
        SettingNote {
            width: parent ? parent.width : 0
            text: !plugin.found ? qsTr("Looking for series…")
                : plugin.series.length === 0 ? qsTr("No series here yet.")
                : qsTr("%n series here: G in the grid, or on a photo of one.", "", plugin.series.length)
        }
    }

    Joining {
        id: joining
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        shown: addAction.hovered && addAction.shown && pinhole.viewer.open
        shots: addAction.joined
        joiner: pinhole.viewer.index
        key: "G"
    }
    // While it shows, G opens that series, the photo among those it could
    // grow into, as the band says.
    Command {
        name: "series.joining"
        keys: ["G"]
        keyLayer: "viewer"
        usable: joining.shown && !plugin.covering
        mouse: qsTr("Click the band")
        touchpad: qsTr("Tap the band")
        onTriggered: plugin.openEditor(addAction.k, pinhole.viewer.index)
    }

    PluginView {
        id: editorView
        source: "Editor.qml"
        closeCommand.mouse: qsTr("The ✕ button")
        closeCommand.touchpad: qsTr("The ✕ button")
    }
    // G opened it, and G closes it.
    PluginView {
        id: studioView
        source: "Studio.qml"
        toggleKey: "G"
        closeCommand.mouse: qsTr("The ✕ button")
        closeCommand.touchpad: qsTr("The ✕ button")
    }
}
