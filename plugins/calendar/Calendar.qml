import QtQuick
import Pinhole.Plugin
import "Days.js" as Days

// The calendar, full window: a row of weeks for each year, oldest at the
// top as in the grid, each day a square in the accent, as bright as it has
// photos, the busiest glowing. A cursor goes from day to day under the
// pointer or with the arrows, saying the day and its photos; a click or
// Enter closes it on that day's first photo in the grid. It opens on the
// year of the photo the grid is on, its squares sweeping in from there.
Item {
    id: calendar
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    // The cursor: a day of a year. Its row follows the year as the library
    // is counted again; if the year goes (another source is shown), the
    // nearest that stays.
    property int cursorYear: 0
    property int day: 0
    readonly property int row: plugin.rowOf[cursorYear] ?? -1
    readonly property var year: row >= 0 ? plugin.years[row] : null
    readonly property int photos: year ? year.counts[day] : 0
    readonly property var date: Days.dateOf(cursorYear, 0, 1 + day)
    Connections {
        target: calendar.plugin
        function onYearsChanged() {
            if (!calendar.shown)
                return
            if (calendar.plugin.years.length === 0)
                calendar.close()
            else if (calendar.waiting)
                calendar.place()
            else if (calendar.row < 0) {
                const y = calendar.plugin.years[Days.nearest(calendar.plugin.years, calendar.cursorYear)]
                calendar.set(y.year, Math.min(calendar.day, y.days - 1))
            }
        }
    }

    // Opening: the squares come in, rippling out from the year it opens on.
    property int openRow: 0
    property real sweep: 2
    NumberAnimation on sweep { id: sweeping; running: false; from: 0; to: 2; duration: 1100; easing.type: Easing.OutQuad }
    // How far in a square is, 0 to 1, by its column and its row's distance
    // from the one opened on.
    function grown(column, row) {
        const t = (sweep - column / 54 * 0.6 - Math.min(Math.abs(row - openRow), 6) * 0.07) / 0.35
        return Math.max(0, Math.min(1, t))
    }

    // Opened before the library is counted: on its way, and placed once it is.
    property bool waiting: false
    function open() {
        today = new Date()
        shown = true
        waiting = !plugin.counted
        if (!waiting)
            place()
    }
    // On the day of the photo the grid is on, else the last day with photos.
    function place() {
        waiting = false
        if (plugin.years.length === 0) {
            close()
            return
        }
        const at = pinhole.currentIndex()
        const ms = at >= 0 ? pinhole.library.item(at).date : 0
        const date = ms !== 0 ? new Date(ms) : null
        if (date && plugin.rowOf[date.getFullYear()] !== undefined)
            set(date.getFullYear(), Days.dayOf(date))
        else
            end(1)
        openRow = row
        sweeping.restart()
        Qt.callLater(() => {
            years.forceLayout()
            years.positionViewAtIndex(row, ListView.Center)
        })
    }
    function close() {
        shown = false
        waiting = false
    }
    // That day's first photo, in the grid.
    function jump() {
        const id = year ? year.first[day] : ""
        const at = id !== "" ? pinhole.library.indexOf(id) : -1
        if (at < 0) {
            nudge.restart()
            return
        }
        close()
        pinhole.reveal(at)
    }

    function set(y, d) {
        cursorYear = y
        day = d
        follow()
    }
    // The cursor `delta` days on; past a year without photos, to the next
    // year with some that way.
    function move(delta) {
        if (!year)
            return
        const to = Days.dateOf(cursorYear, 0, 1 + day + delta)
        if (plugin.rowOf[to.getFullYear()] !== undefined) {
            set(to.getFullYear(), Days.dayOf(to))
            return
        }
        const next = row + (delta > 0 ? 1 : -1)
        if (next >= 0 && next < plugin.years.length)
            set(plugin.years[next].year, delta > 0 ? 0 : plugin.years[next].days - 1)
    }
    // A year up or down, on the same day.
    function moveYear(delta) {
        const next = row + delta
        if (next < 0 || next >= plugin.years.length)
            return
        const y = plugin.years[next]
        set(y.year, Math.min(day, y.days - 1))
    }
    // The first day with photos (`way` -1) or the last (1).
    function end(way) {
        const at = Days.edge(plugin.years, way)
        if (at)
            set(at.year, at.day)
    }
    // The cursor's year in view, gliding there.
    function follow() {
        if (!shown || row < 0)
            return
        const from = years.contentY
        years.positionViewAtIndex(row, ListView.Contain)
        const to = years.contentY
        if (Math.abs(to - from) < 1)
            return
        years.contentY = from
        scrolling.to = to
        scrolling.restart()
    }

    // Squares and their room, from what the window has: the whole of a
    // year across it, and at least two and a half years down.
    readonly property real side: Math.max(24, Math.round(width * 0.04))
    readonly property real labelWidth: Math.round(theme.fontSize * 6.5)
    readonly property real weekdayWidth: Math.round(theme.fontSize * 2.8)
    readonly property real headerHeight: Math.round(theme.fontSize * 7)
    readonly property real footerHeight: Math.round(theme.fontSize * 3.6)
    readonly property real monthHeight: Math.round(theme.fontSize * 1.8)
    readonly property real cell: Math.max(8, Math.floor(Math.min(
        (width - 2 * side - labelWidth - weekdayWidth) / 54,
        (height - headerHeight - footerHeight) / (2.5 * 9.6),
        theme.fontSize * 2.8)))
    readonly property real gap: Math.max(2, Math.round(cell * 0.2))
    readonly property real rowHeight: monthHeight + 7 * cell + Math.round(cell * 2.6)
    readonly property real squaresX: Math.round((width - labelWidth - weekdayWidth - 54 * cell) / 2) + labelWidth + weekdayWidth

    // The squares' colours, from the theme, live: none, then lighter to
    // fuller accent, then the glow; and days still to come.
    readonly property color accent: theme.accent
    readonly property var ramp: {
        const a = accent, bg = theme.background, fg = theme.foreground
        const mix = (base, top, amount) => Qt.tint(base, Qt.rgba(top.r, top.g, top.b, amount))
        const empty = mix(bg, fg, theme.light ? 0.08 : 0.07)
        const list = [String(empty)]
        for (let level = 1; level <= 7; ++level)
            list.push(String(mix(empty, a, 0.16 + 0.84 * Math.pow((level - 1) / 6, 1.3))))
        list.push(String(mix(a, theme.brightForeground, 0.55)))
        list.push(String(mix(bg, fg, 0.03)))
        return list
    }
    function level(n) {
        if (n <= 0)
            return 0
        if (n >= plugin.glowing)
            return 8
        return 1 + Math.floor(Math.min(1, Math.log(n) / Math.log(plugin.full)) * 6.99)
    }
    // Days after today are drawn faint, unless they have photos (a camera's
    // clock set wrong). Today again on opening, and as midnight passes.
    property var today: new Date()
    Timer {
        interval: 60000
        repeat: true
        running: calendar.shown
        onTriggered: {
            const now = new Date()
            if (now.toDateString() !== calendar.today.toDateString())
                calendar.today = now
        }
    }

    // Its keys, while it is open (Command.qml).
    Command {
        name: "calendar.close"
        keys: ["Escape", "Y"]
        keysText: "Esc"
        keyLayer: "plugin"
        when: calendar.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: calendar.close()
    }
    Command {
        name: "calendar.show"
        text: qsTr("There: that day in the grid")
        section: "Year calendar"
        keys: ["Return", "Enter", "Space"]
        keysText: "Enter"
        keyLayer: "plugin"
        when: calendar.shown
        mouse: qsTr("Click a day")
        touchpad: qsTr("Tap a day")
        onTriggered: calendar.jump()
    }
    Command {
        name: "calendar.previousDay"
        text: qsTr("A day, a week")
        section: "Year calendar"
        keys: ["Up"]
        keysText: "↑ ↓ ← →"
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendar.move(-1)
    }
    Command {
        name: "calendar.nextDay"
        keys: ["Down"]
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendar.move(1)
    }
    Command {
        name: "calendar.previousWeek"
        keys: ["Left"]
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendar.move(-7)
    }
    Command {
        name: "calendar.nextWeek"
        keys: ["Right"]
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: calendar.move(7)
    }
    Command {
        name: "calendar.previousYear"
        text: qsTr("A year")
        section: "Year calendar"
        keys: ["PgUp"]
        keysText: "PgUp PgDn"
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendar.moveYear(-1)
    }
    Command {
        name: "calendar.nextYear"
        keys: ["PgDown"]
        keyLayer: "plugin"
        when: calendar.shown
        autoRepeat: true
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendar.moveYear(1)
    }
    Command {
        name: "calendar.first"
        text: qsTr("First, last day with photos")
        section: "Year calendar"
        keys: ["Home"]
        keysText: "Home End"
        keyLayer: "plugin"
        when: calendar.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendar.end(-1)
    }
    Command {
        name: "calendar.last"
        keys: ["End"]
        keyLayer: "plugin"
        when: calendar.shown
        mouse: qsTr("Wheel, then point")
        touchpad: qsTr("Two fingers, then point")
        onTriggered: calendar.end(1)
    }

    Rectangle {
        anchors.fill: parent
        color: theme.background
        opacity: 0.97
    }
    // A glow of the accent behind the year opened on, faint.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: calendar.headerHeight - height * 0.35
        width: parent.width * 1.2
        height: parent.height * 0.9
        radius: width / 2
        opacity: 0.06
        gradient: Gradient {
            GradientStop { position: 0; color: calendar.accent }
            GradientStop { position: 0.6; color: "transparent" }
        }
    }
    // Clicks stay here, off the grid beneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => {}
    }

    // The library at a glance: how many photos, on how many days, since when.
    Item {
        id: header
        x: calendar.side
        width: parent.width - 2 * calendar.side
        height: calendar.headerHeight
        Row {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Math.round(theme.fontSize * 1.4)
            spacing: Math.round(theme.fontSize * 0.9)
            UiText {
                id: big
                text: Number(calendar.plugin.total).toLocaleString(Qt.locale(), "f", 0)
                font.pixelSize: Math.round(theme.fontSize * 3)
                font.weight: Font.Bold
                color: theme.brightForeground
            }
            Column {
                anchors.bottom: big.baseline
                anchors.bottomMargin: -Math.round(theme.fontSize * 0.25)
                UiText {
                    text: qsTr("photos", "", calendar.plugin.total) + " " + qsTr("on %Ln days", "", calendar.plugin.dayCount)
                    color: theme.foreground
                }
                UiText {
                    readonly property var list: calendar.plugin.years
                    text: list.length === 0 ? "" : list.length === 1 ? String(list[0].year)
                        : list[0].year + " – " + list[list.length - 1].year
                    color: calendar.accent
                    font.weight: Font.DemiBold
                }
            }
        }
        // Fewer to more, in the squares' own colours.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: closeButton.width + Math.round(theme.fontSize * 1.5)
            anchors.verticalCenter: closeButton.verticalCenter
            spacing: Math.round(calendar.gap * 1.2)
            UiText {
                text: qsTr("Fewer")
                color: theme.muted
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                anchors.verticalCenter: parent.verticalCenter
            }
            Repeater {
                model: [0, 1, 3, 5, 7, 8]
                Rectangle {
                    required property int modelData
                    width: Math.round(theme.fontSize * 0.9)
                    height: width
                    radius: Math.max(2, width * 0.22)
                    color: calendar.ramp[modelData]
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            UiText {
                text: qsTr("More")
                color: theme.muted
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        ShellButton {
            id: closeButton
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 16
            floating: true
            text: "✕"
            hint: "Esc"
            onTapped: calendar.close()
        }
    }

    ListView {
        id: years
        anchors.fill: parent
        anchors.topMargin: calendar.headerHeight
        anchors.bottomMargin: calendar.footerHeight
        clip: true
        model: calendar.plugin.years
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: calendar.rowHeight * 2
        topMargin: Math.round(calendar.cell * 0.5)
        bottomMargin: Math.round(calendar.cell * 0.5)
        NumberAnimation on contentY { id: scrolling; running: false; duration: 220; easing.type: Easing.OutCubic }

        delegate: Item {
            id: yearRow
            required property var modelData
            required property int index
            readonly property bool current: index === calendar.row
            width: years.width
            height: calendar.rowHeight

            // The year, and its photos.
            Column {
                x: calendar.squaresX - calendar.weekdayWidth - calendar.labelWidth
                y: calendar.monthHeight - Math.round(theme.fontSize * 0.3)
                width: calendar.labelWidth - Math.round(theme.fontSize)
                UiText {
                    width: parent.width
                    horizontalAlignment: Text.AlignRight
                    text: yearRow.modelData.year
                    font.pixelSize: Math.round(theme.fontSize * 2.1)
                    font.weight: Font.Bold
                    color: yearRow.current ? theme.brightForeground : theme.foreground
                    opacity: yearRow.current ? 1 : 0.6
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                }
                UiText {
                    width: parent.width
                    horizontalAlignment: Text.AlignRight
                    text: qsTr("%Ln photos", "", yearRow.modelData.total)
                    color: yearRow.current ? calendar.accent : theme.muted
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                }
            }
            // Its months, over the week each begins in.
            Repeater {
                model: 12
                UiText {
                    required property int index
                    readonly property int first: Days.dayOf(Days.dateOf(yearRow.modelData.year, index, 1))
                    x: calendar.squaresX + Math.floor((first + yearRow.modelData.offset) / 7) * calendar.cell
                    y: Math.round((calendar.monthHeight - height) / 2) - Math.round(calendar.gap / 2)
                    text: Qt.locale().standaloneMonthName(index, Locale.ShortFormat)
                    color: yearRow.current && calendar.date.getMonth() === index ? theme.foreground : theme.muted
                    font.pixelSize: Math.round(theme.fontSize * 0.786)
                }
            }
            // Every other weekday, beside its row.
            Repeater {
                model: [1, 3, 5]
                UiText {
                    required property int modelData
                    x: calendar.squaresX - calendar.weekdayWidth
                    y: calendar.monthHeight + modelData * calendar.cell + Math.round((calendar.cell - calendar.gap - height) / 2)
                    text: Qt.locale().dayName((calendar.plugin.firstWeekday + modelData) % 7, Locale.ShortFormat)
                    color: theme.muted
                    font.pixelSize: Math.round(theme.fontSize * 0.714)
                }
            }

            Canvas {
                id: squares
                x: calendar.squaresX
                y: calendar.monthHeight
                width: 54 * calendar.cell
                height: 7 * calendar.cell
                // Painted again when any of these changes.
                readonly property var looks: [calendar.ramp, calendar.cell, calendar.sweep, yearRow.modelData, calendar.today,
                                              calendar.plugin.full, calendar.plugin.glowing]
                onLooksChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const y = yearRow.modelData
                    const c = calendar.cell, size = c - calendar.gap
                    const ramp = calendar.ramp
                    const now = calendar.today
                    const future = y.year > now.getFullYear() ? 0 : y.year < now.getFullYear() ? y.days : Days.dayOf(now) + 1
                    const glowing = []
                    const square = (d, shrink) => {
                        const at = d + y.offset
                        const column = Math.floor(at / 7)
                        const t = calendar.grown(column, yearRow.index)
                        if (t <= 0)
                            return false
                        const s = size * (0.5 + 0.5 * t) * shrink
                        const r = Math.max(1.5, s * 0.24)
                        ctx.globalAlpha = t
                        ctx.beginPath()
                        ctx.roundedRect(column * c + (size - s) / 2, (at % 7) * c + (size - s) / 2, s, s, r, r)
                        ctx.fill()
                        return true
                    }
                    for (let d = 0; d < y.days; ++d) {
                        const l = y.counts[d] > 0 ? calendar.level(y.counts[d]) : d >= future ? 9 : 0
                        if (l === 8) {
                            glowing.push(d)
                            continue
                        }
                        ctx.fillStyle = ramp[l]
                        square(d, 1)
                    }
                    // The busiest days, glowing over the rest.
                    ctx.shadowColor = String(calendar.accent)
                    ctx.shadowBlur = c * 0.9
                    ctx.fillStyle = ramp[8]
                    for (const d of glowing)
                        square(d, 1)
                }
            }
            // The pointer picks the day under it; a click shows it.
            MouseArea {
                anchors.fill: squares
                hoverEnabled: true
                function dayAt(mouse) {
                    const column = Math.floor(mouse.x / calendar.cell), weekday = Math.floor(mouse.y / calendar.cell)
                    const d = column * 7 + Math.max(0, Math.min(6, weekday)) - yearRow.modelData.offset
                    return d >= 0 && d < yearRow.modelData.days ? d : -1
                }
                onPositionChanged: mouse => {
                    const d = dayAt(mouse)
                    if (d >= 0 && (calendar.cursorYear !== yearRow.modelData.year || calendar.day !== d)) {
                        calendar.cursorYear = yearRow.modelData.year
                        calendar.day = d
                    }
                }
                onClicked: mouse => {
                    const d = dayAt(mouse)
                    if (d < 0)
                        return
                    calendar.cursorYear = yearRow.modelData.year
                    calendar.day = d
                    calendar.jump()
                }
            }
        }

        // The cursor and what it says, over the squares, gliding between days.
        Rectangle {
            id: cursor
            parent: years.contentItem
            z: 10
            visible: calendar.row >= 0
            readonly property int at: calendar.year ? calendar.day + calendar.year.offset : 0
            readonly property real pad: Math.max(3, Math.round(calendar.gap * 0.9))
            x: calendar.squaresX + Math.floor(at / 7) * calendar.cell - pad
            y: calendar.row * calendar.rowHeight + calendar.monthHeight + (at % 7) * calendar.cell - pad
            width: calendar.cell - calendar.gap + 2 * pad
            height: width
            radius: Math.max(3, width * 0.26)
            color: "transparent"
            border.width: Math.max(2, Math.round(calendar.cell * 0.09))
            border.color: theme.brightForeground
            Behavior on x { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
            Behavior on y { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
            // A day without photos, asked for: a shake.
            SequentialAnimation {
                id: nudge
                NumberAnimation { target: cursor; property: "rotation"; to: 12; duration: 50 }
                NumberAnimation { target: cursor; property: "rotation"; to: -10; duration: 80 }
                NumberAnimation { target: cursor; property: "rotation"; to: 0; duration: 70 }
            }
        }
        Rectangle {
            id: tip
            parent: years.contentItem
            z: 11
            visible: cursor.visible
            readonly property bool below: (cursor.at % 7) < 3
            readonly property real leftmost: calendar.squaresX
            readonly property real rightmost: calendar.squaresX + 54 * calendar.cell
            x: Math.max(leftmost, Math.min(rightmost - width, cursor.x + cursor.width / 2 - width / 2))
            y: below ? cursor.y + cursor.height + 6 : cursor.y - height - 6
            Behavior on x { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
            Behavior on y { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
            width: tipText.implicitWidth + 2 * Math.round(theme.fontSize * 0.8)
            height: tipText.implicitHeight + 2 * Math.round(theme.fontSize * 0.5)
            radius: theme.radius > 0 ? Math.min(theme.radius + 2, height / 2) : 0
            color: theme.darkerBackground
            border.width: 1
            border.color: calendar.photos > 0 ? calendar.accent : theme.selection
            Column {
                id: tipText
                anchors.centerIn: parent
                UiText {
                    text: calendar.date.toLocaleDateString(Qt.locale(), "dddd, d MMMM yyyy")
                    color: theme.brightForeground
                    font.weight: Font.DemiBold
                }
                UiText {
                    text: calendar.photos > 0 ? qsTr("%Ln photos", "", calendar.photos) : qsTr("No photos")
                    color: calendar.photos > 0 ? calendar.accent : theme.muted
                }
            }
        }
    }

    // Opened before the library is counted.
    Row {
        anchors.centerIn: parent
        visible: calendar.waiting
        spacing: Math.round(theme.fontSize * 0.6)
        Spinner {
            anchors.verticalCenter: parent.verticalCenter
            busy: calendar.waiting
            delay: 0
            bare: true
        }
        UiText {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Counting the days…")
            color: theme.muted
        }
    }

    // Its keys, at its foot.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round((calendar.footerHeight - height) / 2)
        spacing: Math.round(theme.fontSize * 1.6)
        Repeater {
            model: [
                ["↑ ↓ ← →", qsTr("a day, a week")],
                ["PgUp PgDn", qsTr("a year")],
                ["Enter", qsTr("show in the grid")],
                ["Esc", qsTr("close")]
            ]
            Row {
                required property var modelData
                spacing: Math.round(theme.fontSize * 0.5)
                UiText {
                    text: modelData[0]
                    color: theme.foreground
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                    font.weight: Font.DemiBold
                }
                UiText {
                    text: modelData[1]
                    color: theme.muted
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                }
            }
        }
    }
}
