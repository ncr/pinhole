import QtQuick
import Pinhole
import "keys.js" as KeyNames
import "focus.js" as Focus

// Pinhole's side panel, on Tab or ",": over the photos from the left,
// hidden otherwise. Its named sections keep each task short; Tab moves
// through the chosen section, Escape closes it, and ←/→ changes its tab.
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
// Plugins: how many are on, then each a row with its switch and what it
// does, a dozen of them still a short list: its settings and its tour
// unfold under it, on a tap or ⏎ on the row.
Item {
    id: root
    anchors.fill: parent
    visible: panel.x > -panel.width

    property bool shown: false
    // Keep the cheap panel/focus shell available, but do not instantiate
    // every source, mark and plugin control before the first frame.
    property bool prepared: false
    onPreparedChanged: if (prepared) orderPlugins()
    property string section: "sources"
    property var returnFocus: null
    // Where the keys go as it closes when they cannot go back (the grid).
    property Item fallbackFocus: null
    readonly property var sections: [
        { id: "sources", text: qsTr("Sources") },
        { id: "photos", text: qsTr("Photos") },
        { id: "plugins", text: qsTr("Plugins") },
        { id: "storage", text: qsTr("Storage") },
        { id: "updates", text: qsTr("Updates") }
    ].filter(s => s.id !== "updates" || updater.supported)
    readonly property int visibleSources: section === "sources" ? sourceCount : 0
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Settings")
    Accessible.ignored: !shown
    FocusFence.enabled: shown

    function currentFocus() { return root.Window.window ? root.Window.window.activeFocusItem : null }
    function moveFocus(direction) { Focus.move(root, direction, currentFocus()) }
    function restoreFocus() {
        const source = section === "sources" ? sourceRows.itemAt(Math.max(0, sources.currentIndex)) : null
        const target = source ?? stopsShown[0] ?? sectionTabs.itemAt(sections.findIndex(s => s.id === section))
        if (target) { target.forceActiveFocus(Qt.TabFocusReason); Focus.reveal(target) }
        else root.forceActiveFocus()
    }
    function selectSection(id, keepNavigationFocus) {
        section = id
        confirming = -1
        askingFor = null
        pending = -1
        focusIndex = visibleSources > 0 ? Math.max(0, sources.currentIndex) : 0
        scroller.contentY = 0
        if (shown) Qt.callLater(() => {
            const nav = sectionTabs.itemAt(sections.findIndex(s => s.id === section))
            if (keepNavigationFocus && nav) nav.forceActiveFocus(Qt.TabFocusReason)
            else restoreFocus()
        })
    }
    function turnSection(direction) {
        const at = sections.findIndex(s => s.id === section)
        selectSection(sections[(at + direction + sections.length) % sections.length].id, true)
    }
    // Its keys only while it has them: not under a tour, the help or the guide.
    Keys.onShortcutOverride: event => {
        if (keymap.layer !== "side") return
        const tab = (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
        const navigation = currentFocus()?.objectName?.startsWith("sideSection-") ?? false
        if (tab || (navigation && (event.key === Qt.Key_Left || event.key === Qt.Key_Right))) event.accepted = true
    }
    Keys.onPressed: event => {
        if (keymap.layer !== "side") return
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            moveFocus(event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) ? -1 : 1)
            event.accepted = true
        } else if (currentFocus()?.objectName?.startsWith("sideSection-") && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            turnSection(event.key === Qt.Key_Left ? -1 : 1)
            event.accepted = true
        }
    }

    // A part's title, in the accent, a hairline from it to its key or count
    // at the right.
    component Heading: Item {
        property string text
        property string key
        width: parent.width
        height: Math.round(theme.fontSize * 2.333)
        UiText {
            id: title
            x: Math.round(theme.fontSize * 0.714)
            y: centredIn(parent.height)
            text: parent.text
            color: theme.accent
            font.weight: Font.DemiBold
        }
        UiText {
            id: aside
            anchors.right: parent.right
            anchors.rightMargin: Math.round(theme.fontSize * 0.714)
            y: centredIn(parent.height)
            text: parent.key
            color: theme.darkForeground
        }
        Rectangle {
            readonly property int gap: Math.round(theme.fontSize * 0.6)
            x: title.x + title.implicitWidth + gap
            width: Math.max(0, (aside.text !== "" ? aside.x - gap : parent.width - title.x) - x)
            y: Math.round(parent.height / 2)
            height: 1
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.14)
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
    // Where the keys are: a source's row, then each other control in the
    // panel in turn (`stops`, which add themselves as they are made), as
    // ↑ ↓ move, ⏎ and Space press, the panel scrolling to show it.
    property int focusIndex: 0
    property var stops: []
    function addStop(item) {
        stops = stops.concat([item])
        item.activeFocusChanged.connect(() => {
            if (item.activeFocus) {
                const at = stopsShown.indexOf(item)
                if (at >= 0) focusIndex = visibleSources + at
            }
        })
    }
    function dropStop(item) { stops = stops.filter(s => s !== item) }
    // Those showing, in the order they are laid out.
    readonly property var stopsShown: {
        const shown = stops.filter(s => s.visible && s.enabled && s.usable !== false)
        return shown.sort((a, b) => a.mapToItem(top, 0, 0).y - b.mapToItem(top, 0, 0).y
                                   || a.mapToItem(top, 0, 0).x - b.mapToItem(top, 0, 0).x)
    }
    readonly property int sourceCount: sources.list.length
    readonly property var focusedStop: focusIndex >= visibleSources ? stopsShown[focusIndex - visibleSources] ?? null : null
    // Plugin controls are revealed by keyboard navigation. A pointer may
    // focus a switch already under it without moving the list.
    onFocusIndexChanged: if (section !== "plugins") showFocus()
    function showFocus() {
        const item = focusedStop ?? sourceRows.itemAt(focusIndex)
        if (!item)
            return
        const y = item.mapToItem(top, 0, 0).y
        const margin = theme.fontSize
        if (y < scroller.contentY + margin)
            scroller.contentY = Math.max(0, y - margin)
        else if (y + item.height > scroller.contentY + scroller.height - margin)
            scroller.contentY = Math.min(scroller.contentHeight - scroller.height, y + item.height - scroller.height + margin)
    }
    // The row asking before it acts, -1 for none; and what it asks before:
    // Remove (null), or one of its kind's actions.
    property int confirming: -1
    property var askingFor: null
    signal addRequested()
    // Its row tapped while the source shown needs signing in.
    signal signInRequested(var source)
    // Each kind of source as its plugin shows it, by kind (PluginSource,
    // Main.qml's pluginSources).
    property var pluginSources: ({})
    // The loaded plugin of an id (Main.qml), for its settings; `pluginsLoaded`
    // changes as plugins load.
    property var pluginItem: id => null
    property int pluginsLoaded: 0
    // A plugin's tour asked for again.
    signal tourRequested(string id)
    signal guideRequested()
    // A plugin's part of the panel, scrolled into view, for a tour to light;
    // kept there by `keptInView` as the parts above it fill in.
    property string keptInView
    // The plugins unfolded, by id, to their settings and tour; for as long
    // as the window is open.
    property var unfolded: ({})
    function unfold(id, open) {
        const now = Object.assign({}, unfolded)
        now[id] = open
        unfolded = now
    }
    readonly property int pluginsOn: prepared ? (plugins.revision, plugins.ids.filter(id => plugins.at(id).enabled).length) : 0
    // The plugins in the order listed: Pinhole's own, then one's own, each
    // by its name in the language. Set only when it changes, so the rows
    // stay as they are otherwise.
    property var pluginOrder: []
    function orderPlugins() {
        if (!prepared) return
        const order = plugins.ids.slice().sort((a, b) => {
            const x = plugins.at(a), y = plugins.at(b)
            return x.personal !== y.personal ? (x.personal ? 1 : -1) : x.name.localeCompare(y.name)
        })
        if (order.join("\n") !== pluginOrder.join("\n"))
            pluginOrder = order
    }
    Connections {
        target: plugins
        function onChanged() { root.orderPlugins() }
        function onIdsChanged() { root.orderPlugins() }
    }
    Component.onCompleted: orderPlugins()
    function showPlugin(id) {
        prepared = true
        section = "plugins"
        unfold(id, true)
        pluginsSection.forceLayout()
        top.forceLayout()
        const k = pluginOrder.indexOf(id)
        const row = k >= 0 ? pluginRows.itemAt(k) : null
        if (row) {
            const y = row.mapToItem(scroller.contentItem, 0, 0).y
            if (y + row.height > scroller.contentY + scroller.height || y < scroller.contentY)
                scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, y + row.height - scroller.height + theme.fontSize))
        }
        return row
    }

    // The plugins new since last looked at (Main.qml's notice), marked New
    // for as long as the window is open.
    property var newIds: []
    function showNew(ids) {
        newIds = ids
        prepared = true
        section = "plugins"
        shown = true
        // The first of them in sight, once the rows and the focus are in place.
        Qt.callLater(() => Qt.callLater(() => {
            pluginsSection.forceLayout()
            top.forceLayout()
            const k = pluginOrder.findIndex(id => ids.indexOf(id) >= 0)
            const row = k >= 0 ? pluginRows.itemAt(k) : null
            if (!row)
                return
            const y = row.mapToItem(scroller.contentItem, 0, 0).y
            if (y + row.height > scroller.contentY + scroller.height || y < scroller.contentY)
                scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, y - theme.fontSize))
        }))
    }

    // A mark's switch's words: Pinhole's own marks' here, a plugin's its
    // own, else its plugin's name.
    function markText(kind) {
        if (kind.plugin !== "")
            return kind.text !== "" ? kind.text : plugins.at(kind.plugin).name
        switch (kind.id) {
        case "pinhole/video": return qsTr("Videos")
        case "pinhole/stack": return qsTr("Series and other groups")
        }
        return kind.name
    }

    onShownChanged: {
        if (shown) prepared = true
        confirming = -1
        focusIndex = Math.max(0, sources.currentIndex)
        storage.watching = shown
        pending = -1
        if (shown) {
            returnFocus = currentFocus()
            // Opened by a tour, which keeps the keys (→ goes on, not to
            // another tab): the focus stays where it was.
            if (keymap.layer !== "tour")
                Qt.callLater(restoreFocus)
        } else {
            // Back where it was; or, that gone (the notice that opened it),
            // to `fallbackFocus`, never left in the panel hidden.
            const previous = returnFocus
            Qt.callLater(() => {
                if (previous?.visible && previous.enabled) previous.forceActiveFocus()
                else if (root.fallbackFocus && Focus.within(root, currentFocus())) root.fallbackFocus.forceActiveFocus()
            })
        }
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
    // A source as its plugin is given it: the one shown with how it is
    // doing (only the one shown is being looked after).
    function at(i) {
        const s = sources.list[i]
        if (!s)
            return s
        const shown = s.id === sources.current
        return Object.assign({}, s, { shown: shown, health: shown ? root.health : "" })
    }
    function kindOf(s) { return s ? root.pluginSources[s.kind] ?? null : null }
    function open(i) {
        const s = at(i)
        if (!s)
            return
        const kind = kindOf(s)
        if (s.shown && s.health === "needs_sign_in" && kind && kind.signIn)
            root.signInRequested(s)
        else
            sources.show(s.id)
    }
    // What its plugin says it needs (signing in again, its disk), in place
    // of its count; "" when all is well.
    function trouble(s) {
        const kind = kindOf(s)
        return kind && kind.note ? kind.note(s) || "" : ""
    }
    function detail(s) {
        const said = trouble(s)
        if (said !== "")
            return said
        const kind = kindOf(s)
        const noun = kind && kind.noun !== "" ? kind.noun : plugins.at(s.plugin).name ?? s.kind
        return noun + " · " + count(s.count)
    }
    // Its buttons beside Remove, as its plugin gives them.
    function actionsOf(s) {
        const kind = kindOf(s)
        return kind && kind.actions ? kind.actions(s) || [] : []
    }
    function act(i, action) {
        if (action.ask) {
            root.askingFor = action
            root.confirming = i
        } else {
            action.triggered()
        }
    }
    // Every source's buttons, once each, for the keys under the list: those
    // it has when all is well (Sign out too while signed out), so the list
    // stays as it is as the one shown comes and goes.
    readonly property var actionKeys: {
        const seen = {}, keys = []
        for (let i = 0; i < sources.list.length; ++i)
            for (const action of actionsOf(Object.assign({}, sources.list[i], { shown: false, health: "" })))
                if (action.key && !seen[action.key]) {
                    seen[action.key] = true
                    keys.push([KeyNames.nameOf(action.key), action.text])
                }
        return keys
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
        const kind = kindOf(s)
        const next = r.next === undefined || r.next <= root.now ? qsTr("Checking now…")
                   : r.push ? (kind && kind.pushed !== "" ? kind.pushed : qsTr("It says when something changes"))
                   : qsTr("Next check in %1").arg(span(r.next - root.now))
        return last + "\n" + next
    }

    // Its keys, while it is on top (Command.qml); the buttons it shows
    // press the same.
    Command {
        name: "side.up"
        keys: ["Up"]
        keyLayer: "side"
        usable: root.confirming < 0
        autoRepeat: true
        mouse: qsTr("Point at a row")
        touchpad: qsTr("Point at a row")
        onTriggered: root.moveFocus(-1)
    }
    Command {
        name: "side.down"
        keys: ["Down"]
        keyLayer: "side"
        usable: root.confirming < 0
        autoRepeat: true
        mouse: qsTr("Point at a row")
        touchpad: qsTr("Point at a row")
        onTriggered: root.moveFocus(1)
    }
    Command {
        id: enterAction
        name: "side.enter"
        keys: ["Return", "Enter"]
        keyLayer: "side"
        mouse: qsTr("Click a row, or Yes")
        touchpad: qsTr("Tap a row, or Yes")
        onTriggered: {
            if (root.lowering) {
                root.confirmLimit()
            } else if (root.confirming >= 0) {
                root.answer(true)
            } else if (root.focusedStop) {
                root.focusedStop.press()
            } else {
                root.open(root.focusIndex)
            }
        }
    }
    Command {
        name: "side.press"
        keys: ["Space"]
        keyLayer: "side"
        usable: root.focusedStop !== null && root.confirming < 0 && !root.lowering
        mouse: qsTr("Click it")
        touchpad: qsTr("Tap it")
        onTriggered: root.focusedStop.press()
    }
    function answer(yes) {
        if (yes && root.askingFor) {
            root.askingFor.triggered()
        } else if (yes) {
            sources.remove(root.at(root.confirming).id)
            root.focusIndex = Math.max(0, Math.min(root.focusIndex, sources.list.length - 2))
        }
        root.confirming = -1
        root.askingFor = null
    }
    Command {
        id: removeSource
        name: "side.remove"
        keys: ["Delete", "Backspace"]
        keyLayer: "side"
        usable: root.section === "sources" && root.confirming < 0 && root.focusIndex < root.sourceCount
        // By a pointer, Remove on a source's row; with no source there is
        // no row, and nothing to remove.
        noMouse: root.sourceCount === 0 ? qsTr("No source to remove") : ""
        noTouchpad: root.sourceCount === 0 ? qsTr("No source to remove") : ""
        onTriggered: {
            root.askingFor = null
            root.confirming = root.focusIndex
        }
    }
    Command {
        id: addAction
        name: "side.add"
        keys: ["A"]
        keyLayer: "side"
        usable: root.section === "sources" && root.confirming < 0
        onTriggered: root.addRequested()
    }
    Command {
        id: closeSide
        name: "side.close"
        keys: ["Escape"]
        keyLayer: "side"
        mouse: qsTr("Click beside it")
        touchpad: qsTr("Tap beside it")
        onTriggered: root.lowering ? root.pending = -1 : root.confirming >= 0 ? root.confirming = -1 : root.shown = false
    }
    // Storage.
    Command {
        id: storageLess
        name: "storage.less"
        keys: ["Left"]
        keyLayer: "side"
        usable: root.section === "storage" && storage.loaded
        autoRepeat: true
        onTriggered: root.step(-1)
    }
    Command {
        id: storageMore
        name: "storage.more"
        keys: ["Right"]
        keyLayer: "side"
        usable: root.section === "storage" && storage.loaded
        autoRepeat: true
        onTriggered: root.step(1)
    }
    Command {
        id: locationAction
        name: "storage.location"
        keys: ["L"]
        keyLayer: "side"
        usable: root.section === "storage" && storage.loaded && !storage.moving
        onTriggered: storage.chooseLocation()
    }

    // The photos dimmed behind it, as behind Omarchy's menus and the other
    // panels (a scrim of the theme's background at half), coming and going
    // with the bar's slide: they are not what the keys work on meanwhile.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
        opacity: Math.max(0, Math.min(1, (panel.x + panel.width) / panel.width))
    }

    // A click beside it puts it away; nothing else there reaches the photos.
    Shield {
        anchors.fill: parent
        enabled: root.shown
        onClicked: root.shown = false
    }

    Rectangle {
        id: panel
        width: Math.min(parent.width - 16, theme.fontSize * 36)
        height: parent.height
        x: root.shown ? 0 : -width - 1
        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: Qt.rgba(theme.darkBackground.r, theme.darkBackground.g, theme.darkBackground.b, 0.96)
        Shield { anchors.fill: parent }
        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
        }

        Column {
            id: navigation
            x: Math.round(theme.fontSize * 0.857)
            y: x
            width: parent.width - 2 * x
            spacing: Math.round(theme.fontSize * 0.6)
            Item {
                width: parent.width
                height: closeSettings.height
                UiText {
                    text: qsTr("Settings")
                    y: centredIn(parent.height)
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(theme.fontSize * 1.25)
                }
                ShellButton {
                    id: closeSettings
                    anchors.right: parent.right
                    text: qsTr("Close")
                    hint: "Esc"
                    action: closeSide
                }
            }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    id: sectionTabs
                    model: root.sections
                    delegate: ShellButton {
                        required property var modelData
                        objectName: "sideSection-" + modelData.id
                        text: modelData.text
                        active: root.section === modelData.id
                        flat: true
                        Accessible.role: Accessible.PageTab
                        Accessible.selected: active
                        onTriggered: root.selectSection(modelData.id, true)
                    }
                }
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Tab to move · Esc to close")
                color: theme.darkForeground
                font.pixelSize: Math.round(theme.fontSize * 0.85)
            }
            Rectangle { width: parent.width; height: 1; color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.14) }
        }
        Flickable {
            id: scroller
            objectName: "sidePanelScroller"
            anchors.fill: parent
            onContentHeightChanged: if (root.keptInView !== "") root.showPlugin(root.keptInView)
            anchors.margins: Math.round(theme.fontSize * 0.857)
            anchors.topMargin: navigation.y + navigation.height + theme.fontSize
            contentHeight: top.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true

        Column {
            id: top
            width: parent.width
            spacing: 6

            Column {
                id: sourcesSection
                width: parent.width
                spacing: 6
                visible: root.section === "sources"
                Heading {
                    text: qsTr("Sources")
                    key: "Tab"
                }

                Repeater {
                    id: sourceRows
                    model: root.prepared ? sources.list : []
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        readonly property bool current: modelData.id === sources.current
                        readonly property bool asking: root.confirming === index
                        // As its plugin is given it, and what it says of it.
                        readonly property var source: (root.health, root.at(index))
                        readonly property var kind: root.kindOf(source)
                        readonly property string trouble: root.trouble(source)
                        readonly property var actions: root.actionsOf(source)
                        width: parent.width
                        height: rowContent.implicitHeight + Math.round(theme.fontSize * 1.143)
                        radius: Math.min(4, theme.radius)
                        color: current ? Qt.tint(theme.selection, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, sourceTap.pressed ? 0.18 : hover.hovered ? 0.08 : 0))
                             : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, sourceTap.pressed ? 0.22 : hover.hovered ? 0.08 : 0)
                        activeFocusOnTab: true
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.name
                        Accessible.description: root.detail(row.source)
                        Accessible.checkable: true
                        Accessible.checked: current
                        Accessible.focusable: true
                        Accessible.focused: activeFocus
                        Accessible.pressed: sourceTap.pressed
                        Accessible.onPressAction: root.open(index)
                        onActiveFocusChanged: if (activeFocus) { root.focusIndex = index; Focus.reveal(row) }
                        Keys.onShortcutOverride: event => {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) event.accepted = true
                        }
                        Keys.onReturnPressed: root.open(index)
                        Keys.onEnterPressed: root.open(index)
                        Keys.onSpacePressed: root.open(index)
                        border.width: 1
                        border.color: asking ? theme.red
                                    : root.shown && root.focusIndex === index && (sources.list.length > 1 || keymap.input === "keyboard")
                                      ? theme.accent : "transparent"
                        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor; onHoveredChanged: if (hovered) root.focusIndex = row.index }
                        // Not a tap on one of its buttons, which do their own.
                        TapHandler {
                            id: sourceTap
                            onTapped: eventPoint => {
                                const p = row.mapToItem(rowButtons, eventPoint.position.x, eventPoint.position.y)
                                if (rowButtons.visible && rowButtons.contains(p))
                                    return
                                row.forceActiveFocus(Qt.MouseFocusReason)
                                root.confirming >= 0 ? root.confirming = -1 : root.open(row.index)
                            }
                        }

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
                                text: row.kind ? row.kind.icon : "\uf03e"
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
                                    text: root.detail(row.source)
                                    color: row.trouble !== "" ? theme.red : theme.darkForeground
                                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                                }
                                UiText {
                                    visible: !row.asking && row.trouble === "" && text !== ""
                                    width: parent.width
                                    text: root.rounds(row.source)
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
                                    text: root.askingFor ? root.askingFor.ask
                                          : row.kind && row.kind.forgets ? row.kind.forgets(row.source)
                                          : qsTr("Forget its index? The photos stay where they are.")
                                }
                                // Its buttons: Yes and No while asking; else, under
                                // the pointer or the keys, its plugin's (Sign out)
                                // and Remove.
                                Row {
                                    id: rowButtons
                                    spacing: 6
                                    visible: row.asking || hover.hovered || (root.focusIndex === row.index && keymap.input === "keyboard")
                                    ShellButton {
                                        visible: row.asking
                                        active: true
                                        text: qsTr("Yes")
                                        hint: "⏎"
                                        onTriggered: root.answer(true)
                                    }
                                    ShellButton {
                                        visible: row.asking
                                        flat: true
                                        text: qsTr("No")
                                        hint: "Esc"
                                        onTriggered: root.confirming = -1
                                    }
                                    Repeater {
                                        model: row.actions
                                        delegate: ShellButton {
                                            required property var modelData
                                            visible: !row.asking
                                            flat: true
                                            text: modelData.text
                                            key: modelData.key ?? ""
                                            keyLayer: "side"
                                            usable: root.confirming < 0
                                            onTriggered: root.act(row.index, modelData)
                                        }
                                    }
                                    ShellButton {
                                        visible: !row.asking
                                        flat: true
                                        text: qsTr("Remove")
                                        action: removeSource
                                    }
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
                    id: addButton
                    text: qsTr("Add a source")
                    lit: root.focusedStop === addButton
                    Component.onCompleted: root.addStop(addButton)
                    Component.onDestruction: root.dropStop(addButton)
                    action: addAction
                }
                // The sources' keys, a pair after another as the line has room;
                // with no source, none of them does anything.
                Flow {
                    visible: root.sourceCount > 0
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    topPadding: 4
                    spacing: Math.round(theme.fontSize * 0.3)
                    Repeater {
                        model: [["↑ ↓", qsTr("Choose")], ["⏎", qsTr("Show it")], ["1–9", qsTr("Show one, from anywhere")]]
                               .concat(root.actionKeys).concat([["Del", qsTr("Remove")]])
                        delegate: Row {
                            required property var modelData
                            spacing: Math.round(theme.fontSize * 0.4)
                            rightPadding: Math.round(theme.fontSize * 0.8)
                            UiText {
                                text: modelData[0]
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            UiText {
                                text: modelData[1]
                                color: theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                        }
                    }
                }

            }
            Column {
                id: updatesSection
                width: parent.width
                spacing: 6
                visible: root.section === "updates"
                Heading { visible: updater.supported; height: visible ? Math.round(theme.fontSize * 2.333) : 0; text: qsTr("Pinhole updates") }
                UpdateStatus { id: updateStatus }
                Note { visible: updater.supported; height: visible ? implicitHeight : 0; text: updateStatus.text }
                Flow {
                    visible: updater.supported
                    height: visible ? implicitHeight : 0
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    spacing: 6
                    ShellButton {
                        id: updateButton
                        objectName: "updateButton"
                        text: updateStatus.button
                        usable: updater.supported && !updateStatus.busy
                        busy: updateStatus.busy
                        lit: root.focusedStop === updateButton
                        Component.onCompleted: root.addStop(updateButton)
                        Component.onDestruction: root.dropStop(updateButton)
                        onTriggered: updateStatus.press()
                    }
                    ShellButton {
                        id: updateLogButton
                        text: qsTr("Show update log")
                        visible: updater.logPath !== "" && updater.state === "error"
                        lit: root.focusedStop === updateLogButton
                        Component.onCompleted: root.addStop(updateLogButton)
                        Component.onDestruction: root.dropStop(updateLogButton)
                        onTriggered: updater.showLog()
                    }
                }

            }
            Column {
                id: storageSection
                width: parent.width
                spacing: 6
                visible: root.section === "storage"
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
                        lit: root.focusedStop === change
                        Component.onCompleted: root.addStop(change)
                        Component.onDestruction: root.dropStop(change)
                        anchors.right: parent.right
                        usable: !storage.moving
                        text: qsTr("Change")
                        action: locationAction
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
                // Of what: a line each, the sizes lined up at the right.
                Column {
                    readonly property var kept: ["library", "full", "medium", "video", "original", "live"]
                        .filter(kind => (storage.kinds[kind] || 0) > 0)
                    visible: storage.loaded && kept.length > 0
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    topPadding: 2
                    bottomPadding: 2
                    Repeater {
                        model: parent.kept
                        delegate: Item {
                            required property string modelData
                            width: parent.width
                            height: kindName.implicitHeight
                            UiText {
                                id: kindName
                                text: root.kindNames[modelData]
                                color: theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            UiText {
                                anchors.right: parent.right
                                text: storage.size(storage.kinds[modelData])
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                        }
                    }
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
                            action: storageLess
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
                            action: storageMore
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
                            onTriggered: root.confirmLimit()
                        }
                        ShellButton {
                            text: qsTr("Cancel")
                            hint: "Esc"
                            onTriggered: root.pending = -1
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
                // What the limit can and cannot do.
                Note {
                    visible: storage.loaded
                    text: qsTr("The index and thumbnails always stay, so the limit is never under %1. Unless you choose one, it is %2.")
                          .arg(storage.size(storage.minLimit)).arg(storage.size(storage.defaultLimit))
                }

            }
            Column {
                id: photosSection
                width: parent.width
                spacing: 6
                visible: root.section === "photos"
                // Only on a display that shows more than SDR white (stage.h).
                Item { width: 1; height: Math.round(theme.fontSize * 1.5); visible: display.hdr }
                Heading { text: qsTr("Display"); visible: display.hdr }
                Item {
                    visible: display.hdr
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    height: hdrSwitch.height
                    UiText {
                        width: parent.width - hdrSwitch.width - theme.fontSize
                        y: centredIn(parent.height)
                        text: qsTr("Full HDR")
                        color: theme.brightForeground
                        elide: Text.ElideRight
                    }
                    Toggle {
                        id: hdrSwitch
                        objectName: "fullHdrSwitch"
                        lit: root.focusedStop === hdrSwitch
                        Component.onCompleted: root.addStop(hdrSwitch)
                        Component.onDestruction: root.dropStop(hdrSwitch)
                        anchors.right: parent.right
                        checked: stage.fullHdr
                        Accessible.name: qsTr("Full HDR")
                        onToggled: stage.fullHdr = !stage.fullHdr
                    }
                }
                Note {
                    visible: display.hdr
                    text: qsTr("Photos and videos as bright as the display can show them. Off, nothing is brighter than white.")
                }

                Item { width: 1; height: Math.round(theme.fontSize * 1.5); visible: display.hdr }
                Heading { text: qsTr("In the grid") }
                Note { text: qsTr("Photo order") }
                Flow {
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    spacing: 6
                    Repeater {
                        model: [{ value: "smart", text: qsTr("Automatic") },
                                { value: "taken", text: qsTr("Date taken") },
                                { value: "added", text: qsTr("Date added") }]
                        delegate: ShellButton {
                            id: orderChoice
                            required property var modelData
                            objectName: "order-" + modelData.value
                            text: modelData.text
                            active: timeline.mode === modelData.value
                            checkable: true
                            Accessible.role: Accessible.RadioButton
                            lit: root.focusedStop === orderChoice
                            Component.onCompleted: root.addStop(orderChoice)
                            Component.onDestruction: root.dropStop(orderChoice)
                            onTriggered: timeline.mode = modelData.value
                        }
                    }
                }
                Note {
                    text: qsTr("Automatic order keeps photos added in the last 30 days at the newest end. Older additions follow the date taken. Your choice is remembered.")
                }
                Item { width: 1; height: Math.round(theme.fontSize * 0.6) }
                Note { text: qsTr("Marks in the corner of a photo that say what it is.") }
                // Pinhole's own marks and the plugins' (plugins.markKinds), in
                // their order in a cell's corner, each switch kept by its id.
                Repeater {
                    model: root.prepared ? plugins.markKinds : []
                    delegate: Item {
                        id: markRow
                        required property var modelData
                        readonly property bool on: (markChoices.revision, markChoices.shown(modelData.id, modelData.defaultShown))
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
                                source: "image://mark/" + encodeURIComponent(markRow.modelData.icon)
                            }
                        }
                        UiText {
                            x: swatch.width + Math.round(theme.fontSize * 0.6)
                            width: parent.width - x - markSwitch.width - theme.fontSize
                            y: centredIn(parent.height)
                            text: root.markText(markRow.modelData)
                            color: theme.brightForeground
                            elide: Text.ElideRight
                        }
                        Toggle {
                            id: markSwitch
                            objectName: "markSwitch:" + markRow.modelData.id
                            lit: root.focusedStop === markSwitch
                            Component.onCompleted: root.addStop(markSwitch)
                            Component.onDestruction: root.dropStop(markSwitch)
                            anchors.right: parent.right
                            checked: markRow.on
                            Accessible.name: root.markText(markRow.modelData)
                            onToggled: markChoices.setShown(markRow.modelData.id, !markRow.on)
                        }
                    }
                }

            }
            Column {
                id: pluginsSection
                width: parent.width
                spacing: 6
                visible: root.section === "plugins"
                Heading {
                    text: qsTr("Plugins")
                    key: plugins.ids.length > 0 ? qsTr("%1 of %2 on").arg(root.pluginsOn).arg(plugins.ids.length) : ""
                }
                Note { text: qsTr("Make Pinhole your own: new views, photo tools and sources. The workshop has examples, the complete API and instructions for an agent.") }
                ShellButton {
                    id: guideButton
                    objectName: "pluginGuideButton"
                    x: Math.round(theme.fontSize * 0.714)
                    text: qsTr("Build a plugin")
                    lit: root.focusedStop === guideButton
                    Component.onCompleted: root.addStop(guideButton)
                    Component.onDestruction: root.dropStop(guideButton)
                    onTriggered: root.guideRequested()
                }
                Item { width: 1; height: theme.fontSize }

                // Each: its name and switch, what it does under them; on, and
                // with settings or a tour, a tap on it (or ⏎ or Space) unfolds
                // them, a line down from its chevron tying them to it.
                Repeater {
                    id: pluginRows
                    objectName: "settingsPluginRows"
                    model: root.prepared ? root.pluginOrder : []
                    delegate: Item {
                        id: pluginRow
                        required property string modelData
                        required property int index
                        readonly property var info: (plugins.revision, plugins.at(modelData))
                        readonly property var item: (root.pluginsLoaded, root.pluginItem(modelData))
                        readonly property bool on: info.enabled === true
                        readonly property bool hasSettings: on && item !== null && item.settings !== null
                        readonly property bool hasTour: on && item !== null && (item.tour?.length ?? 0) > 0
                        readonly property bool open: (hasSettings || hasTour) && root.unfolded[modelData] === true
                        // One that brings sources of photos: what turning it
                        // off keeps, and, one's own, what trusting it means.
                        readonly property bool bringsSources: (info.sources || []).length > 0 && info.error === ""
                        // Its kinds' sources sign in (iCloud's): as pinholed
                        // says of the kind, on or off, or as the plugin's own
                        // PluginSource does once it is on.
                        readonly property bool signsIn: (sources.kinds, (info.sources || []).some(kind =>
                            sources.kinds.some(k => k.kind === kind && k.needs_sign_in === true)
                            || (item?.sources ?? []).some(s => s.kind === kind && s.signIn)))
                        readonly property string trust: !bringsSources ? ""
                            : info.copy && signsIn ? qsTr("Your own copy of %1: it sees the password you type.").arg(info.name)
                            : info.copy ? qsTr("Your own copy of %1: it reads the photos of every source you show with it.").arg(info.name)
                            : info.personal ? qsTr("It reads your photos, and the accounts you sign in to with it. Turn it on only if you trust it.")
                            : ""
                        // The gutter its chevron stands in, as a source's glyph does.
                        readonly property int gutter: Math.round(theme.fontSize * 0.714) + Math.round(theme.fontSize * 1.429)
                        width: parent.width
                        height: pluginColumn.implicitHeight

                        // A hairline from the one before.
                        Rectangle {
                            visible: pluginRow.index > 0
                            x: Math.round(theme.fontSize * 0.714)
                            width: parent.width - 2 * x
                            height: 1
                            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.08)
                        }
                        Column {
                            id: pluginColumn
                            width: parent.width
                            Rectangle {
                                id: pluginHead
                                width: parent.width
                                height: headText.implicitHeight + 2 * headText.y
                                radius: Math.min(4, theme.radius)
                                color: headHover.hovered && unfold.visible
                                       ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.05) : "transparent"
                                border.width: 1
                                border.color: root.focusedStop === unfold ? theme.accent : "transparent"
                                HoverHandler { id: headHover; cursorShape: unfold.visible ? Qt.PointingHandCursor : Qt.ArrowCursor }
                                // Not a tap on its switch, which does its own.
                                TapHandler {
                                    onTapped: eventPoint => {
                                        const p = pluginHead.mapToItem(pluginSwitch, eventPoint.position.x, eventPoint.position.y)
                                        if (!(pluginSwitch.visible && pluginSwitch.contains(p))) {
                                            unfold.forceActiveFocus(Qt.MouseFocusReason)
                                            unfold.press()
                                        }
                                    }
                                }
                                // A chevron, turned down unfolded: a stop for the keys.
                                ControlSurface {
                                    id: unfold
                                    visible: pluginRow.hasSettings || pluginRow.hasTour
                                    function press() { root.unfold(pluginRow.modelData, !pluginRow.open) }
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("Settings for %1").arg(pluginRow.info.name)
                                    Accessible.expandable: true
                                    Accessible.expanded: pluginRow.open
                                    Accessible.focusable: true
                                    Accessible.focused: activeFocus
                                    Accessible.onPressAction: press()
                                    onActivated: press()
                                    onActiveFocusChanged: if (activeFocus) Focus.reveal(unfold)
                                    Component.onCompleted: root.addStop(unfold)
                                    Component.onDestruction: root.dropStop(unfold)
                                    x: Math.round(theme.fontSize * 0.714)
                                    width: Math.round(theme.fontSize * 1.429)
                                    // On the switch's line, so the keys come to
                                    // it before the switch.
                                    y: pluginSwitch.y
                                    height: pluginSwitch.height
                                    UiText {
                                        anchors.fill: parent
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        text: "\uf054"
                                        font.pixelSize: Math.round(theme.fontSize * 0.643)
                                        rotation: pluginRow.open ? 90 : 0
                                        Behavior on rotation { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                                        color: pluginRow.open ? theme.accent : theme.darkForeground
                                        Accessible.ignored: true
                                    }
                                }
                                Column {
                                    id: headText
                                    x: pluginRow.gutter
                                    y: Math.round(theme.fontSize * 0.5)
                                    width: pluginSwitch.x - x - theme.fontSize
                                    spacing: 2
                                    Row {
                                        width: parent.width
                                        spacing: Math.round(theme.fontSize * 0.5)
                                        UiText {
                                            id: name
                                            width: Math.min(implicitWidth, parent.width - (yours.visible ? yours.width + parent.spacing : 0)
                                                            - (fresh.visible ? fresh.width + parent.spacing : 0))
                                            text: pluginRow.info.name
                                            color: pluginRow.on ? theme.brightForeground : theme.foreground
                                            elide: Text.ElideRight
                                        }
                                        // New since last looked at (showNew).
                                        Rectangle {
                                            id: fresh
                                            objectName: "newPluginMark"
                                            visible: root.newIds.indexOf(pluginRow.modelData) >= 0
                                            anchors.verticalCenter: name.verticalCenter
                                            width: freshText.implicitWidth + Math.round(theme.fontSize * 0.8)
                                            height: freshText.implicitHeight + 2
                                            radius: height / 2
                                            color: "transparent"
                                            border.width: 1
                                            border.color: theme.accent
                                            UiText {
                                                id: freshText
                                                anchors.centerIn: parent
                                                text: qsTr("New")
                                                color: theme.accent
                                                font.pixelSize: Math.round(theme.fontSize * 0.714)
                                            }
                                        }
                                        // One of one's own.
                                        Rectangle {
                                            id: yours
                                            visible: pluginRow.info.personal === true
                                            anchors.verticalCenter: name.verticalCenter
                                            width: yoursText.implicitWidth + Math.round(theme.fontSize * 0.8)
                                            height: yoursText.implicitHeight + 2
                                            radius: height / 2
                                            color: "transparent"
                                            border.width: 1
                                            border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.3)
                                            UiText {
                                                id: yoursText
                                                anchors.centerIn: parent
                                                text: qsTr("Yours")
                                                color: theme.darkForeground
                                                font.pixelSize: Math.round(theme.fontSize * 0.714)
                                            }
                                        }
                                    }
                                    // What it does, two lines of it folded; or
                                    // why it did not load.
                                    UiText {
                                        width: parent.width
                                        visible: text !== ""
                                        text: pluginRow.info.error !== "" ? pluginRow.info.error : pluginRow.info.description
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: pluginRow.open || pluginRow.info.error !== "" ? 1000 : 2
                                        elide: Text.ElideRight
                                        color: pluginRow.info.error !== "" ? theme.red : theme.darkForeground
                                        opacity: pluginRow.on || pluginRow.info.error !== "" ? 1 : 0.75
                                        font.pixelSize: Math.round(theme.fontSize * 0.857)
                                    }
                                    UiText {
                                        width: parent.width
                                        visible: text !== ""
                                        text: pluginRow.trust
                                        wrapMode: Text.WordWrap
                                        color: theme.yellow
                                        font.pixelSize: Math.round(theme.fontSize * 0.857)
                                    }
                                    UiText {
                                        width: parent.width
                                        visible: pluginRow.bringsSources && !pluginRow.on
                                        text: qsTr("Its sources and their photos stay on this computer, out of sight, until it is on again.")
                                        wrapMode: Text.WordWrap
                                        color: theme.foreground
                                        font.pixelSize: Math.round(theme.fontSize * 0.857)
                                    }
                                    // What it needs that is not installed (its
                                    // manifest's "requires").
                                    UiText {
                                        width: parent.width
                                        visible: (pluginRow.info.missing ?? []).length > 0
                                        text: qsTr("It needs %1, which is not installed here.")
                                                  .arg((pluginRow.info.missing ?? []).join(", "))
                                        wrapMode: Text.WordWrap
                                        color: theme.yellow
                                        font.pixelSize: Math.round(theme.fontSize * 0.857)
                                    }
                                    // What went wrong in it as it ran: its QML not
                                    // loading, an error thrown, a warning from one
                                    // of its files, each with the file and line.
                                    UiText {
                                        width: parent.width
                                        visible: pluginRow.on && text !== ""
                                        text: pluginRow.info.problem ?? ""
                                        wrapMode: Text.WrapAnywhere
                                        color: theme.red
                                        font.pixelSize: Math.round(theme.fontSize * 0.786)
                                    }
                                }
                                Toggle {
                                    id: pluginSwitch
                                    objectName: "pluginSwitch:" + pluginRow.modelData
                                    revealOnPointerFocus: false
                                    lit: root.focusedStop === pluginSwitch
                                    Component.onCompleted: root.addStop(pluginSwitch)
                                    Component.onDestruction: root.dropStop(pluginSwitch)
                                    anchors.right: parent.right
                                    anchors.rightMargin: Math.round(theme.fontSize * 0.714)
                                    y: headText.y + name.y + Math.round((name.implicitHeight - height) / 2)
                                    visible: pluginRow.info.error === ""
                                    checked: pluginRow.on
                                    Accessible.name: pluginRow.info.name
                                    onToggled: plugins.setEnabled(pluginRow.modelData, !checked)
                                }
                            }
                            // Unfolded: its settings, then its tour again.
                            Item {
                                visible: pluginRow.open
                                width: parent.width
                                height: visible ? pluginBody.implicitHeight : 0
                                Rectangle {
                                    x: Math.round(theme.fontSize * 0.714) + Math.round(theme.fontSize * 0.714)
                                    width: 1
                                    height: parent.height - Math.round(theme.fontSize * 0.6)
                                    color: Qt.alpha(theme.accent, 0.4)
                                }
                                Column {
                                    id: pluginBody
                                    x: pluginRow.gutter
                                    width: parent.width - x - Math.round(theme.fontSize * 0.714)
                                    spacing: 6
                                    topPadding: 2
                                    bottomPadding: Math.round(theme.fontSize * 0.857)
                                    Loader {
                                        width: parent.width
                                        active: pluginRow.open && pluginRow.hasSettings
                                        visible: active
                                        sourceComponent: active ? pluginRow.item.settings : null
                                    }
                                    ShellButton {
                                        id: tourButton
                                        visible: pluginRow.open && pluginRow.hasTour
                                        lit: root.focusedStop === tourButton
                                        Component.onCompleted: root.addStop(tourButton)
                                        Component.onDestruction: root.dropStop(tourButton)
                                        text: qsTr("Show how it works")
                                        onTriggered: root.tourRequested(pluginRow.modelData)
                                    }
                                }
                            }
                        }
                    }
                }
                // One's own.
                Rectangle {
                    x: Math.round(theme.fontSize * 0.714)
                    width: parent.width - 2 * x
                    height: 1
                    color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.08)
                }
                Note {
                    topPadding: Math.round(theme.fontSize * 0.5)
                    text: qsTr("Plugins of your own go in %1: a folder with a manifest.json and a QML file. They load again as you save them.")
                          .arg(plugins.personalDir.replace(/^\/home\/[^/]+/, "~"))
                }
                ShellButton {
                    x: Math.round(theme.fontSize * 0.714)
                    id: folderButton
                    text: qsTr("Open the folder")
                    lit: root.focusedStop === folderButton
                    Component.onCompleted: root.addStop(folderButton)
                    Component.onDestruction: root.dropStop(folderButton)
                    onTriggered: plugins.showPersonalDir()
                }
                Item { width: 1; height: theme.fontSize }
            }
        }
        }
        // The wheel and the touchpad anywhere over the panel, its margins
        // too, scroll it as the photos scroll, and stop at its ends.
        Scroller {
            anchors.fill: parent
            flickable: scroller
        }
    }
}
