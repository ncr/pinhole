import QtQuick
import Pinhole.Plugin
// Screenshots are told apart as the Screenshots plugin tells them.

// On this day: the photos taken on today's date in earlier years, a year
// at a time (Days.qml), from a button in the grid that shows only when
// there are any. The library is read into its days whole (its columns of
// days and of videos, not a photo at a time): at once when it is read
// again while the years show, else ten seconds later, for the button's
// count, the changes meanwhile with it (a library being built changes
// every few seconds), and at once on opening, before anything can be
// opened by a position gone or of another library. Today's photos, a new
// day's at midnight and the settings' are then a look-up. What the grid
// hides is left out, as it is there.
PinholePlugin {
    id: plugin
    readonly property var days: daysView.item
    covering: days !== null && days.shown
    function openDays() {
        daysView.active = true
        days.open()
    }

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
    // The library read again since: the days are read again by `reindex`,
    // or at once by fresh() when they are needed.
    property bool stale: false
    // The screenshots' ids once found, and their positions now.
    property var shotIds: []
    property var shotAt: ({})
    // Today, as the years were last counted for it.
    property var today: new Date()
    // The earlier years with photos of this day, the latest first:
    // [{ year, ago, label, exact, photos }], each year's photos (their
    // positions) in the order they were taken. `label` is the years they
    // were taken in: "2025", or "2025–2026" for days around New Year;
    // `exact` today's very date that year (yyyymmdd), 0 for a year without
    // it (the 29th of February).
    property var years: []
    property string yearsKey: ""

    function dayKey(year, month, day) { return year * 10000 + (month + 1) * 100 + day }
    function index() {
        stale = false
        reindex.stop()
        const days = pinhole.library.field("day")
        const map = {}
        let first = 0
        // A run grows while the photos after it are of its day.
        let runs = null
        let previous = 0
        for (let i = 0, count = days.length; i < count; ++i) {
            const d = days[i]
            if (d === 0) {
                previous = 0
                continue
            }
            if (d === previous) {
                runs[runs.length - 1] = i + 1
                continue
            }
            previous = d
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
    // library (pinhole.screenshots), whether or not it is on.
    property var finding: null
    function findShots() {
        if (finding)
            finding.cancel()
        finding = null
        // No photos, no screenshots: and with no source shown (a first
        // run, every source's plugin off) no library to ask either.
        if (pinhole.library.count === 0) {
            shotIds = []
            placeShots()
            recount()
            return
        }
        // Read only, away from the window; while the service writes the
        // index (as a round starts), waited for rather than failed.
        const found = pinhole.screenshots()
        finding = found
        found.then(ids => {
            if (finding !== found)
                return
            finding = null
            shotIds = ids
            placeShots()
            recount()
        }).catch(error => {
            if (finding === found)
                finding = null
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

    // The days as the library is now, for what is about to use them.
    function fresh() {
        if (stale)
            index()
    }
    // Read again ten seconds after the first change: those that come
    // meanwhile wait with it.
    Timer {
        id: reindex
        interval: 10000
        onTriggered: plugin.index()
    }

    // Screenshots hidden from the grid (Screenshots' hide mode): they stay
    // out here too, whatever the setting says.
    property bool shotsHidden: false

    function recount() {
        // Old positions: counted once the days are read again.
        if (!indexed || stale)
            return
        const now = new Date()
        today = now
        const thisYear = now.getFullYear(), month = now.getMonth(), date = now.getDate()
        // The 28th of February in a year without a 29th has the 29th's
        // photos of the years with one.
        const leapDay = month === 1 && date === 28 && new Date(thisYear, 1, 29).getMonth() === 2
        // What the grid hides (Screenshots' hide mode, say) is left out,
        // as the grid leaves it out.
        hiddenSeen = pinhole.hiddenCount
        const hiding = hiddenSeen > 0
        shotsHidden = hiding && shotIds.some(id => pinhole.isHidden(id))
        const list = []
        for (let year = thisYear - 1; firstYear > 0 && year >= firstYear; --year) {
            // This date that year, the 29th of February the 28th when
            // there is none; and the real days around it.
            let centre = new Date(year, month, date)
            const real = centre.getMonth() === month
            if (!real)
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
                exact: real ? dayKey(year, month, date) : 0,
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

    // Read again: the positions moved, so the days are read again too, at
    // once while the years show, before anything can be opened by an old
    // one; else within ten seconds, for the button.
    Connections {
        target: pinhole.library
        function onChanged() {
            if (plugin.days !== null && plugin.days.shown) {
                plugin.index()
            } else {
                plugin.stale = true
                if (!reindex.running)
                    reindex.start()
            }
        }
    }
    // Photos hidden from the grid, or shown again: counted again. After the
    // library's own change, when it is read again too (the grid's comes
    // first), so the days are this reading's and the overlay stays as it is
    // when none of its photos moved. With the days still to be read again
    // (the library read again in the last ten seconds), photos hidden or
    // shown again have them read at once, for the button's count; not those
    // only moved by the library's reading, as they are by every photo come
    // before them, or a library being built would be read each time.
    property int hiddenSeen: 0
    Connections {
        target: pinhole
        function onHiddenChanged() { Qt.callLater(plugin.hiddenMoved) }
        // Another source's library: its days at once, not the last one's
        // count for ten seconds.
        function onCacheDirChanged() { plugin.fresh() }
    }
    function hiddenMoved() {
        if (stale && pinhole.hiddenCount !== hiddenSeen)
            index()
        else
            recount()
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
            onTriggered: plugin.openDays()
        }
    ]

    help: [["D", qsTr("On this day, in earlier years")]]

    // A real year of one's own: the latest with photos of this day, a
    // photo of the very date if it has one (on the 29th of February, a
    // year with one; the 28th in another is around it).
    tourReady: indexed && years.length > 0
    readonly property var tourPhoto: {
        if (years.length === 0)
            return null
        const year = years[0]
        const exact = year.exact !== 0 ? year.photos.find(i => dayOf[i] === year.exact) : undefined
        return { photo: exact || year.photos[0], exact: exact !== undefined, ago: year.ago }
    }
    tour: [
        {
            title: qsTr("Years ago today"),
            text: !tourPhoto ? ""
                : (tourPhoto.exact ? qsTr("Taken %n years ago today, in %1.", "", tourPhoto.ago)
                                   : qsTr("Taken around this day %n years ago, in %1.", "", tourPhoto.ago))
                  .arg(Math.floor(dayOf[tourPhoto.photo] / 10000)),
            photo: () => {
                plugin.fresh()
                return tourPhoto ? tourPhoto.photo : -1
            }
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
            Component.onCompleted: plugin.fresh()
            width: parent ? parent.width : 0
            spacing: 6
            SettingNote {
                text: !plugin.indexed ? qsTr("Reading the library…")
                    : plugin.years.length === 0 ? qsTr("Nothing from this day in earlier years.")
                    : qsTr("This day in %n earlier years: D in the grid.", "", plugin.years.length)
            }
            Choices {
                numbered: false
                options: [{ value: 0, text: qsTr("The day") }, { value: 1, text: qsTr("± 1 day") }, { value: 3, text: qsTr("± 3 days") }, { value: 7, text: qsTr("± a week") }]
                value: plugin.span
                onChosen: value => plugin.set("span", value)
            }
            Repeater {
                model: [["videos", qsTr("Videos")], ["screenshots", qsTr("Screenshots")]]
                delegate: SettingSwitch {
                    required property var modelData
                    // Screenshots hidden from the grid stay out whatever
                    // this says.
                    readonly property bool held: modelData[0] === "screenshots" && plugin.shotsHidden
                    text: modelData[1]
                    usable: !held
                    checked: plugin[modelData[0]] && !held
                    onToggled: plugin.set(modelData[0], !checked)
                }
            }
            SettingNote {
                dim: true
                visible: plugin.shotsHidden
                text: qsTr("Screenshots are hidden from the grid (Screenshots, ⇧\u00a0S), so not here either.")
            }
        }
    }

    // The count belongs to the first frame; the large view is only needed
    // after D. Retain it after closing so returning from a photo is instant.
    Loader {
        id: daysView
        anchors.fill: parent
        active: false
        sourceComponent: Days { plugin: daysView.parent }
    }
}
