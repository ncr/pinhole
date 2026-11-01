import QtQuick
import Pinhole.Plugin

// The calendar's keys, while it is open (Command.qml): here, not in
// Calendar.qml, so that they are in the help before it is first made.
Item {
    id: calendarKeys
    // The calendar, once it has been opened; null before.
    required property var calendar
    readonly property bool shown: calendar !== null && calendar.shown

    Command {
        name: "calendar.close"
        keys: ["Escape", "Y"]
        keysText: "Esc"
        keyLayer: "plugin"
        when: calendarKeys.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: calendarKeys.calendar.close()
    }
    Command {
        name: "calendar.show"
        text: qsTr("There: that day in the grid")
        section: "Year calendar"
        keys: ["Return", "Enter", "Space"]
        keysText: "Enter"
        keyLayer: "plugin"
        when: calendarKeys.shown
        mouse: qsTr("Click a day")
        touchpad: qsTr("Tap a day")
        onTriggered: calendarKeys.calendar.jump()
    }
    Command {
        name: "calendar.previousDay"
        text: qsTr("A day, a week")
        section: "Year calendar"
        keys: ["Up"]
        keysText: "↑ ↓ ← →"
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarKeys.calendar.move(-1)
    }
    Command {
        name: "calendar.nextDay"
        keys: ["Down"]
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarKeys.calendar.move(1)
    }
    Command {
        name: "calendar.previousWeek"
        keys: ["Left"]
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarKeys.calendar.move(-7)
    }
    Command {
        name: "calendar.nextWeek"
        keys: ["Right"]
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendarKeys.calendar.move(7)
    }
    Command {
        name: "calendar.previousYear"
        text: qsTr("A year")
        section: "Year calendar"
        keys: ["PgUp"]
        keysText: "PgUp PgDn"
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarKeys.calendar.moveYear(-1)
    }
    Command {
        name: "calendar.nextYear"
        keys: ["PgDown"]
        keyLayer: "plugin"
        when: calendarKeys.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarKeys.calendar.moveYear(1)
    }
    Command {
        name: "calendar.first"
        text: qsTr("First, last day with photos")
        section: "Year calendar"
        keys: ["Home"]
        keysText: "Home End"
        keyLayer: "plugin"
        when: calendarKeys.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarKeys.calendar.end(-1)
    }
    Command {
        name: "calendar.last"
        keys: ["End"]
        keyLayer: "plugin"
        when: calendarKeys.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendarKeys.calendar.end(1)
    }
}
