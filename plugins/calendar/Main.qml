import QtQuick
import Pinhole.Plugin
import "Days.js" as Days

// Year calendar: every day of every year as a square, lit in the theme's
// accent by how many photos were taken that day, one row of weeks a year,
// as GitHub draws contributions (Calendar.qml). Y opens it at the year of
// the photo the grid is on; a day, clicked or picked with the arrows and
// Enter, is shown in the grid.
//
// The library is counted into days (Days.js) once, a little a frame so the
// grid never waits, and again whenever it is read again.
PinholePlugin {
    id: plugin
    covering: calendar.shown

    // What was counted (Days.js finish): every year with a photo, oldest
    // first, each year's row by the year, and how many photos light a
    // square fully and make it glow: most days stay below.
    property var years: []
    property var rowOf: ({})
    property int full: 2
    property int glowing: 3
    property int total: 0
    property int dayCount: 0
    property bool counted: false

    // The week's rows start on the locale's first day.
    readonly property int firstWeekday: Qt.locale().firstDayOfWeek

    // Counting, a few milliseconds a frame so the grid never waits; what it
    // has so far.
    property var counting: null
    function count() {
        counting = Days.start(pinhole.library.count, firstWeekday)
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
        if (!Days.count(c, i => pinhole.library.item(i), 3))
            return
        stepping.stop()
        counting = null
        const done = Days.finish(c)
        full = done.full
        glowing = done.glowing
        total = done.total
        dayCount = done.dayCount
        rowOf = done.rowOf
        years = done.years
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
            shown: pinhole.library.count > 0
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

    help: [["Y", qsTr("Every day's photos")]]

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
