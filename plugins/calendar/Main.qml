import QtQuick
import Pinhole.Plugin
import "CalendarDays.js" as Days

// Year calendar: every day of every year as a square, lit in the theme's
// accent by how many photos were taken that day, one row of weeks a year,
// as GitHub draws contributions (Calendar.qml). Y opens it at the year of
// the photo the grid is on; a day, clicked or picked with the arrows and
// Enter, is shown in the grid.
//
// The library is counted into days (CalendarDays.js) from its column of
// days, at once: again when it is read again while the calendar shows,
// else when it next opens (or its settings show), so a library being
// built, read again every few seconds, is not counted each time. What the
// grid hides (Screenshots' hide mode, say) is not counted, as the grid
// does not show it.
//
// The calendar itself, its rows of squares a few hundred items, is made
// the first time it opens, and kept: most launches never show it.
PinholePlugin {
    id: plugin
    // The calendar once made (Calendar.qml), null before.
    readonly property var calendar: calendarView.item
    function open() {
        fresh()
        calendarView.open()
    }

    // What was counted (CalendarDays.js finish): every year with a photo,
    // oldest first, each year's row by the year, and how many photos light
    // a square fully and make it glow: most days stay below.
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
        // The photos the grid shows (API 15): those hidden left out.
        const library = pinhole.library
        const done = Days.finish(Days.count(library.field("day", { shown: true }), library.field("id", { shown: true }),
                                            firstWeekday, null))
        full = done.full
        glowing = done.glowing
        total = done.total
        dayCount = done.dayCount
        rowOf = done.rowOf
        years = done.years
        counted = true
    }

    // Read again, or photos hidden from the grid or shown again (settled,
    // once for both when they come at once): counted again while it
    // shows, else when it is next needed.
    property bool stale: true
    function recount() {
        if (calendarView.shown) {
            plugin.count()
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
        function onSettled(how) { plugin.recount() }
    }
    // The histogram is needed on opening the calendar or its settings.

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Calendar")
            key: "Y"
            shown: pinhole.library.count > 0
            onTriggered: plugin.open()
        }
    ]

    tourReady: counted && years.length > 0
    tour: [
        {
            title: qsTr("Every day, year by year"),
            text: qsTr("Y shows each day of each year as a square, brighter the more photos it has: a click, or ↑\u00a0↓\u00a0←\u00a0→ and ⏎, shows that day in the grid."),
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
        SettingNote {
            Component.onCompleted: plugin.fresh()
            width: parent ? parent.width : 0
            text: !plugin.counted ? qsTr("Counting the days…")
                : plugin.years.length === 0 ? qsTr("No photos with a date yet.")
                : qsTr("%Ln days with photos, from %1 to %2: Y in the grid.", "", plugin.dayCount)
                    .arg(plugin.years[0].year).arg(plugin.years[plugin.years.length - 1].year)
        }
    }

    // Made the first time it opens, and kept.
    PluginView {
        id: calendarView
        // The Calendar is given its view as it is made: its bindings read
        // the plugin through it from the first.
        content: Component { Calendar { view: calendarView } }
        toggleKey: "Y"
    }

    // Its keys, here rather than in Calendar.qml, so that they are in the
    // help before it is first made.
    Command {
        name: "calendar.show"
        text: qsTr("There: that day in the grid")
        section: "Year calendar"
        keys: ["Return", "Enter", "Space"]
        keysText: "⏎"
        keyLayer: "plugin"
        when: calendarView.shown
        mouse: qsTr("Click a day")
        touchpad: qsTr("Tap a day")
        onTriggered: calendarView.item.jump()
    }
    Command {
        name: "calendar.previousDay"
        text: qsTr("A day, a week")
        section: "Year calendar"
        keys: ["Up"]
        keysText: "↑ ↓ ← →"
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarView.item.move(-1)
    }
    Command {
        name: "calendar.nextDay"
        keys: ["Down"]
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarView.item.move(1)
    }
    Command {
        name: "calendar.previousWeek"
        keys: ["Left"]
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarView.item.move(-7)
    }
    Command {
        name: "calendar.nextWeek"
        keys: ["Right"]
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarView.item.move(7)
    }
    Command {
        name: "calendar.previousYear"
        text: qsTr("A year")
        section: "Year calendar"
        keys: ["PgUp"]
        keysText: "PgUp PgDn"
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarView.item.moveYear(-1)
    }
    Command {
        name: "calendar.nextYear"
        keys: ["PgDown"]
        keyLayer: "plugin"
        when: calendarView.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarView.item.moveYear(1)
    }
    Command {
        name: "calendar.first"
        text: qsTr("First, last day with photos")
        section: "Year calendar"
        keys: ["Home"]
        keysText: "Home End"
        keyLayer: "plugin"
        when: calendarView.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarView.item.end(-1)
    }
    Command {
        name: "calendar.last"
        keys: ["End"]
        keyLayer: "plugin"
        when: calendarView.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarView.item.end(1)
    }
}
