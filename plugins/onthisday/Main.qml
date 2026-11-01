import QtQuick
import Pinhole.Plugin
// Screenshots are told apart as the Screenshots plugin tells them.
import "../screenshots/Find.js" as Find

// On this day: the photos taken on today's date in earlier years, a year
// at a time (Days.qml), from a button in the grid that shows only when
// there are any. The library is read into its days whenever it is read
// again, at once and whole (its columns of days and of videos, not a
// photo at a time), before anything can be opened by a position gone or
// of another library; today's photos, a new day's at midnight and the
// settings' are then a look-up. What the grid hides is left out, as it
// is there.
PinholePlugin {
    id: plugin
    covering: days.shown

    // How many days either side of the date count, each year: 0 is the
    // day itself, more for the years with few photos.
    property int span: pinhole.setting("span", 0)
    property bool videos: pinhole.setting("videos", true)
    property bool screenshots: pinhole.setting("screenshots", false)
    function set(name, value) {
        plugin[name] = value
        pinhole.setSetting(name, value)
        recount()
    }

    // The library's positions by the day they were taken on, in local
    // time (yyyymmdd), as runs: [from, to, from, to, ...], one run a day
    // as the library is in the order they were taken. A photo with no
    // date has none. With every photo's day and whether it is a video,
    // and the first year with a dated photo.
    property var byDay: ({})
    property var dayOf: []
    property var isVideo: []
    property int firstYear: 0
    property bool indexed: false
    // The screenshots' ids once found, and their positions now.
    property var shotIds: []
    property var shotAt: ({})
    // Today, as the years were last counted for it.
    property var today: new Date()
    // The earlier years with photos of this day, the latest first:
    // [{ year, ago, label, exact, photos }], each year's photos (their
    // positions) in the order they were taken. `label` is the years they
    // were taken in: "2025", or "2025–2026" for days around New Year;
    // `exact` the very date that year.
    property var years: []
    property string yearsKey: ""

    function dayKey(year, month, day) { return year * 10000 + (month + 1) * 100 + day }
    function index() {
        const days = pinhole.library.field("day")
        const map = {}
        let first = 0
        // A run grows while the photos after it are of its day.
        let runs = null
        for (let i = 0; i < days.length; ++i) {
            const d = days[i]
            if (d === 0)
                continue
            if (i > 0 && days[i - 1] === d) {
                runs[runs.length - 1] = i + 1
                continue
            }
            runs = map[d] || (map[d] = [])
            runs.push(i, i + 1)
            if (first === 0 || d < first * 10000)
                first = Math.floor(d / 10000)
        }
        byDay = map
        dayOf = days
        isVideo = pinhole.library.field("video")
        firstYear = first
        placeShots()
        indexed = true
        recount()
        findShots()
    }

    // The screenshots, as the Screenshots plugin finds them in the
    // library (Find.js), whether or not it is on.
    property var finding: null
    function findShots() {
        if (finding)
            finding.cancel()
        const library = pinhole.library.dir + "/library.sqlite"
        const tables = pinhole.run("sqlite3", ["-readonly", library, Find.tablesQuery])
        finding = tables
        tables.then(result => {
            if (finding !== tables)
                return
            const found = pinhole.run("sqlite3", ["-readonly", library, Find.query(result.stdout.split("\n"))])
            finding = found
            return found.then(result => {
                if (finding !== found)
                    return
                finding = null
                shotIds = result.stdout.split("\n").filter(id => id !== "")
                placeShots()
                recount()
            })
        }).catch(error => {
            if (error.code !== "cancelled")
                pinhole.warn(error)
        })
    }

    function placeShots() {
        const at = {}
        for (const id of shotIds) {
            const i = pinhole.library.indexOf(id)
            if (i >= 0)
                at[i] = true
        }
        shotAt = at
    }

    function recount() {
        if (!indexed)
            return
        const now = new Date()
        today = now
        const thisYear = now.getFullYear(), month = now.getMonth(), date = now.getDate()
        // The 28th of February in a year without a 29th has the 29th's
        // photos of the years with one.
        const leapDay = month === 1 && date === 28 && new Date(thisYear, 1, 29).getMonth() === 2
        // What the grid hides (Screenshots' hide mode, say) is left out,
        // as the grid leaves it out.
        const hiding = pinhole.hiddenCount > 0
        const list = []
        for (let year = thisYear - 1; firstYear > 0 && year >= firstYear; --year) {
            // This date that year, the 29th of February the 28th when
            // there is none; and the real days around it.
            let centre = new Date(year, month, date)
            if (centre.getMonth() !== month)
                centre = new Date(year, month + 1, 0)
            const keys = []
            for (let d = -span; d <= span; ++d) {
                const day = new Date(year, centre.getMonth(), centre.getDate() + d)
                keys.push(dayKey(day.getFullYear(), day.getMonth(), day.getDate()))
            }
            if (leapDay && new Date(year, 1, 29).getMonth() === 1)
                keys.push(dayKey(year, 1, 29))
            const photos = []
            for (const key of keys) {
                const runs = byDay[key] || []
                for (let r = 0; r < runs.length; r += 2)
                    for (let i = runs[r]; i < runs[r + 1]; ++i)
                        if ((videos || !isVideo[i]) && (screenshots || !shotAt[i])
                                && !(hiding && pinhole.isHidden(i)))
                            photos.push(i)
            }
            if (photos.length === 0)
                continue
            photos.sort((a, b) => a - b)
            const from = Math.floor(dayOf[photos[0]] / 10000), to = Math.floor(dayOf[photos[photos.length - 1]] / 10000)
            list.push({
                year: year,
                ago: thisYear - year,
                label: from === to ? String(from) : from + "–" + to,
                exact: dayKey(year, centre.getMonth(), centre.getDate()),
                photos: photos
            })
        }
        // The same photos at the same places as before, as after most
        // readings of the library: the overlay is left as it is.
        const key = list.map(y => y.ago + ":" + y.photos.join(",")).join(";")
        if (key === yearsKey)
            return
        yearsKey = key
        years = list
    }

    // Read again: the positions moved, so the days are read again too,
    // at once, before anything can be opened by an old one.
    Connections {
        target: pinhole.library
        function onChanged() { plugin.index() }
    }
    // Photos hidden from the grid, or shown again: counted again. After the
    // library's own change, when it is read again too (the grid's comes
    // first), so the days are this reading's and the overlay stays as it is
    // when none of its photos moved.
    Connections {
        target: pinhole
        function onHiddenChanged() { Qt.callLater(plugin.recount) }
    }
    Component.onCompleted: index()
    // Midnight: another day's photos. Looked at every minute rather than
    // timed to midnight, which a computer asleep then would miss.
    Timer {
        interval: 60 * 1000
        repeat: true
        running: true
        onTriggered: {
            const now = new Date()
            if (now.getDate() !== plugin.today.getDate() || now.getMonth() !== plugin.today.getMonth()
                    || now.getFullYear() !== plugin.today.getFullYear())
                plugin.recount()
        }
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("%n years", "", plugin.years.length)
            key: "D"
            shown: plugin.years.length > 0
            onTriggered: days.open()
        }
    ]

    help: [["D", qsTr("On this day, in earlier years")]]

    // A real year of one's own: the latest with photos of this day, a
    // photo of the very date if it has one.
    tourReady: indexed && years.length > 0
    readonly property var tourPhoto: {
        if (years.length === 0)
            return null
        const year = years[0]
        const exact = year.photos.find(i => dayOf[i] === year.exact)
        return { photo: exact || year.photos[0], exact: exact !== undefined, ago: year.ago }
    }
    tour: [
        {
            title: qsTr("Years ago today"),
            text: !tourPhoto ? ""
                : (tourPhoto.exact ? qsTr("Taken %n years ago today, in %1.", "", tourPhoto.ago)
                                   : qsTr("Taken around this day %n years ago, in %1.", "", tourPhoto.ago))
                  .arg(Math.floor(dayOf[tourPhoto.photo] / 10000)),
            photo: () => tourPhoto ? tourPhoto.photo : -1
        },
        {
            title: qsTr("Every year"),
            text: qsTr("D shows this day in every year with photos of it, the latest first. A photo opens full window; Esc comes back here."),
            action: gridAction
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab): the day alone or a few days around it, for the years with fewer photos, and whether videos and screenshots count."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: !plugin.indexed ? qsTr("Reading the library…")
                    : plugin.years.length === 0 ? qsTr("Nothing from this day in earlier years.")
                    : qsTr("This day in %n earlier years: D in the grid.", "", plugin.years.length)
            }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    model: [[0, qsTr("The day")], [1, qsTr("± 1 day")], [3, qsTr("± 3 days")], [7, qsTr("± a week")]]
                    delegate: ShellButton {
                        required property var modelData
                        flat: true
                        text: modelData[1]
                        active: plugin.span === modelData[0]
                        onTapped: plugin.set("span", modelData[0])
                    }
                }
            }
            Repeater {
                model: [["videos", qsTr("Videos")], ["screenshots", qsTr("Screenshots")]]
                delegate: Item {
                    required property var modelData
                    width: parent.width
                    height: toggle.implicitHeight
                    UiText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData[1]
                        color: theme.foreground
                    }
                    Toggle {
                        id: toggle
                        anchors.right: parent.right
                        checked: plugin[modelData[0]]
                        onToggled: plugin.set(modelData[0], !checked)
                    }
                }
            }
        }
    }

    Days {
        id: days
        anchors.fill: parent
        plugin: plugin
    }
}
