import QtQuick
import Pinhole.Plugin
import "Find.js" as Find

// Screenshots: kept from crowding out the photos. Each day's are one card
// in the grid, or, as the side panel says, none of them is there at all
// (`hidden`), the viewer's ← and → passing over them too, and a button in
// the grid saying how many are away and bringing them back.
//
// A screenshot is known by what the library says of it (Find.js): iCloud
// marks an iPhone's or an iPad's as one; one named so is one (Omarchy's
// screenshot-…, a Mac's Screenshot … or Zrzut ekranu …); and so is a PNG
// with no place shaped like a phone's screen. What it found last is kept
// for each source's library, and used at once when Pinhole starts, so the
// screenshots do not show for a moment and then go.
PinholePlugin {
    id: plugin

    // Hidden from the grid, or each day's one card.
    property bool hiding: pinhole.setting("hide", false) === true
    // The screenshots' ids, as last found; their library positions in
    // order, and each day's, those with two or more.
    property var ids: []
    property var shots: []
    property var days: []
    property bool finding: false
    property bool findAgain: false
    // Looked for in this library since it was shown (not only kept from
    // before).
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
            pinhole.warn(qsTr("The setting could not be kept"))
        hiding = hide
        // Never quietly: how many went, and the way back.
        if (hide && shots.length > 0)
            pinhole.notify(qsTr("%n screenshots hidden: ⇧S shows them again", "", shots.length))
    }

    // Where what it found is kept, for this source's library.
    readonly property string keptFile: pinhole.cacheDir + "/screenshots"
    function loadKept() {
        found = false
        const file = keptFile
        pinhole.run("cat", [file]).then(result => {
            // Found afresh meanwhile, or another library shown since.
            if (!found && file === keptFile)
                take(result.stdout.split("\n").filter(id => id !== ""))
        }, () => {}) // none kept yet
        find()
    }
    function keep(list) {
        if (list.join("\n") === ids.join("\n"))
            return
        pinhole.run("sh", ["-c", "mkdir -p \"${1%/*}\" && cat > \"$1\"", "sh", keptFile], { input: list.join("\n") + "\n" })
            .catch(pinhole.warn)
    }

    function find() {
        if (finding) {
            findAgain = true
            return
        }
        if (pinhole.library.count === 0) {
            take([])
            found = true
            return
        }
        finding = true
        // Which tables there are, then what it can ask of them.
        const library = pinhole.library.dir + "/library.sqlite"
        pinhole.run("sqlite3", ["-readonly", library, Find.tablesQuery])
            .then(result => pinhole.run("sqlite3", ["-readonly", library, Find.query(result.stdout.split("\n"))]))
            .then(result => {
                const list = result.stdout.split("\n").filter(id => id !== "")
                keep(list)
                take(list)
                found = true
            })
            .catch(pinhole.warn)
            .then(() => {
                finding = false
                if (findAgain) {
                    findAgain = false
                    find()
                }
            })
    }
    // The ids, as positions, and each day's together.
    function take(list) {
        ids = list
        const positions = list.map(id => pinhole.library.indexOf(id)).filter(i => i >= 0).sort((a, b) => a - b)
        const byDay = {}
        const order = []
        for (const i of positions) {
            const day = new Date(pinhole.library.item(i).date).toDateString()
            if (!byDay[day]) {
                byDay[day] = []
                order.push(day)
            }
            byDay[day].push(i)
        }
        shots = positions
        days = order.map(day => byDay[day]).filter(day => day.length >= 2)
    }

    // The library read again: the same screenshots where they are now at
    // once, then a look for new ones once it has settled. Another source's
    // library: what was kept for it.
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() {
            plugin.take(plugin.ids)
            later.restart()
        }
    }
    Connections {
        target: pinhole
        function onCacheDirChanged() { plugin.loadKept() }
    }
    Component.onCompleted: loadKept()

    actions: [
        // While they are away, how many, and the way back.
        PluginAction {
            id: hiddenAction
            place: "grid"
            text: qsTr("%n screenshots hidden", "", plugin.shots.length)
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

    tourReady: shots.length > 0
    // As it is now: each day's one card, or hidden.
    tour: hiding ? [
        {
            title: qsTr("Out of the way"),
            text: qsTr("Screenshots, from an iPhone or a desktop, are hidden from the grid, and the viewer passes over them, so they leave room for the photos. This button says how many are away."),
            action: hiddenAction
        },
        {
            title: qsTr("Or each day's one card"),
            text: qsTr("⇧S or the button shows them again, each day's one card; ⇧S hides them again. Ctrl+Z undoes either. Here, in the side panel (Tab), too, and this tour can be shown again."),
            settings: true
        }
    ] : [
        {
            title: qsTr("Out of the way"),
            text: qsTr("Screenshots, from an iPhone or a desktop, are one card for each day's, so they leave room for the photos."),
            photo: () => {
                if (plugin.days.length === 0)
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
            text: qsTr("⇧S hides them from the grid, and the viewer passes over them; a button at the top then says how many are away, and brings them back. Ctrl+Z undoes either. Here, in the side panel (Tab), too, and this tour can be shown again."),
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
                text: plugin.shots.length === 0 ? (plugin.found ? qsTr("No screenshots here.") : qsTr("Looking for screenshots…"))
                    : plugin.hiding ? qsTr("%n screenshots hidden", "", plugin.shots.length)
                    : qsTr("%n screenshots, each day's one card.", "", plugin.shots.length)
            }
        }
    }
}
