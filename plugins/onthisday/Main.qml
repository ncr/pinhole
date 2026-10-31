import QtQuick
import Pinhole.Plugin

// On this day: the photos taken on today's date in earlier years, a year
// at a time (Days.qml), from a button in the grid that shows only when
// there are any. The library is read once into its days of the year, each
// the positions taken on it, whenever it is read again; today's photos,
// a new day's at midnight and the settings' are then only a look-up.
PinholePlugin {
    id: plugin
    covering: days.shown

    // How many days either side of today count: 0 is the day itself, more
    // for the years with few photos.
    property int span: pinhole.setting("span", 0)
    property bool videos: pinhole.setting("videos", true)
    property bool screenshots: pinhole.setting("screenshots", false)
    function set(name, value) {
        plugin[name] = value
        pinhole.setSetting(name, value)
        recount()
    }

    // The library's positions by the day of the year they were taken on,
    // in local time: month * 32 + day.
    property var byDay: ({})
    property bool indexed: false
    // Today, as the years were last counted for it.
    property var today: new Date()
    // The earlier years with photos on this day, the latest first:
    // [{ year, ago, positions }], each year's in the order they were taken.
    property var years: []

    function dayOf(date) { return date.getMonth() * 32 + date.getDate() }
    // The days are read a few thousand photos a frame, the window kept
    // moving: a whole library at once takes a third of a second. The last
    // ones stay until the new ones are whole.
    property var reading: null
    function index() {
        reading = { map: {}, next: 0 }
        indexer.start()
    }
    function indexSome() {
        const library = pinhole.library
        const map = reading.map
        const end = Math.min(library.count, reading.next + 2500)
        for (let i = reading.next; i < end; ++i) {
            const key = dayOf(new Date(library.item(i).date))
            const list = map[key]
            if (list)
                list.push(i)
            else
                map[key] = [i]
        }
        reading.next = end
        if (end < library.count)
            return
        indexer.stop()
        byDay = map
        reading = null
        indexed = true
        recount()
    }
    Timer { id: indexer; interval: 1; repeat: true; onTriggered: plugin.indexSome() }
    // A screenshot: an iPhone's are PNGs; elsewhere they say so by name.
    function isScreenshot(item) {
        return /\.png$/i.test(item.filename) || /^(screenshot|screen shot|zrzut)/i.test(item.filename)
    }
    function recount() {
        if (!indexed)
            return
        const now = new Date()
        today = now
        const thisYear = now.getFullYear()
        const found = {}
        for (let d = -span; d <= span; ++d) {
            const day = new Date(thisYear, now.getMonth(), now.getDate() + d)
            // A day of next year or the last (around New Year) is this
            // year's in the year before or after it.
            const shift = day.getFullYear() - thisYear
            const keys = [dayOf(day)]
            // The 29th of February is the 28th's in the other years.
            if (day.getMonth() === 1 && day.getDate() === 28 && new Date(day.getFullYear(), 1, 29).getMonth() === 2)
                keys.push(1 * 32 + 29)
            for (const key of keys) {
                for (const i of byDay[key] || []) {
                    const item = pinhole.library.item(i)
                    if ((item.video && !videos) || (!screenshots && isScreenshot(item)))
                        continue
                    const year = new Date(item.date).getFullYear() - shift
                    if (year >= thisYear)
                        continue
                    if (!found[year])
                        found[year] = []
                    found[year].push({ i: i, date: item.date })
                }
            }
        }
        years = Object.keys(found).map(Number).sort((a, b) => b - a).map(year => ({
            year: year,
            ago: thisYear - year,
            positions: found[year].sort((a, b) => a.date - b.date).map(p => p.i)
        }))
    }

    // Read again: the positions moved, so the days are read again too.
    Connections {
        target: pinhole.library
        function onChanged() { plugin.index() }
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

    // A real year of one's own: the latest with photos on this day.
    tourReady: indexed && years.length > 0
    tour: [
        {
            title: qsTr("Years ago today"),
            text: years.length > 0
                  ? qsTr("Taken %n years ago today, in %1.", "", years[0].ago).arg(years[0].year)
                  : "",
            photo: () => years.length > 0 ? years[0].positions[0] : -1
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
