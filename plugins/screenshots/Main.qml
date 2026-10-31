import QtQuick
import Pinhole.Plugin
import "Find.js" as Find

// Screenshots: kept from crowding out the photos. Each day's are one card
// in the grid, or, as the side panel says, none of them is there at all
// (`hidden`), the viewer's ← and → passing over them too, and a button in
// the grid saying how many are away and bringing them back.
//
// A screenshot is known by what the library says of it (Find.js): iCloud
// marks an iPhone's or an iPad's as one; a desktop's is named so (Omarchy's
// screenshot-…, a Mac's Screenshot … or Zrzut ekranu …); and a PNG with no
// place, the size of an iPhone's screen or a monitor's, is one too.
PinholePlugin {
    id: plugin

    // Hidden from the grid, or each day's one card.
    property bool hiding: pinhole.setting("hide", false) === true
    // The screenshots, library positions in order, and each day's, those
    // with two or more.
    property var shots: []
    property var days: []
    property bool finding: false
    property bool findAgain: false
    property bool found: false

    stacks: hiding ? [] : days
    hidden: hiding ? shots : []

    // Kept at once, and a step of the history, to undo with Ctrl+Z.
    function setHiding(hide) {
        if (hide === hiding)
            return
        keepHiding(hide)
        pinhole.did(hide ? qsTr("hiding screenshots") : qsTr("showing screenshots"),
                    () => keepHiding(!hide), () => keepHiding(hide))
    }
    function keepHiding(hide) {
        if (!pinhole.setSetting("hide", hide))
            console.warn("Screenshots: the setting could not be kept")
        hiding = hide
    }

    function find() {
        if (finding) {
            findAgain = true
            return
        }
        if (pinhole.library.count === 0) {
            take([])
            return
        }
        finding = true
        // Which tables there are, then what it can ask of them.
        const library = pinhole.library.dir + "/library.sqlite"
        pinhole.run("sqlite3", ["-readonly", library, Find.tablesQuery], (code, out, error) => {
            if (code !== 0) {
                done(error)
                return
            }
            pinhole.run("sqlite3", ["-readonly", library, Find.query(out.split("\n"))], (code, out, error) => {
                if (code === 0)
                    take(out.split("\n").filter(id => id !== ""))
                done(code === 0 ? "" : error)
            })
        })
    }
    function done(error) {
        finding = false
        if (error !== "")
            console.warn("Screenshots: " + error)
        if (findAgain) {
            findAgain = false
            find()
        }
    }
    // The ids found, as positions, and each day's together.
    function take(ids) {
        const list = ids.map(id => pinhole.library.indexOf(id)).filter(i => i >= 0).sort((a, b) => a - b)
        const byDay = {}
        const order = []
        for (const i of list) {
            const day = new Date(pinhole.library.item(i).date).toDateString()
            if (!byDay[day]) {
                byDay[day] = []
                order.push(day)
            }
            byDay[day].push(i)
        }
        shots = list
        days = order.map(day => byDay[day]).filter(day => day.length >= 2)
        found = true
    }

    // The library changed: look again, once it has settled.
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() { later.restart() }
    }
    Component.onCompleted: find()

    actions: [
        // While they are away, how many, and the way back.
        PluginAction {
            id: hiddenAction
            place: "grid"
            text: qsTr("%n hidden", "", plugin.shots.length)
            key: "Shift+S"
            shown: plugin.hiding && plugin.shots.length > 0
            active: true
            onTriggered: plugin.setHiding(false)
        }
    ]
    // ⇧S hides them again while they are stacked.
    Command {
        name: "screenshots.hide"
        keys: ["Shift+S"]
        keyLayer: "grid"
        when: !plugin.hiding && plugin.shots.length > 0 && !plugin.covering
        mouse: qsTr("The side panel")
        touchpad: qsTr("The side panel")
        onTriggered: plugin.setHiding(true)
    }

    help: [["Shift+S", qsTr("Screenshots: hide them, or show them again, each day's one card")]]

    tourReady: found && shots.length > 0
    tour: [
        {
            title: qsTr("Out of the way"),
            text: qsTr("Screenshots, from an iPhone or a desktop, are one card for each day's, so they leave room for the photos."),
            photo: () => {
                if (plugin.hiding || plugin.days.length === 0)
                    return -1
                const near = pinhole.currentIndex()
                let best = plugin.days[0][0]
                for (const day of plugin.days)
                    if (Math.abs(day[0] - near) < Math.abs(best - near))
                        best = day[0]
                return best
            }
        },
        {
            title: qsTr("Or not there at all"),
            text: qsTr("⇧S hides them from the grid, and the viewer passes over them; a button at the top says how many are away, and ⇧S again brings them back. Ctrl+Z undoes either."),
            action: hiddenAction
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab), they are hidden or stacked, and this tour can be shown again."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: Math.round(theme.fontSize * 0.571)
            Item {
                width: parent.width
                height: Math.max(hideText.implicitHeight, hideSwitch.height)
                UiText {
                    id: hideText
                    anchors.left: parent.left
                    anchors.right: hideSwitch.left
                    anchors.rightMargin: theme.fontSize
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    color: theme.foreground
                    text: qsTr("Hide them from the grid")
                }
                Toggle {
                    id: hideSwitch
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    hint: "⇧S"
                    checked: plugin.hiding
                    onToggled: plugin.setHiding(!checked)
                }
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: !plugin.found ? qsTr("Looking for screenshots…")
                    : plugin.shots.length === 0 ? qsTr("No screenshots here.")
                    : plugin.hiding ? qsTr("%n screenshots hidden.", "", plugin.shots.length)
                    : qsTr("%n screenshots, each day's one card.", "", plugin.shots.length)
            }
        }
    }
}
