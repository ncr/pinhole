import QtQuick
import Pinhole.Plugin

// Screenshots: kept from crowding out the photos. Each day's are one card
// in the grid, or, as the side panel says, none of them is there at all
// (`hidden`), the viewer's ← and → passing over them too, and a button in
// the grid saying how many are away and bringing them back.
//
// A screenshot is known by what the library says of it (pinhole.screenshots): iCloud
// marks an iPhone's or an iPad's as one; one named so is one (Omarchy's
// screenshot-…, a Mac's Screenshot … or Zrzut ekranu …); and so is a PNG
// with no place shaped like a phone's screen. What it found last is kept
// for each source's library in its cache, and used at once when Pinhole
// starts, so the screenshots do not show for a moment and then go.
PinholePlugin {
    id: plugin

    // Hidden from the grid, or each day's one card (false until chosen:
    // the manifest's "settings").
    readonly property bool hiding: pinhole.settings.hide === true
    // The screenshots' ids, as last found; their library positions in
    // order, and each day's, those with two or more.
    property var ids: []
    property var shots: []
    property var days: []
    readonly property bool finding: finder.busy
    // Looked for in this library since it was shown (not only kept from
    // before).
    property bool found: false

    stacks: hiding ? [] : days
    hidden: hiding ? shots : []

    // Kept at once, and a step of the history, to undo with Ctrl Z.
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
        // Never quietly: how many went, and the way back.
        if (hide && shots.length > 0)
            pinhole.notify(qsTr("%n screenshots hidden: ⇧\u00a0S shows them again", "", shots.length))
    }

    // What it found, kept for each source's library in its cache (it can
    // be found again; it is not a setting): read at once as it loads, which
    // is before the grid's first frame, so the screenshots never show for a
    // moment and go. Not for a library that does not say whose it is (its
    // key ""), which another such would share; nor more than `keepAtMost`,
    // which would be long to read before that frame: those show for a
    // moment as the window opens.
    readonly property int keepAtMost: 5000
    // The library shown: its key, or its folder; and where what was found
    // in it is kept, "" for nowhere.
    property string shown: ""
    property string keptKey: ""
    // Another library than the last: what was kept for it, and true.
    function loadKept() {
        const key = pinhole.library.key
        const library = key !== "" ? key : "folder " + pinhole.library.dir
        if (library === shown)
            return false
        shown = library
        keptKey = key !== "" ? "found:" + key : ""
        found = false
        take(keptKey !== "" ? pinhole.cache[keptKey] || [] : [])
        return true
    }
    // Kept (written only if it changed), and what was found of sources no
    // longer added gone.
    function keep(key, list) {
        pinhole.cache[key] = list.length <= keepAtMost ? list : null
        const sources = pinhole.library.keys()
        for (const other of pinhole.cache.keys())
            if (other.startsWith("found:") && other !== key && sources.indexOf(other.slice(6)) < 0
                    && pinhole.cache[other] !== undefined)
                pinhole.cache[other] = null
    }

    // Asked while it looks: once more after, in what is shown then.
    function find() {
        finder.start(() => {
            const key = keptKey, showing = shown, dir = pinhole.library.dir
            if (pinhole.library.count === 0)
                return { ids: [], key: "", showing: showing, dir: dir }
            // Read only, away from the window; while the service writes the
            // index (as a round starts), waited for rather than failed.
            return pinhole.screenshots().then(ids => ({ ids: ids, key: key, showing: showing, dir: dir }))
        })
    }
    Job {
        id: finder
        coalesce: true
        onDone: result => {
            // Another library shown meanwhile: these are the last one's,
            // neither kept as this one's nor shown; it is looked at next.
            if (result.showing !== plugin.shown || result.dir !== pinhole.library.dir) {
                plugin.find()
                return
            }
            if (result.key !== "")
                plugin.keep(result.key, result.ids)
            plugin.take(result.ids)
            plugin.found = true
        }
        onFailed: error => pinhole.warn(error)
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
    // once (another source's: what was kept for it), then a look for new
    // ones once it has settled.
    Timer { id: later; interval: 1500; onTriggered: plugin.find() }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (!plugin.loadKept())
                plugin.take(plugin.ids)
            later.restart()
        }
    }
    Component.onCompleted: {
        loadKept()
        find()
    }

    actions: [
        // While they are away, how many, and the way back.
        PluginAction {
            id: hiddenAction
            place: "grid"
            text: qsTr("%n screenshots hidden", "", plugin.shots.length)
            key: "Shift+S"
            helpText: qsTr("Hide or show them")
            shown: plugin.hiding && plugin.shots.length > 0
            active: true
            onTriggered: plugin.setHiding(false)
        }
    ]
    // ⇧ S hides them again while they are stacked.
    Command {
        name: "screenshots.hide"
        keys: ["Shift+S"]
        keyLayer: "grid"
        when: !plugin.hiding && plugin.shots.length > 0 && !plugin.covering
        mouse: qsTr("The side panel")
        touchpad: qsTr("The side panel")
        onTriggered: plugin.setHiding(true)
    }

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
            text: qsTr("⇧\u00a0S or the button shows them again, each day's one card; ⇧\u00a0S hides them again. Ctrl\u00a0Z undoes either. Here, in the side panel (Tab), too, and this tour can be shown again."),
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
            text: qsTr("⇧\u00a0S hides them from the grid, and the viewer passes over them; a button at the top then says how many are away, and brings them back. Ctrl\u00a0Z undoes either. Here, in the side panel (Tab), too, and this tour can be shown again."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: Math.round(theme.fontSize * 0.571)
            SettingSwitch {
                text: qsTr("Hide them from the grid")
                hint: "⇧ S"
                checked: plugin.hiding
                onToggled: plugin.setHiding(!checked)
            }
            SettingNote {
                text: plugin.shots.length === 0 ? (plugin.found ? qsTr("No screenshots here.") : qsTr("Looking for screenshots…"))
                    : plugin.hiding ? qsTr("%n screenshots hidden", "", plugin.shots.length)
                    : qsTr("%n screenshots, each day's one card.", "", plugin.shots.length)
            }
        }
    }
}
