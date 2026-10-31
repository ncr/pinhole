import QtQuick
import Pinhole.Plugin

// Photo frame: photos one after another, full window, each slowly panned
// and zoomed and cross-faded into the next, for a laptop left open. P on a
// photo starts it from there, with the photos its settings choose
// (favourites, all, or those taken on this day in other years); P in select
// mode shows what is ticked. Each comes as its thumbnail and is shown only
// once its medium rendition is here, loaded under the one showing, so no
// slide is ever blurry while it is up.
PinholePlugin {
    id: plugin
    covering: frame.shown

    // Its settings, kept as they change.
    readonly property string photos: pinhole.settings.photos ?? "favourites"
    readonly property int seconds: pinhole.settings.seconds ?? 8
    readonly property bool shuffle: pinhole.settings.shuffle ?? true
    readonly property bool caption: pinhole.settings.caption ?? true
    readonly property bool motion: pinhole.settings.motion ?? true
    function keep(name, value) { pinhole.settings[name] = value }
    readonly property var secondsChoices: [5, 8, 12, 20, 30]

    // The library positions its settings choose, oldest first: looked
    // for a few milliseconds a frame, so the window never waits on it (a
    // pass over 50,000 photos takes about half a second), and given to
    // `done` once all are seen.
    property var scan: null
    function choose(kind, done) {
        const n = pinhole.library.count
        if (kind === "all") {
            scan = null
            done(Array.from({ length: n }, (_, i) => i))
            return
        }
        // On this day: its month and day in the years before; failing any,
        // the week around it.
        const now = new Date()
        const dayOf = date => date.getMonth() * 32 + date.getDate()
        const near = new Set()
        for (let d = -3; d <= 3; ++d)
            if (d !== 0)
                near.add(dayOf(new Date(now.getFullYear(), now.getMonth(), now.getDate() + d)))
        scan = { kind: kind, done: done, i: 0, n: n, list: [], week: [], year: now.getFullYear(), day: dayOf(now), near: near }
    }
    FrameAnimation {
        running: plugin.scan !== null
        onTriggered: {
            const s = plugin.scan
            const library = plugin.pinhole.library
            const until = Date.now() + 3
            while (s.i < s.n && (s.i % 64 !== 0 || Date.now() < until)) {
                const item = library.item(s.i)
                if (s.kind === "favourites") {
                    if (item.favorite)
                        s.list.push(s.i)
                } else {
                    const date = new Date(item.date)
                    if (date.getFullYear() < s.year) {
                        const day = date.getMonth() * 32 + date.getDate()
                        if (day === s.day)
                            s.list.push(s.i)
                        else if (s.near.has(day))
                            s.week.push(s.i)
                    }
                }
                ++s.i
            }
            if (s.i < s.n)
                return
            plugin.scan = null
            s.done(s.list.length > 0 || s.kind !== "onthisday" ? s.list : s.week)
        }
    }
    // `list` as it will be shown, beginning at `first` (one of it, or not):
    // shuffled, or on through time from there, round to the start.
    function arrange(list, first) {
        if (list.length === 0)
            return first >= 0 ? [first] : []
        if (shuffle) {
            const rest = list.filter(i => i !== first)
            for (let k = rest.length - 1; k > 0; --k) {
                const j = Math.floor(Math.random() * (k + 1))
                const t = rest[k]
                rest[k] = rest[j]
                rest[j] = t
            }
            return first >= 0 ? [first].concat(rest) : rest
        }
        if (first < 0)
            return list
        let at = list.findIndex(i => i >= first)
        if (at < 0)
            at = 0
        const round = list.slice(at).concat(list.slice(0, at))
        return round[0] === first ? round : [first].concat(round)
    }

    // A word on the photos when nothing could be shown.
    function say(text) {
        notice.text = text
        noticeTimer.restart()
    }

    actions: [
        PluginAction {
            id: viewerAction
            place: "viewer"
            text: qsTr("Frame")
            key: "P"
            onTriggered: {
                // The photo shown at once, and the rest as they are found.
                const from = pinhole.viewer.index
                frame.start([from], true)
                plugin.choose(plugin.photos, list => {
                    if (!frame.shown || !frame.fromViewer)
                        return
                    if (list.length === 0) {
                        frame.stop()
                        plugin.say(plugin.photos === "onthisday" ? qsTr("Nothing taken on this day in other years")
                                   : qsTr("No favourites to show"))
                        return
                    }
                    frame.extend(plugin.arrange(list, frame.current.index))
                })
            }
        },
        PluginAction {
            place: "selection"
            text: qsTr("Frame")
            key: "P"
            usable: pinhole.selectionCount > 0
            onTriggered: {
                const ticked = Array.from(pinhole.selection())
                frame.start(plugin.arrange(ticked, plugin.shuffle ? -1 : ticked[0]), false)
            }
        }
    ]

    help: [
        ["P", qsTr("Photo frame: photos one after another, from this one")],
        ["P", qsTr("In select mode: a photo frame of what is ticked")]
    ]

    tourReady: pinhole.library.count > 0
    tour: [
        {
            title: qsTr("A photo frame"),
            text: qsTr("P on a photo shows your photos from there, one after another and slowly moving, full window: for a laptop left open. In select mode, P shows what is ticked."),
            open: () => pinhole.currentIndex(),
            action: viewerAction
        },
        {
            title: qsTr("While it shows"),
            text: qsTr("Esc or a click stops it, ← → step through, and a two-finger tap or Space holds it still. Move the pointer for its buttons.")
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab): which photos, how long each, shuffled or in order, the date on each, and whether videos and Live Photos move."),
            settings: true
        }
    ]

    settings: Component {
        Column {
            id: form
            width: parent ? parent.width : 0
            spacing: 4
            readonly property int small: Math.round(theme.fontSize * 0.857)
            component Label: UiText {
                width: form.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                font.pixelSize: form.small
            }
            component Switch: Item {
                id: row
                property string text
                property bool checked
                signal toggled()
                width: form.width
                height: toggle.height
                UiText {
                    width: parent.width - toggle.width - theme.fontSize
                    y: Math.round((parent.height - implicitHeight) / 2)
                    text: row.text
                    color: theme.brightForeground
                    elide: Text.ElideRight
                }
                Toggle {
                    id: toggle
                    anchors.right: parent.right
                    checked: row.checked
                    onToggled: row.toggled()
                }
            }

            Label { text: qsTr("Photos") }
            Flow {
                width: form.width
                Repeater {
                    model: [["favourites", qsTr("Favourites")], ["all", qsTr("All")], ["onthisday", qsTr("On this day")]]
                    delegate: ShellButton {
                        required property var modelData
                        flat: true
                        text: modelData[1]
                        active: plugin.photos === modelData[0]
                        onTapped: plugin.keep("photos", modelData[0])
                    }
                }
            }
            Label { text: qsTr("Seconds each") }
            Flow {
                width: form.width
                Repeater {
                    model: plugin.secondsChoices
                    delegate: ShellButton {
                        required property int modelData
                        flat: true
                        text: Number(modelData).toLocaleString(Qt.locale(), "f", 0)
                        active: plugin.seconds === modelData
                        onTapped: plugin.keep("seconds", modelData)
                    }
                }
            }
            Switch {
                text: qsTr("Shuffle")
                checked: plugin.shuffle
                onToggled: plugin.keep("shuffle", !plugin.shuffle)
            }
            Switch {
                text: qsTr("Date on each photo")
                checked: plugin.caption
                onToggled: plugin.keep("caption", !plugin.caption)
            }
            Switch {
                text: qsTr("Videos and Live Photos move")
                checked: plugin.motion
                onToggled: plugin.keep("motion", !plugin.motion)
            }
            Label {
                topPadding: 4
                text: qsTr("P on a photo starts it from there; in select mode, P shows what is ticked.")
            }
        }
    }

    Frame {
        id: frame
        anchors.fill: parent
        plugin: plugin
    }

    Chip {
        id: notice
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 96
        visible: opacity > 0
        opacity: noticeTimer.running ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
    }
    Timer { id: noticeTimer; interval: 2500 }
}
