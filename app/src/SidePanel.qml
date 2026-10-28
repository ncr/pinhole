import QtQuick
import QtCore

// Pinhole's side panel, on Tab or ",": over the photos from the left,
// hidden otherwise. Everything that is set is here, in three parts.
//
// Sources: every source added. One click or key shows another: ↑ ↓ and
// Enter here, 1 to 9 from anywhere. What needs doing shows on its row
// (signing in again, a disk to connect, thumbnails still coming), and
// Delete takes one out, asked on the row first.
//
// Storage: the one folder Pinhole keeps everything in (each source's
// library, and the photos fetched from them), how much room that takes and
// how much it may; the library always fits.
//
// In the grid: which marks the cells carry (a video, a Live Photo, a
// favourite, several photos as one card), each with its switch.
//
// Plugins: each with its switch, and its own settings under it while on.
Item {
    id: root
    anchors.fill: parent
    visible: panel.x > -panel.width

    property bool shown: false

    // A part's title, in the accent, with a key at the right.
    component Heading: Item {
        property string text
        property string key
        width: parent.width
        height: Math.round(theme.fontSize * 2.333)
        UiText {
            x: Math.round(theme.fontSize * 0.714)
            y: centredIn(parent.height)
            text: parent.text
            color: theme.accent
        }
        UiText {
            anchors.right: parent.right
            anchors.rightMargin: Math.round(theme.fontSize * 0.714)
            y: centredIn(parent.height)
            text: parent.key
            color: theme.darkForeground
        }
    }
    // Words under a title, inset as the rows' are.
    component Note: UiText {
        x: Math.round(theme.fontSize * 0.714)
        width: parent.width - 2 * x
        wrapMode: Text.WordWrap
        color: theme.darkForeground
        font.pixelSize: Math.round(theme.fontSize * 0.857)
    }

    // How the source shown is doing (the service's health).
    property string health: "fine"
    // The row the keys are on; the one asked about before removing or
    // signing out, and which of the two.
    property int focusIndex: 0
    property int confirming: -1
    property bool signingOut: false
    signal addRequested()
    signal signInRequested(string appleId)
    // The loaded plugin of an id (Main.qml), for its settings; `pluginsLoaded`
    // changes as plugins load.
    property var pluginItem: id => null
    property int pluginsLoaded: 0

    // The marks the grid draws (marks.h: Mark), kept for this window alone:
    // the service has no use for them.
    readonly property int marks: gridSettings.marks
    Settings {
        id: gridSettings
        location: StandardPaths.writableLocation(StandardPaths.GenericConfigLocation) + "/pinhole/window.conf"
        category: "grid"
        // Not Live: nearly every iPhone photo is one.
        property int marks: 13
    }

    onShownChanged: {
        confirming = -1
        focusIndex = Math.max(0, sources.currentIndex)
        storage.watching = shown
        pending = -1
    }

    // A limit stepped to but not yet set. One at or above what is kept is
    // set a moment later; one below it waits to be confirmed, since setting
    // it removes photos.
    property double pending: -1
    readonly property double chosen: pending >= 0 ? pending : storage.limit
    readonly property bool lowering: pending >= 0 && pending < storage.used
    readonly property double removing: Math.max(0, storage.used - chosen)

    // The limits to step through: the least (the library and a little room
    // for photos), round sizes up to what this disk could hold, the default
    // and the one set now.
    readonly property var steps: {
        const gb = 1024 * 1024 * 1024
        const most = Math.max(storage.limit, storage.used + storage.free)
        const list = [5, 10, 20, 50, 100, 200, 500, 1000, 2000]
            .map(n => n * gb).filter(b => b > storage.minLimit && b <= most)
        for (const b of [storage.minLimit, storage.defaultLimit, storage.limit])
            if (b > 0 && list.indexOf(b) < 0)
                list.push(b)
        return list.sort((a, b) => a - b)
    }
    function step(direction) {
        if (steps.length === 0)
            return
        let at = 0
        while (at < steps.length - 1 && steps[at] < chosen)
            ++at
        if (steps[at] > chosen && direction > 0)
            --at
        const next = steps[Math.max(0, Math.min(steps.length - 1, at + direction))]
        pending = next === storage.limit ? -1 : next
        if (pending >= 0 && !lowering)
            settle.restart()
    }
    function confirmLimit() {
        if (pending >= 0)
            storage.setLimit(pending)
        pending = -1
    }
    // Stepping up (or down while still above what is kept) sets the limit
    // once the steps stop.
    Timer { id: settle; interval: 600; onTriggered: if (!root.lowering) root.confirmLimit() }

    readonly property var kindNames: ({
        "library": qsTr("Index and thumbnails"),
        "full": qsTr("Full size"),
        "medium": qsTr("Previews"),
        "video": qsTr("Videos"),
        "original": qsTr("Originals"),
        "live": qsTr("Live Photos")
    })

    function count(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0) }
    function at(i) { return sources.list[i] }
    function open(i) {
        const s = at(i)
        if (!s)
            return
        if (s.id === sources.current && trouble(s) === "needs_sign_in")
            root.signInRequested(s.account)
        else
            sources.show(s.id)
    }
    // What is wrong with a source, as far as is known: only the one shown
    // is being looked after.
    function trouble(s) {
        if (s.id !== sources.current)
            return ""
        if (root.health === "needs_sign_in" && s.kind === "icloud")
            return "needs_sign_in"
        if (root.health === "offline" && s.kind === "folder")
            return "offline"
        return ""
    }
    function detail(s) {
        switch (trouble(s)) {
        case "needs_sign_in": return qsTr("Sign in again")
        case "offline": return qsTr("Not there: is its disk connected?")
        }
        const kind = s.kind === "icloud" ? "iCloud" : qsTr("Folder")
        return kind + " · " + count(s.count)
    }
    // When the one shown was last looked at for new photos, and when it is
    // next (the service's rounds), counted on as the bar is open; or
    // that Apple notifies of changes, when it does (a round then comes at
    // once, and the minute's rounds are only in case one is missed).
    property double now: Date.now()
    Timer {
        running: root.shown
        interval: 1000
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = Date.now()
    }
    function span(ms) {
        const s = Math.max(1, Math.round(ms / 1000))
        if (s < 60)
            return qsTr("%1 s").arg(s)
        if (s < 3600)
            return qsTr("%1 min").arg(Math.round(s / 60))
        return qsTr("%1 h").arg(Math.round(s / 3600))
    }
    // The two, a line each; nothing for a source not shown.
    function rounds(s) {
        const r = sources.rounds
        if (s.id !== sources.current || r.source !== s.id)
            return ""
        const ago = root.now - r.last
        const last = r.last === undefined ? qsTr("Not checked yet")
                   : ago < 5000 ? qsTr("Checked just now")
                   : qsTr("Checked %1 ago").arg(span(ago))
        const next = r.next === undefined || r.next <= root.now ? qsTr("Checking now…")
                   : r.push ? qsTr("Apple notifies of changes")
                   : qsTr("Next check in %1").arg(span(r.next - root.now))
        return last + "\n" + next
    }

    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "Up"; context: Qt.ApplicationShortcut
        onActivated: root.focusIndex = Math.max(0, root.focusIndex - 1) }
    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "Down"; context: Qt.ApplicationShortcut
        onActivated: root.focusIndex = Math.min(sources.list.length - 1, root.focusIndex + 1) }
    Shortcut {
        enabled: root.shown; sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut
        onActivated: {
            if (root.lowering) {
                root.confirmLimit()
            } else if (root.confirming >= 0 && root.signingOut) {
                sources.signOut(root.at(root.confirming).id)
                root.confirming = -1
            } else if (root.confirming >= 0) {
                sources.remove(root.at(root.confirming).id)
                root.confirming = -1
                root.focusIndex = Math.max(0, Math.min(root.focusIndex, sources.list.length - 2))
            } else {
                root.open(root.focusIndex)
            }
        }
    }
    Shortcut { enabled: root.shown && root.confirming < 0 && sources.list.length > 0; sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut
        onActivated: {
            root.signingOut = false
            root.confirming = root.focusIndex
        } }
    // Signing out of an Apple ID keeps its photos here; signed out, the row
    // offers signing in again instead.
    Shortcut {
        enabled: root.shown && root.confirming < 0 && root.at(root.focusIndex) !== undefined
                 && root.at(root.focusIndex).kind === "icloud" && root.trouble(root.at(root.focusIndex)) !== "needs_sign_in"
        sequence: "O"; context: Qt.ApplicationShortcut
        onActivated: {
            root.signingOut = true
            root.confirming = root.focusIndex
        }
    }
    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "A"; context: Qt.ApplicationShortcut; onActivated: root.addRequested() }
    Shortcut {
        enabled: root.shown; sequence: "Escape"; context: Qt.ApplicationShortcut
        onActivated: root.lowering ? root.pending = -1 : root.confirming >= 0 ? root.confirming = -1 : root.shown = false
    }
    // Storage.
    Shortcut { enabled: root.shown && storage.loaded; sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: root.step(-1) }
    Shortcut { enabled: root.shown && storage.loaded; sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: root.step(1) }
    Shortcut { enabled: root.shown && storage.outdated; sequence: "R"; context: Qt.ApplicationShortcut; onActivated: storage.restartService() }
    Shortcut { enabled: root.shown && storage.loaded && !storage.moving; sequence: "L"; context: Qt.ApplicationShortcut; onActivated: storage.chooseLocation() }

    // The photos dimmed behind it, as behind Omarchy's menus and the other
    // panels (a scrim of the theme's background at half), coming and going
    // with the bar's slide: they are not what the keys work on meanwhile.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
        opacity: Math.max(0, Math.min(1, (panel.x + panel.width) / panel.width))
    }

    // A click beside it puts it away.
    MouseArea {
        anchors.fill: parent
        enabled: root.shown
        onClicked: root.shown = false
    }

    Rectangle {
        id: panel
        width: Math.min(parent.width - 48, theme.fontSize * 24)
        height: parent.height
        x: root.shown ? 0 : -width - 1
        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: Qt.rgba(theme.darkBackground.r, theme.darkBackground.g, theme.darkBackground.b, 0.96)
        MouseArea { anchors.fill: parent }
        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
        }

        Flickable {
            anchors.fill: parent
            anchors.margins: Math.round(theme.fontSize * 0.857)
            contentHeight: top.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true

        Column {
            id: top
            width: parent.width
            spacing: 6

            Heading {
                text: qsTr("Sources")
                key: "Tab"
            }

            Repeater {
                model: sources.list
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool current: modelData.id === sources.current
                    readonly property bool asking: root.confirming === index
                    readonly property string trouble: root.trouble(modelData)
                    width: parent.width
                    height: rowContent.implicitHeight + Math.round(theme.fontSize * 1.143)
                    radius: Math.min(4, theme.radius)
                    color: current ? theme.selection
                         : hover.hovered ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.05) : "transparent"
                    border.width: 1
                    border.color: asking ? theme.red
                                : root.shown && root.focusIndex === index && sources.list.length > 1 ? theme.accent : "transparent"
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor; onHoveredChanged: if (hovered) root.focusIndex = row.index }
                    TapHandler { onTapped: root.confirming >= 0 ? root.confirming = -1 : root.open(row.index) }

                    Row {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Math.round(theme.fontSize * 0.714)
                        anchors.rightMargin: Math.round(theme.fontSize * 0.714)
                        spacing: Math.round(theme.fontSize * 0.714)
                        UiText {
                            id: glyph
                            width: Math.round(theme.fontSize * 1.429)
                            horizontalAlignment: Text.AlignHCenter
                            text: row.modelData.kind === "icloud" ? "" : ""
                            color: row.trouble !== "" ? theme.red : row.current ? theme.accent : theme.foreground
                        }
                        Column {
                            width: parent.width - glyph.width - number.width - 2 * parent.spacing
                            spacing: 2
                            UiText {
                                width: parent.width
                                text: row.modelData.name
                                color: row.trouble === "offline" ? theme.darkForeground : theme.brightForeground
                                wrapMode: Text.WrapAnywhere
                            }
                            UiText {
                                visible: !row.asking
                                width: parent.width
                                text: root.detail(row.modelData)
                                color: row.trouble !== "" ? theme.red : theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            UiText {
                                visible: !row.asking && row.trouble === "" && text !== ""
                                width: parent.width
                                text: root.rounds(row.modelData)
                                wrapMode: Text.WordWrap
                                color: theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            // Thumbnails still coming.
                            Rectangle {
                                visible: !row.asking && row.modelData.thumbs < row.modelData.count
                                width: parent.width
                                height: 2
                                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                                Rectangle {
                                    width: parent.width * row.modelData.thumbs / Math.max(1, row.modelData.count)
                                    height: 2
                                    color: theme.accent
                                }
                            }
                            // Removing, asked first: what goes, and what stays.
                            UiText {
                                visible: row.asking
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: theme.red
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                                text: (root.signingOut
                                       ? qsTr("Sign out? Its index stays here, so signing in again is quick.")
                                       : row.modelData.kind === "icloud"
                                       ? qsTr("Sign out and forget its index? The photos stay in iCloud.")
                                       : qsTr("Forget its index? The photos stay in the folder."))
                                      + "  " + qsTr("⏎ yes · Esc no")
                            }
                        }
                        UiText {
                            id: number
                            text: row.index < 9 ? String(row.index + 1) : ""
                            color: theme.darkForeground
                        }
                    }
                }
            }

            ShellButton {
                width: parent.width
                text: qsTr("Add a source")
                hint: "A"
                onTapped: root.addRequested()
            }
            // The sources' keys.
            Grid {
                x: Math.round(theme.fontSize * 0.714)
                topPadding: 4
                columns: 2
                columnSpacing: theme.fontSize
                rowSpacing: 2
                // Key, words; key, words.
                Repeater {
                    model: ["↑ ↓", qsTr("Choose"), "⏎", qsTr("Show it"), "1–9", qsTr("Show one, from anywhere"),
                            "O", qsTr("Sign out"), "Del", qsTr("Remove")]
                    delegate: UiText {
                        required property var modelData
                        required property int index
                        text: modelData
                        color: index % 2 === 0 ? theme.foreground : theme.darkForeground
                        font.pixelSize: Math.round(theme.fontSize * 0.857)
                    }
                }
            }

            Item { width: 1; height: theme.fontSize }
            Heading { text: qsTr("Storage") }

            // Where.
            Note {
                visible: storage.loaded
                text: qsTr("Pinhole keeps its library and photos in")
            }
            Item {
                visible: storage.loaded
                x: Math.round(theme.fontSize * 0.714)
                width: parent.width - 2 * x
                height: change.height
                UiText {
                    width: parent.width - change.width - theme.fontSize
                    y: centredIn(parent.height)
                    text: storage.location
                    color: theme.brightForeground
                    elide: Text.ElideMiddle
                }
                ShellButton {
                    id: change
                    anchors.right: parent.right
                    usable: !storage.moving
                    text: qsTr("Change")
                    hint: "L"
                    onTapped: storage.chooseLocation()
                }
            }

            // How much: what is kept against the limit, in the accent;
            // what a lower limit would remove, in red.
            Item {
                visible: storage.loaded
                x: Math.round(theme.fontSize * 0.714)
                width: parent.width - 2 * x
                height: Math.max(4, Math.round(theme.fontSize * 0.4))
                readonly property double whole: Math.max(root.chosen, storage.used, 1)
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                }
                Rectangle {
                    width: parent.width * Math.min(storage.used, root.chosen) / parent.whole
                    height: parent.height
                    radius: height / 2
                    color: theme.accent
                    Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                }
                Rectangle {
                    visible: root.removing > 0
                    x: parent.width * root.chosen / parent.whole
                    width: parent.width - x
                    height: parent.height
                    radius: height / 2
                    color: theme.red
                }
            }
            Note {
                visible: storage.loaded
                color: theme.foreground
                text: qsTr("%1 of %2 used · %3 free on this disk")
                      .arg(storage.size(storage.used)).arg(storage.size(storage.limit)).arg(storage.size(storage.free))
            }
            Note {
                text: {
                    const parts = []
                    for (const kind of ["library", "full", "medium", "video", "original", "live"]) {
                        const bytes = storage.kinds[kind] || 0
                        if (bytes > 0)
                            // Kept on one line each.
                            parts.push((root.kindNames[kind] + " " + storage.size(bytes)).replace(/ /g, "\u00a0"))
                    }
                    return parts.join(" · ")
                }
                visible: storage.loaded && text !== ""
            }

            // The limit, stepped with the buttons or ← →.
            Item {
                visible: storage.loaded
                x: Math.round(theme.fontSize * 0.714)
                width: parent.width - 2 * x
                height: less.height
                UiText {
                    y: centredIn(parent.height)
                    text: qsTr("Keep at most")
                }
                Row {
                    anchors.right: parent.right
                    spacing: 6
                    ShellButton {
                        id: less
                        text: "−"
                        hint: "←"
                        usable: root.steps.length > 0 && root.chosen > root.steps[0]
                        onTapped: root.step(-1)
                    }
                    UiText {
                        width: Math.max(implicitWidth, theme.fontSize * 4.5)
                        height: less.height
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: storage.size(root.chosen)
                        color: root.lowering ? theme.red : theme.brightForeground
                    }
                    ShellButton {
                        text: "+"
                        hint: "→"
                        usable: root.steps.length > 0 && root.chosen < root.steps[root.steps.length - 1]
                        onTapped: root.step(1)
                    }
                }
            }

            // A lower limit than what is kept: what it removes, asked first.
            Column {
                visible: root.lowering
                x: Math.round(theme.fontSize * 0.714)
                width: parent.width - 2 * x
                spacing: 6
                UiText {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: theme.red
                    text: qsTr("This removes %1 of the photos viewed longest ago from this computer. They stay in %2 and come back when you look at them.")
                          .arg(storage.size(root.removing)).arg(storage.sourceName)
                }
                Row {
                    spacing: 6
                    ShellButton {
                        active: true
                        text: qsTr("Remove %1").arg(storage.size(root.removing))
                        hint: "⏎"
                        onTapped: root.confirmLimit()
                    }
                    ShellButton {
                        text: qsTr("Cancel")
                        hint: "Esc"
                        onTapped: root.pending = -1
                    }
                }
            }

            // Under way.
            Row {
                visible: storage.freeing
                x: Math.round(theme.fontSize * 0.714)
                spacing: theme.fontSize
                Spinner { busy: storage.freeing; delay: 0 }
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Freeing space…")
                }
            }
            Column {
                visible: storage.moving
                x: Math.round(theme.fontSize * 0.714)
                width: parent.width - 2 * x
                spacing: 6
                UiText {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Moving everything to the new folder: %1 of %2")
                          .arg(storage.size(storage.moved)).arg(storage.size(storage.moveTotal))
                }
                Item {
                    width: parent.width
                    height: Math.max(4, Math.round(theme.fontSize * 0.4))
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                    }
                    Rectangle {
                        width: parent.width * (storage.moveTotal > 0 ? storage.moved / storage.moveTotal : 0)
                        height: parent.height
                        radius: height / 2
                        color: theme.accent
                    }
                }
            }
            Note {
                visible: storage.error !== ""
                color: theme.red
                text: storage.error
            }
            ShellButton {
                visible: storage.outdated
                x: Math.round(theme.fontSize * 0.714)
                text: qsTr("Restart it")
                hint: "R"
                onTapped: storage.restartService()
            }
            // What the limit can and cannot do.
            Note {
                visible: storage.loaded
                text: qsTr("The index and thumbnails always stay, so the limit is never under %1. Unless you choose one, it is %2.")
                      .arg(storage.size(storage.minLimit)).arg(storage.size(storage.defaultLimit))
            }

            Item { width: 1; height: theme.fontSize }
            Heading { text: qsTr("In the grid") }
            Note { text: qsTr("Marks in the corner of a photo that say what it is.") }
            Repeater {
                model: [
                    { mark: 1, icon: "video", name: qsTr("Videos") },
                    { mark: 2, icon: "live", name: qsTr("Live Photos") },
                    { mark: 4, icon: "favorite", name: qsTr("Favourites") },
                    { mark: 8, icon: "stack", name: qsTr("Series and other groups") }
                ]
                delegate: Item {
                    id: markRow
                    required property var modelData
                    readonly property bool on: (gridSettings.marks & modelData.mark) !== 0
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    height: markSwitch.height
                    // As it looks on a photo: on a dim square, white.
                    Rectangle {
                        id: swatch
                        width: Math.round(theme.fontSize * 1.8)
                        height: width
                        anchors.verticalCenter: parent.verticalCenter
                        radius: Math.min(4, theme.radius)
                        color: "#5d6670"
                        opacity: markRow.on ? 1 : 0.45
                        Image {
                            anchors.centerIn: parent
                            width: Math.round(parent.width * 0.9)
                            height: width
                            sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
                            source: "image://mark/" + markRow.modelData.icon
                        }
                    }
                    UiText {
                        x: swatch.width + Math.round(theme.fontSize * 0.6)
                        width: parent.width - x - markSwitch.width - theme.fontSize
                        y: centredIn(parent.height)
                        text: markRow.modelData.name
                        color: theme.brightForeground
                        elide: Text.ElideRight
                    }
                    Toggle {
                        id: markSwitch
                        anchors.right: parent.right
                        checked: markRow.on
                        onToggled: gridSettings.marks ^= markRow.modelData.mark
                    }
                }
            }

            Item { width: 1; height: theme.fontSize }
            Heading { text: qsTr("Plugins") }

            Repeater {
                model: plugins.ids
                delegate: Item {
                    id: pluginRow
                    required property string modelData
                    readonly property var info: (plugins.revision, plugins.at(modelData))
                    readonly property var item: (root.pluginsLoaded, root.pluginItem(modelData))
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    height: pluginColumn.implicitHeight + 6
                    Column {
                        id: pluginColumn
                        width: parent.width
                        spacing: 2
                        Item {
                            width: parent.width
                            height: pluginSwitch.height
                            UiText {
                                width: parent.width - pluginSwitch.width - theme.fontSize
                                y: centredIn(parent.height)
                                text: pluginRow.info.name
                                color: theme.brightForeground
                                wrapMode: Text.WordWrap
                            }
                            Toggle {
                                id: pluginSwitch
                                anchors.right: parent.right
                                visible: pluginRow.info.error === ""
                                checked: pluginRow.info.enabled
                                onToggled: plugins.setEnabled(pluginRow.modelData, !checked)
                            }
                        }
                        UiText {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            visible: text !== ""
                            text: pluginRow.info.error !== "" ? pluginRow.info.error
                                : (pluginRow.info.personal ? qsTr("Yours.") + " " : "") + pluginRow.info.description
                            color: pluginRow.info.error !== "" ? theme.red : theme.darkForeground
                            font.pixelSize: Math.round(theme.fontSize * 0.857)
                        }
                        Loader {
                            width: parent.width
                            active: pluginRow.info.enabled && pluginRow.item !== null && pluginRow.item.settings !== null
                            sourceComponent: active ? pluginRow.item.settings : null
                        }
                    }
                }
            }
            // One's own.
            Note {
                topPadding: 4
                text: qsTr("Plugins of your own go in %1: a folder with a manifest.json and a QML file. They load again as you save them.")
                      .arg(plugins.personalDir.replace(/^\/home\/[^/]+/, "~"))
            }
            ShellButton {
                x: Math.round(theme.fontSize * 0.714)
                text: qsTr("Open the folder")
                onTapped: plugins.showPersonalDir()
            }
            Item { width: 1; height: theme.fontSize }
        }
        }
    }
}
