import QtQuick
import Pinhole.Plugin

// Year calendar: every day of every year as a square, lit in the theme's
// accent by how many photos were taken that day, one row of weeks a year,
// as GitHub draws contributions (Calendar.qml). Y opens it at the year of
// the photo the grid is on; a day, clicked or picked with the arrows and
// Enter, is shown in the grid.
//
// The library is counted into days once, a few thousand photos a frame so
// the grid never waits, and again whenever it is read again.
PinholePlugin {
    id: plugin
    covering: calendar.shown

    // Every year with a photo, oldest first: {year, days (365 or 366),
    // offset (the weekday row of 1 January), counts and first (each day's
    // photos and its first photo's position, -1 for none), total}.
    property var years: []
    // A year's row in `years`, by the year.
    property var rowOf: ({})
    // The photos a day has for its square to be lit fully, and from which
    // it glows: most days stay below.
    property int full: 1
    property int glowing: 1
    property int total: 0
    property int dayCount: 0
    property bool counted: false

    // A day of its year, from 0.
    function dayOf(date) {
        const y = date.getFullYear()
        return Math.round((Date.UTC(y, date.getMonth(), date.getDate()) - Date.UTC(y, 0, 1)) / 86400000)
    }
    // The week's rows start on the locale's first day.
    readonly property int firstWeekday: Qt.locale().firstDayOfWeek
    function weekdayRow(date) { return (date.getDay() - firstWeekday + 7) % 7 }

    // Counting under way: where it is and what it has so far.
    property var counting: null
    function count() {
        counting = { at: 0, n: pinhole.library.count, byYear: ({}) }
        stepping.restart()
    }
    Timer {
        id: stepping
        interval: 1
        repeat: true
        onTriggered: plugin.step()
    }
    function step() {
        const c = counting
        if (!c) {
            stepping.stop()
            return
        }
        const end = Math.min(c.n, c.at + 3000)
        for (let i = c.at; i < end; ++i) {
            const ms = pinhole.library.item(i).date
            if (!(ms > 0))
                continue
            const date = new Date(ms)
            const y = date.getFullYear()
            let year = c.byYear[y]
            if (!year) {
                const leap = new Date(y, 1, 29).getMonth() === 1
                year = c.byYear[y] = { year: y, days: leap ? 366 : 365, offset: weekdayRow(new Date(y, 0, 1)),
                                       counts: new Array(366).fill(0), first: new Array(366).fill(-1), total: 0 }
            }
            const day = dayOf(date)
            // The library is in time order: a day's first is its earliest.
            if (year.counts[day]++ === 0)
                year.first[day] = i
            ++year.total
        }
        c.at = end
        if (end < c.n)
            return
        stepping.stop()
        counting = null
        finish(c.byYear)
    }
    function finish(byYear) {
        const list = Object.keys(byYear).map(y => byYear[y]).sort((a, b) => a.year - b.year)
        const rows = {}
        const busy = []
        let sum = 0
        list.forEach((year, row) => {
            rows[year.year] = row
            sum += year.total
            for (let d = 0; d < year.days; ++d)
                if (year.counts[d] > 0)
                    busy.push(year.counts[d])
        })
        busy.sort((a, b) => a - b)
        const at = q => busy.length > 0 ? busy[Math.min(busy.length - 1, Math.floor(busy.length * q))] : 1
        full = Math.max(2, at(0.97))
        glowing = Math.max(full + 1, at(0.995))
        total = sum
        dayCount = busy.length
        rowOf = rows
        years = list
        counted = true
    }

    // Read again: counted again, once it has settled.
    Timer { id: later; interval: 1200; onTriggered: plugin.count() }
    Connections {
        target: pinhole.library
        function onChanged() { later.restart() }
    }
    Component.onCompleted: later.start()

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Calendar")
            key: "Y"
            shown: plugin.years.length > 0
            onTriggered: calendar.open()
        }
    ]

    tourReady: counted && years.length > 0
    tour: [
        {
            title: qsTr("Every day, year by year"),
            text: qsTr("Y shows each day of each year as a square, brighter the more photos it has: a click, or the arrows and Enter, shows that day in the grid."),
            action: gridAction
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab), this tour can be shown again."),
            settings: true
        }
    ]

    help: [["Y", qsTr("Year calendar: every day's photos")]]

    settings: Component {
        UiText {
            width: parent ? parent.width : 0
            wrapMode: Text.WordWrap
            color: theme.foreground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            text: !plugin.counted ? qsTr("Counting the days…")
                : plugin.years.length === 0 ? qsTr("No photos with a date yet.")
                : qsTr("%Ln days with photos, from %1 to %2: Y in the grid.", "", plugin.dayCount)
                    .arg(plugin.years[0].year).arg(plugin.years[plugin.years.length - 1].year)
        }
    }

    Calendar {
        id: calendar
        anchors.fill: parent
        plugin: plugin
    }
}
