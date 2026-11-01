import QtQuick
import Pinhole.Plugin

// Photo frame: photos one after another, full window, each slowly panned
// and zoomed and cross-faded into the next, for a laptop left open. P on a
// photo starts it from there, and its button in the grid (P there too)
// with the photos its settings choose: favourites, all, or those taken on
// this day in other years. P in select mode shows what is ticked. Each
// comes as its thumbnail and is shown only once its medium rendition is
// here, loaded under the one showing, so no slide is blurry while it is
// up; the full size follows where the screen has the pixels for it. While
// it shows, the screen stays on.
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

    // The photos its settings choose, by id, oldest first, none hidden
    // from the grid (Screenshots' screenshots): a few milliseconds over the
    // whole library. On this day: its month and day in the years before;
    // failing any, the week around it.
    function choose(kind) {
        kind = kind ?? photos
        const library = pinhole.library
        const ids = library.field("id")
        const shown = pinhole.hiddenCount > 0 ? i => !pinhole.isHidden(i) : i => true
        if (kind === "all")
            return pinhole.hiddenCount > 0 ? ids.filter((id, i) => shown(i)) : ids
        if (kind !== "onthisday") {
            const favourite = library.field("favorite")
            return ids.filter((id, i) => favourite[i] && shown(i))
        }
        const days = library.field("day")
        const now = new Date()
        const monthDay = date => (date.getMonth() + 1) * 100 + date.getDate()
        const today = monthDay(now)
        const near = new Set()
        for (let d = -3; d <= 3; ++d)
            if (d !== 0)
                near.add(monthDay(new Date(now.getFullYear(), now.getMonth(), now.getDate() + d)))
        const list = [], week = []
        for (let i = 0; i < days.length; ++i) {
            const day = days[i]
            if (day === 0 || Math.floor(day / 10000) >= now.getFullYear() || !shown(i))
                continue
            if (day % 10000 === today)
                list.push(ids[i])
            else if (near.has(day % 10000))
                week.push(ids[i])
        }
        return list.length > 0 ? list : week
    }
    function nothing() {
        say(photos === "onthisday" ? qsTr("Nothing taken on this day in other years") : qsTr("No favourites to show"))
    }
    // Of `list` (ids, oldest first), the first taken at `position` in the
    // library or after it; round to the first.
    function from(list, position) {
        let lo = 0, hi = list.length
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            if (pinhole.library.indexOf(list[mid]) < position)
                lo = mid + 1
            else
                hi = mid
        }
        return list.length > 0 ? list[lo % list.length] : ""
    }

    // Showing what was ticked, not what the settings choose.
    property bool ticked: false

    // From the grid, with the photos its settings choose (or `kind`'s, for
    // its self-test): shuffled from anywhere; in order, from the photo in
    // the middle of the grid (or under the pointer) on.
    function begin(kind) {
        const list = choose(kind)
        if (list.length === 0) {
            nothing()
            return
        }
        ticked = false
        frame.start(shuffle ? "" : from(list, pinhole.currentIndex()), list, false, () => choose(kind))
    }

    // A word on the photos when nothing could be shown.
    function say(text) {
        notice.text = text
        noticeTimer.restart()
    }

    actions: [
        PluginAction {
            id: gridAction
            place: "grid"
            text: qsTr("Frame")
            key: "P"
            usable: pinhole.library.count > 0
            onTriggered: plugin.begin()
        },
        PluginAction {
            id: viewerAction
            place: "viewer"
            text: qsTr("Frame")
            key: "P"
            onTriggered: {
                // The photo shown first, once, and then the photos chosen.
                const list = plugin.choose()
                if (list.length === 0) {
                    plugin.nothing()
                    return
                }
                plugin.ticked = false
                frame.start(pinhole.library.item(pinhole.viewer.index).id ?? "", list, true, () => plugin.choose())
            }
        },
        PluginAction {
            place: "selection"
            text: qsTr("Frame")
            key: "P"
            usable: pinhole.selectionCount > 0
            onTriggered: {
                const library = pinhole.library
                const ticked = Array.from(pinhole.selection()).map(i => library.item(i).id)
                plugin.ticked = true
                // Those still there and not hidden since.
                frame.start(plugin.shuffle ? "" : ticked[0], ticked, false, () => ticked.filter(id => {
                    const i = library.indexOf(id)
                    return i >= 0 && !pinhole.isHidden(i)
                }))
            }
        }
    ]

    help: [
        ["P", qsTr("From this photo on")],
        ["P", qsTr("The photos chosen, from the grid")],
        ["P", qsTr("Of what is ticked")]
    ]

    tourReady: pinhole.library.count > 0
    tour: [
        {
            title: qsTr("A photo frame"),
            text: qsTr("Frame (P) shows your photos one after another, slowly moving, full window, and keeps the screen on: for a laptop left open. On a photo, P starts from it; in select mode, P shows what is ticked."),
            action: gridAction
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
                text: qsTr("Frame (P) in the grid starts it; P on a photo starts from it; in select mode, P shows what is ticked. While it shows, the screen stays on.")
            }
        }
    }

    Frame {
        id: frame
        objectName: "photoFrame"
        anchors.fill: parent
        plugin: plugin
        // The library read again without any of them; what was ticked
        // gone needs no word.
        onEmptied: if (!plugin.ticked) plugin.nothing()
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
