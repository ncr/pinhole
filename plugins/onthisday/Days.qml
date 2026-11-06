import QtQuick
import QtQuick.Window
import Pinhole.Plugin

// This day in every earlier year with photos of it, the latest first, over
// the whole window. A photo opens full window, and closing it comes back
// here; "In the grid" shows a year's where it is among the rest. Photos
// are library positions: the years are counted again, at once, whenever
// the library is read again while they show, and on opening.
Item {
    id: days
    // Its PluginView (Main.qml), and the plugin through it.
    required property var view
    readonly property var plugin: view.plugin
    readonly property bool shown: view.shown

    // A photo opened from here shows: closing it comes back (onReturned).
    readonly property bool away: pinhole.away

    readonly property var years: plugin.years
    // Where each year's photos start among them all, in the order shown,
    // and how many there are; the keys' cursor is one of them.
    readonly property var starts: {
        const list = []
        let n = 0
        years.forEach(year => { list.push(n); n += year.photos.length })
        list.push(n)
        return list
    }
    readonly property int total: starts[starts.length - 1]
    property int cursor: 0
    // Where one of them all is, among `among` (the years shown if not
    // given): which year, and which of its photos. Counted from the years
    // themselves: as they change, `starts` may not have caught up yet.
    function placeOf(at, among) {
        const of = among || years
        let s = 0
        while (s + 1 < of.length && at >= of[s].photos.length) {
            at -= of[s].photos.length
            ++s
        }
        return { s: s, k: at }
    }
    // Its photo (a library position), -1 for none.
    function photoAt(at) {
        const place = placeOf(at)
        const year = years[place.s]
        return year && place.k >= 0 && place.k < year.photos.length ? year.photos[place.k] : -1
    }
    // The line a photo is on, for these years laid out so many across.
    function rowOf(at, among, across) {
        const of = among || years, n = across || columns
        const place = placeOf(at, of)
        let row = 0
        for (let s = 0; s < place.s; ++s)
            row += 1 + Math.ceil(of[s].photos.length / n)
        return row + 1 + Math.floor(place.k / n)
    }

    // The lines of the list, only while it shows (or comes back): each
    // year's heading, then its photos a row at a time. Only those in
    // sight are made. The list's model is how many lines there are, each
    // line taking its own from `rows`: new years of as many lines leave
    // the list as it is.
    property var rows: []
    property int lineCount: 0
    // The years and the columns the lines are laid out for.
    property var laidYears: []
    property int laidColumns: 1
    // The cursor's line as laid out, and how far down the list's view it
    // is; null when it is not in sight.
    function cursorLine() {
        if (lineCount === 0 || laidYears.length === 0)
            return null
        const line = rowOf(cursor, laidYears, laidColumns)
        const item = list.itemAtIndex(line)
        return item ? { line: line, offset: item.y - list.contentY } : null
    }
    function scrollTo(y) {
        list.contentY = Math.max(list.originY, Math.min(y, list.originY + list.contentHeight - list.height))
    }
    // Laid out again for the years and columns as they are now. More or
    // fewer lines keep where the list was scrolled to; with `keep` (from
    // cursorLine() before the change), the cursor's line stays where it
    // was in the view, however many lines came or went above it.
    function relayout(keep) {
        const lines = []
        if (shown || away)
            years.forEach((year, s) => {
                lines.push({ s: s, k: -1 })
                for (let k = 0; k < year.photos.length; k += columns)
                    lines.push({ s: s, k: k })
            })
        laidYears = years
        laidColumns = columns
        const same = lines.length === rows.length && lines.every((line, i) => line.s === rows[i].s && line.k === rows[i].k)
        if (!same)
            rows = lines
        if (lines.length !== lineCount) {
            const y = list.contentY
            lineCount = lines.length
            list.forceLayout()
            scrollTo(y)
        }
        if (keep && years.length > 0) {
            const line = rowOf(cursor)
            if (!same || line !== keep.line) {
                list.forceLayout()
                list.positionViewAtIndex(line, ListView.Beginning)
                const item = list.itemAtIndex(line)
                if (item)
                    scrollTo(item.y - keep.offset)
            }
        }
    }
    onShownChanged: relayout(null)
    onAwayChanged: relayout(null)
    onColumnsChanged: relayout(cursorLine())

    // Opened (its view's opened()).
    function start() {
        cursor = 0
        list.positionViewAtBeginning()
    }
    // ← → through these years' photos, as they are laid out here.
    function openPhoto(at) {
        if (at < 0 || at >= total)
            return
        cursor = at
        pinhole.openPhotos(years.reduce((all, year) => all.concat(year.photos), []), photoAt(at), { comeBack: true })
    }
    function reveal(i) {
        view.close()
        pinhole.reveal(i)
    }
    Connections {
        target: pinhole
        // Back on the photo last looked at, its lines kept meanwhile.
        function onReturned(photo) {
            days.plugin.fresh()
            const position = pinhole.library.indexOf(photo)
            const all = days.years.reduce((list, year) => list.concat(year.photos), [])
            const at = all.indexOf(position)
            if (at >= 0) days.cursor = at
            days.keepInView()
        }
    }
    // The years changed under it (the library read again, a setting,
    // photos hidden, or midnight): the cursor stays on its photo, by its
    // id, if it is still among them, and its line where it was on the
    // screen.
    property string cursorId: ""
    function idAt(at) {
        const photo = photoAt(at)
        return photo >= 0 ? pinhole.library.item(photo).id ?? "" : ""
    }
    onCursorChanged: cursorId = idAt(cursor)
    onYearsChanged: {
        const keep = cursorLine()
        const at = cursorId !== "" ? pinhole.library.indexOf(cursorId) : -1
        let k = -1, n = 0
        for (const year of years) {
            const i = at >= 0 && k < 0 ? year.photos.indexOf(at) : -1
            if (i >= 0)
                k = n + i
            n += year.photos.length
        }
        cursor = k >= 0 ? k : Math.min(cursor, Math.max(0, n - 1))
        cursorId = idAt(cursor)
        relayout(keep)
    }

    // The keys: along a year's rows, and from one year to the next.
    function move(dx, dy) {
        if (total === 0)
            return
        if (dx !== 0) {
            cursor = Math.max(0, Math.min(total - 1, cursor + dx))
        } else {
            const at = placeOf(cursor)
            const count = years[at.s].photos.length
            const column = at.k % columns
            let k = at.k + dy * columns
            let s = at.s
            if (k < 0) {
                if (s === 0)
                    return
                s -= 1
                const n = years[s].photos.length
                k = Math.min(n - 1, Math.floor((n - 1) / columns) * columns + column)
            } else if (k >= count) {
                if (Math.floor(at.k / columns) < Math.floor((count - 1) / columns)) {
                    k = count - 1
                } else {
                    if (s === years.length - 1)
                        return
                    s += 1
                    k = Math.min(years[s].photos.length - 1, column)
                }
            }
            cursor = starts[s] + k
        }
        keepInView()
    }
    function keepInView() {
        if (total === 0)
            return
        const row = rowOf(cursor)
        // A year's first row with its heading.
        if (placeOf(cursor).k < columns)
            list.positionViewAtIndex(row - 1, ListView.Contain)
        list.positionViewAtIndex(row, ListView.Contain)
    }

    Repeater {
        model: [["left", "Left", -1, 0], ["right", "Right", 1, 0], ["up", "Up", 0, -1], ["down", "Down", 0, 1]]
        delegate: Command {
            required property var modelData
            required property int index
            name: "onthisday." + modelData[0]
            text: qsTr("Choose a photo")
            section: index === 0 ? qsTr("On this day") : ""
            keys: [modelData[1]]
            keysText: "← → ↑ ↓"
            keyLayer: "plugin"
            when: days.shown
            autoRepeat: true
            mouse: qsTr("Point at it")
            touchpad: qsTr("Point at it")
            pointerHelp: false
            onTriggered: days.move(modelData[2], modelData[3])
        }
    }
    Command {
        name: "onthisday.open"
        text: qsTr("Open the photo")
        section: qsTr("On this day")
        keys: ["Return", "Enter"]
        keysText: "⏎"
        keyLayer: "plugin"
        when: days.shown && days.total > 0
        mouse: qsTr("Click it")
        touchpad: qsTr("Tap it")
        onTriggered: days.openPhoto(days.cursor)
    }
    Command {
        name: "onthisday.reveal"
        text: qsTr("Show the photo in the grid")
        section: qsTr("On this day")
        keys: ["Shift+Return", "Shift+Enter"]
        keysText: "⇧ ⏎"
        keyLayer: "plugin"
        when: days.shown && days.total > 0
        mouse: qsTr("In the grid, beside its year")
        touchpad: qsTr("In the grid, beside its year")
        onTriggered: days.reveal(days.photoAt(days.cursor))
    }

    // Nothing over it reaches the photos beneath.
    Panel {}

    Row {
        id: top
        x: 16
        y: 16
        spacing: 6
        Chip {
            text: qsTr("On this day") + " <font color=\"" + theme.darkForeground + "\">· </font>"
                  + days.plugin.today.toLocaleDateString(Qt.locale(), qsTr("d MMMM"))
                  + (days.plugin.span > 0 ? " <font color=\"" + theme.darkForeground + "\">· ± "
                     + qsTr("%n days", "", days.plugin.span) + "</font>" : "")
            textColor: theme.brightForeground
        }
    }
    ShellButton {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        action: days.view.closeCommand
    }

    // Its years emptied while it showed: a setting, or midnight.
    UiText {
        anchors.centerIn: parent
        visible: days.total === 0
        text: qsTr("Nothing from this day in earlier years.")
        color: theme.darkForeground
    }

    // The cells: as many across as fit at about a thumbnail's size.
    readonly property real gap: 4
    readonly property int columns: Math.max(3, Math.floor((list.width + gap) / (theme.fontSize * 13 + gap)))
    readonly property real cell: Math.floor((list.width - (columns - 1) * gap) / columns)

    ScrollList {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: top.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 10
        model: days.lineCount
        cacheBuffer: Math.max(0, height)
        footer: Item { height: 16 }
        delegate: Item {
            id: line
            required property int index
            readonly property var place: days.rows[index] ?? { s: 0, k: -1 }
            readonly property var year: days.years[place.s]
            readonly property bool heading: place.k < 0
            readonly property real above: heading && place.s > 0 ? Math.round(theme.fontSize * 1.4) : 0
            width: list.width
            height: heading ? above + inGrid.height + 8 : days.cell + days.gap

            UiText {
                id: headingText
                visible: line.heading
                y: line.above + centredIn(inGrid.height)
                textFormat: Text.StyledText
                text: !line.heading || !line.year ? ""
                    : qsTr("%n years ago", "", line.year.ago)
                      + " <font color=\"" + theme.darkForeground + "\">· " + line.year.label
                      + " · " + qsTr("%n photos", "", line.year.photos.length) + "</font>"
                color: theme.brightForeground
            }
            ShellButton {
                id: inGrid
                visible: line.heading
                anchors.right: parent.right
                y: line.above
                flat: true
                text: qsTr("In the grid")
                onTapped: days.reveal(line.year.photos[0])
            }

            Row {
                visible: !line.heading
                spacing: days.gap
                Repeater {
                    model: line.heading || !line.year ? [] : line.year.photos.slice(line.place.k, line.place.k + days.columns)
                    delegate: Item {
                        id: tile
                        required property int modelData
                        required property int index
                        readonly property int at: days.starts[line.place.s] + line.place.k + index
                        width: days.cell
                        height: days.cell
                        Rectangle {
                            anchors.fill: parent
                            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.06)
                        }
                        Image {
                            anchors.fill: parent
                            source: pinhole.library.item(tile.modelData).thumbnail ?? ""
                            sourceSize.width: days.cell * Screen.devicePixelRatio
                            sourceSize.height: days.cell * Screen.devicePixelRatio
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            opacity: status === Image.Ready ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }
                        Chip {
                            visible: days.plugin.isVideo[tile.modelData] === true
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            text: "▶"
                        }
                        // The one the keys are on, and the one under the
                        // pointer, as PhotoTiles rings them.
                        Rectangle {
                            readonly property bool chosen: pinhole.keyboardInUse && days.cursor === tile.at
                            anchors.fill: parent
                            color: "transparent"
                            border.width: chosen ? 2 : 1
                            border.color: theme.accent
                            visible: chosen || hover.hovered
                        }
                        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: days.openPhoto(tile.at) }
                    }
                }
            }
        }
    }
}
