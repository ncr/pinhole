import QtQuick
import Pinhole.Plugin

// Series: photos of one subject taken close together, the way a moment
// worth a photo rarely gets just one, made into a short GIF or video to
// send (Studio.qml). Finding them is pinhole-series's (finder/), which
// keeps what it learns of each photo in the plugin's cache, so only new
// photos take it any time.
PinholePlugin {
    id: plugin
    covering: studio.shown

    // Every series, in the library's order: the positions of its shots.
    property var series: []
    // A photo's series by its position, for those in one.
    property var seriesAt: ({})
    property bool finding: false
    property bool findAgain: false
    property bool found: false

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
    // The finder's lines (each a series' ids) as library positions.
    function take(out) {
        const list = []
        for (const line of out.split("\n")) {
            if (line === "")
                continue
            const shots = line.split(" ").map(id => pinhole.library.indexOf(id)).filter(i => i >= 0)
            if (shots.length >= 3)
                list.push(shots.sort((a, b) => a - b))
        }
        list.sort((a, b) => a[0] - b[0])
        const at = {}
        list.forEach((shots, k) => shots.forEach(i => at[i] = k))
        series = list
        seriesAt = at
        found = true
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
        PluginAction {
            readonly property int k: pinhole.viewer.open ? plugin.of(pinhole.viewer.index) : -1
            place: "viewer"
            text: k >= 0 ? qsTr("Series of %1").arg(plugin.series[k].length) : ""
            key: "G"
            shown: k >= 0
            onTriggered: studio.openSeries(k, false)
        }
    ]

    help: [
        ["G", qsTr("Series")],
        ["← →", qsTr("Previous, next series")],
        ["1–5", qsTr("Motion")],
        ["P L O", qsTr("Pace, look, shape")],
        ["T", qsTr("Steady")],
        ["Space", qsTr("Pause")],
        ["⏎", qsTr("Make a GIF")],
        ["⇧ ⏎", qsTr("Make a video")]
    ]

    settings: Component {
        UiText {
            width: parent ? parent.width : 0
            wrapMode: Text.WordWrap
            color: theme.foreground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            text: !plugin.found ? qsTr("Looking for series…")
                : plugin.series.length === 0 ? qsTr("No series here yet.")
                : qsTr("%n series here: G in the grid, or on a photo of one.", "", plugin.series.length)
        }
    }

    Studio {
        id: studio
        anchors.fill: parent
        plugin: plugin
    }
}
