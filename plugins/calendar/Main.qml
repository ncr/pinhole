import QtQuick
import Pinhole.Plugin
import "Days.js" as Days

// Year calendar: every day of every year as a square, lit in the theme's
// accent by how many photos were taken that day, one row of weeks a year,
// as GitHub draws contributions (Calendar.qml). Y opens it at the year of
// the photo the grid is on; a day, clicked or picked with the arrows and
// Enter, is shown in the grid.
//
// The library is counted into days (Days.js) from its column of days, at
// once: again when it is read again while the calendar shows, else when
// it next opens (or its settings show), so a library being built, read
// again every few seconds, is not counted each time. What the grid hides
// (Screenshots' hide mode, say) is not counted, as the grid does not show
// it.
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

    function count() {
        stale = false
        const library = pinhole.library
        const done = Days.finish(Days.count(library.field("day"), library.field("id"), firstWeekday,
                                            pinhole.hiddenCount > 0 ? i => pinhole.isHidden(i) : null))
        full = done.full
        glowing = done.glowing
        total = done.total
        dayCount = done.dayCount
        rowOf = done.rowOf
        years = done.years
        counted = true
    }

    // Read again, or photos hidden from the grid or shown again: counted
    // again while it shows, once for both when the library's reading moves
    // what is hidden; else when it is next needed.
    property bool stale: false
    function recount() {
        if (calendar.shown) {
            Qt.callLater(plugin.count)
        } else {
            stale = true
        }
    }
    function fresh() {
        if (stale)
            count()
    }
    Connections {
        target: pinhole.library
        function onChanged() { plugin.recount() }
    }
    Connections {
        target: pinhole
        function onHiddenChanged() { plugin.recount() }
    }
    Component.onCompleted: count()

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
            Component.onCompleted: plugin.fresh()
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
