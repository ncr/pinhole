import QtQuick
import Pinhole.Plugin

// Series: photos of one subject taken close together, the way a moment
// worth a photo rarely gets just one, shown in the grid as one card, and
// made into a short GIF or video to send (Studio.qml). Finding them is
// pinhole-series's (finder/), which keeps what it learns of each photo in
// the plugin's cache, so only new photos take it any time.
//
// What it finds can be put right by hand: a photo taken out of a series or
// added to one, a series ungrouped, photos grouped. Those edits are kept
// by the photos' ids, over what the finder finds, until a photo is
// grouped automatically again.
PinholePlugin {
    id: plugin
    covering: studio.shown

    // Every series, in the library's order: the positions of its shots.
    property var series: []
    // A photo's series by its position, for those in one.
    property var seriesAt: ({})
    // What the finder found, each series its photos' ids.
    property var foundIds: []
    // The edits: series made or changed by hand (ids), and photos kept out
    // of any series.
    property var edits: pinhole.setting("edits", { manual: [], apart: [] })
    // Bumped when the series or the edits change, for bindings to follow.
    property int revision: 0
    property bool finding: false
    property bool findAgain: false
    property bool found: false
    // Each series one card in the grid, unless turned off.
    property bool asOne: pinhole.setting("asOne", true)
    stacks: asOne ? series : []

    // Installed beside this file; in a checkout, where cargo builds it.
    readonly property string finder: pinhole.exists(pinhole.dir + "/pinhole-series")
                                     ? "pinhole-series" : pinhole.dir + "/finder/target/release/pinhole-series"

    function find() {
        if (finding) {
            findAgain = true
            return
        }
        if (pinhole.library.count === 0)
            return
        finding = true
        pinhole.run(finder, ["find", pinhole.library.dir, pinhole.cacheDir], (code, out, error) => {
            finding = false
            if (code === 0)
                take(out)
            else
                console.warn("Series: " + error)
            if (findAgain) {
                findAgain = false
                find()
            }
        })
    }
    // The finder's lines, each a series' ids.
    function take(out) {
        foundIds = out.split("\n").filter(line => line !== "").map(line => line.split(" "))
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
    function keep(manual, apart) {
        edits = { manual: manual, apart: apart }
        pinhole.setSetting("edits", edits)
        apply()
    }
    // The series made by hand, without any that holds one of `ids`.
    function manualWithout(ids) {
        const set = new Set(ids)
        return edits.manual.filter(series => !series.some(id => set.has(id)))
    }
    function isEdited(index) {
        const id = idOf(index)
        return edits.apart.includes(id) || edits.manual.some(series => series.includes(id))
    }
    // One series of all these photos, and the series they are in.
    function group(indexes) {
        const ids = allWithSeries(indexes)
        if (ids.length < 2)
            return
        const set = new Set(ids)
        keep(manualWithout(ids).concat([ids]), edits.apart.filter(id => !set.has(id)))
    }
    function takeOut(index) {
        const k = of(index)
        if (k < 0)
            return
        const id = idOf(index)
        const all = idsOf(k)
        const rest = all.filter(other => other !== id)
        keep(manualWithout(all).concat(rest.length >= 2 ? [rest] : []), edits.apart.concat([id]))
    }
    function ungroup(indexes) {
        const ids = allWithSeries(Array.from(indexes).filter(i => of(i) >= 0))
        keep(manualWithout(ids), [...new Set(edits.apart.concat(ids))])
    }
    // Back to what the finder finds, for these photos, the series they are
    // in, and those the finder put them with.
    function automatic(indexes) {
        const ids = new Set(allWithSeries(indexes))
        for (const series of foundIds.concat(edits.manual))
            if (series.some(id => ids.has(id)))
                series.forEach(id => ids.add(id))
        keep(manualWithout([...ids]), edits.apart.filter(id => !ids.has(id)))
    }
    function automaticAll() { keep([], []) }
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

    // The library changed: look again, once it has settled.
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() { later.restart() }
    }
    Component.onCompleted: find()

    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("Series")
            key: "G"
            shown: plugin.series.length > 0
            onTriggered: studio.openSeries(plugin.near(pinhole.currentIndex()), true)
        },
        // A photo shown full window: its series, and putting it right.
        PluginAction {
            place: "viewer"
            text: plugin.shownSeries >= 0 ? qsTr("Series of %1").arg(plugin.series[plugin.shownSeries].length) : ""
            key: "G"
            shown: plugin.shownSeries >= 0
            onTriggered: studio.openSeries(plugin.shownSeries, false)
        },
        PluginAction {
            place: "viewer"
            text: qsTr("Take out")
            key: "X"
            shown: plugin.shownSeries >= 0
            onTriggered: plugin.takeOut(pinhole.viewer.index)
        },
        PluginAction {
            place: "viewer"
            text: qsTr("Ungroup")
            key: "U"
            shown: plugin.shownSeries >= 0
            onTriggered: plugin.ungroup([pinhole.viewer.index])
        },
        PluginAction {
            readonly property int k: (plugin.revision, pinhole.viewer.open ? plugin.beside(pinhole.viewer.index) : -1)
            place: "viewer"
            text: qsTr("Add to series")
            key: "A"
            shown: k >= 0
            onTriggered: plugin.group([pinhole.viewer.index, plugin.series[k][0]])
        },
        PluginAction {
            place: "viewer"
            text: qsTr("Automatic")
            key: "R"
            shown: (plugin.revision, pinhole.viewer.open && pinhole.viewer.index >= 0 && plugin.isEdited(pinhole.viewer.index))
            onTriggered: plugin.automatic([pinhole.viewer.index])
        },
        // What is ticked in the grid.
        PluginAction {
            place: "selection"
            text: qsTr("Group")
            key: "G"
            usable: pinhole.selectionCount >= 2
            onTriggered: plugin.group(pinhole.selection())
        },
        PluginAction {
            place: "selection"
            text: qsTr("Ungroup")
            key: "U"
            shown: plugin.picked.some(i => plugin.of(i) >= 0)
            onTriggered: plugin.ungroup(pinhole.selection())
        },
        PluginAction {
            place: "selection"
            text: qsTr("Automatic")
            key: "R"
            shown: plugin.picked.some(i => plugin.isEdited(i))
            onTriggered: plugin.automatic(pinhole.selection())
        }
    ]
    // The series of the photo shown full window, or -1.
    readonly property int shownSeries: (revision, pinhole.viewer.open ? of(pinhole.viewer.index) : -1)
    // The photos ticked in the grid.
    readonly property var picked: (revision, pinhole.selectionCount, Array.from(pinhole.selection()))

    help: [
        ["X", qsTr("Take a photo out of its series")],
        ["A", qsTr("Add a photo to the series beside it")],
        ["U", qsTr("Ungroup")],
        ["G", qsTr("Group what is ticked")],
        ["R", qsTr("Group automatically again")],
        ["G", qsTr("Series: a GIF or a video")],
        ["← →", qsTr("Previous, next series")],
        ["1–5", qsTr("Motion")],
        ["P L O", qsTr("Pace, look, shape")],
        ["T", qsTr("Steady")],
        ["Space", qsTr("Pause")],
        ["⏎", qsTr("Make a GIF")],
        ["⇧ ⏎", qsTr("Make a video")]
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: !plugin.found ? qsTr("Looking for series…")
                    : plugin.series.length === 0 ? qsTr("No series here yet.")
                    : qsTr("%n series here: G in the grid, or on a photo of one.", "", plugin.series.length)
            }
            // Put right by hand: all of it undone at once.
            Item {
                readonly property int count: (plugin.revision, plugin.edits.manual.length + plugin.edits.apart.length)
                visible: count > 0
                width: parent.width
                height: visible ? automaticAll.height + 4 : 0
                UiText {
                    width: parent.width - automaticAll.width - theme.fontSize
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                    text: qsTr("Put right by hand: %n photo(s) or series.", "", parent.count)
                }
                ShellButton {
                    id: automaticAll
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("All automatic")
                    onTapped: plugin.automaticAll()
                }
            }
            Item {
                width: parent.width
                height: asOneSwitch.height
                UiText {
                    width: parent.width - asOneSwitch.width - theme.fontSize
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    color: theme.foreground
                    text: qsTr("One card a series in the grid")
                }
                Toggle {
                    id: asOneSwitch
                    anchors.right: parent.right
                    checked: plugin.asOne
                    onToggled: {
                        plugin.asOne = !plugin.asOne
                        pinhole.setSetting("asOne", plugin.asOne)
                    }
                }
            }
        }
    }

    Studio {
        id: studio
        anchors.fill: parent
        plugin: plugin
    }
}
