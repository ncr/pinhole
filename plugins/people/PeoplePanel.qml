import QtQuick
import QtQuick.Window
import Pinhole.Plugin

Panel {
    id: panel
    property bool shown: false
    property bool returning: false
    property string source: ""
    property string stateKey: ""
    property var edits: ({})
    property var anchor: null
    property string tab: "groups"
    property int listPage: 0
    readonly property string grouping: pinhole.settings.grouping || "careful"
    property string picking: ""
    property var pickerOpener: null
    property var assignmentJob: null
    readonly property var names: {
        const found = new Set()
        for (const key of Object.keys(edits)) {
            if (edits[key].name) found.add(edits[key].name)
            if (edits[key].person) found.add(edits[key].person)
        }
        return Array.from(found).sort((a, b) => a.localeCompare(b))
    }
    readonly property bool personView: !!anchor?.name
    property var rows: []
    property int page: 0
    property bool nextPage: false
    property int selected: 0
    property var status: ({ phase: "waiting" })
    property string failure: ""
    property var readJob: null
    readonly property var selection: rows[selected]
    visible: shown
    color: Qt.rgba(0, 0, 0, 0.36)
    onClicked: close()

    function show() {
        if (shown) return
        source = pinhole.library.key || pinhole.library.dir
        stateKey = "people:" + source
        edits = pinhole.state[stateKey] || {}
        shown = true
        tab = names.length ? "people" : "groups"
        page = 0
        status = { phase: "waiting" }
        forceActiveFocus()
        load(false)
    }
    function close() {
        readJob?.cancel()
        readJob = null
        closePicker(false)
        shown = false
        returning = false
        anchor = null
        rows = []
        failure = ""
    }
    function load(focusResults = true) {
        readJob?.cancel()
        const operation = anchor ? (personView ? "person" : "members") : tab
        const job = request(operation, { anchor: anchor?.anchor, person: anchor?.name, offset: page * 48 })
        readJob = job
        job.then(result => {
            if (readJob !== job) return
            const response = JSON.parse(result.stdout)
            failure = ""
            status = response.status
            nextPage = response.rows.length > 48
            if (!response.rows.length && page > 0) {
                readJob = null
                --page
                load(focusResults)
                return
            }
            const current = selection?.key
            const incoming = response.rows.slice(0, 48).filter(row => {
                const photo = pinhole.library.item(row.id)
                return photo.thumbPath === pinhole.library.dir + "/" + row.path
            })
            if (JSON.stringify(incoming) !== JSON.stringify(rows)) {
                rows = incoming
                selected = Math.max(0, rows.findIndex(row => row.key === current))
                if (grid.activeFocus && rows.length)
                    grid.positionViewAtIndex(selected, GridView.Contain)
            }
            readJob = null
            if (shown && !picking && focusResults && rows.length) grid.forceActiveFocus()
        }).catch(error => {
            if (readJob !== job) return
            readJob = null
            if (error.code !== "cancelled") {
                failure = qsTr("People could not load the face groups.")
                pinhole.warn(error)
            }
        })
    }
    function statusText() {
        switch (status.phase) {
        case "scanning": return qsTr("Checked %1 of %2 thumbnails.").arg(status.done).arg(status.total)
        case "grouping": return qsTr("Grouping similar faces…")
        case "models": return qsTr("Downloading face models…")
        case "runtime": return qsTr("Face indexing needs Python OpenCV and NumPy.")
        case "error": return qsTr("Face indexing encountered an error and will retry.")
        case "ready":
            if (status.errors) return qsTr("Skipped unreadable or missing thumbnails: %1.").arg(status.errors)
            if (personView) return qsTr("Photos assigned to this person.")
            if (anchor) return qsTr("Review the faces, then assign the group or the selected photo.")
            if (tab === "people") return qsTr("Choose a person to see their photos.")
            return qsTr("Face groups are suggestions. New photos are checked in the background.")
        default: return qsTr("Waiting for background face indexing…")
        }
    }
    Timer {
        interval: 3000
        repeat: true
        running: panel.shown && !panel.picking
        onTriggered: { if (!panel.readJob) panel.load(false) }
    }
    function enter(row) {
        if (!row) return
        if (anchor) {
            if (pinhole.library.indexOf(row.id) < 0) return
            returning = true
            pinhole.openPhotoDirect(row.id)
        } else {
            anchor = row
            listPage = page
            page = 0
            rows = []
            nextPage = false
            load()
        }
    }
    function back(reload = true) {
        if (picking) {
            closePicker()
        } else if (!anchor) {
            close()
        } else {
            anchor = null
            page = listPage
            rows = []
            nextPage = false
            if (reload) load()
        }
    }
    function request(operation, options) {
        return pinhole.run("pinhole-faces", [operation, "--cache", pinhole.cacheDir], {
            input: JSON.stringify(Object.assign({ source: source, edits: edits }, options)),
            timeout: 10000
        })
    }
    function openPicker(mode, opener) {
        readJob?.cancel()
        readJob = null
        picking = mode
        pickerOpener = opener
        failure = ""
        picker.reset()
    }
    function closePicker(restoreFocus = true) {
        assignmentJob?.cancel()
        assignmentJob = null
        picking = ""
        if (restoreFocus && pickerOpener) pickerOpener.forceActiveFocus()
        pickerOpener = null
    }
    function writeEdits(key, patch, focusResults = false) {
        const saved = Object.assign({}, pinhole.state[key] || {})
        for (const face of Object.keys(patch)) {
            const edit = patch[face]
            if (edit.name || edit.person || edit.rejected?.length) saved[face] = edit
            else delete saved[face]
        }
        pinhole.state[key] = saved
        if (stateKey === key) {
            edits = saved
            if (shown) load(focusResults)
        }
    }
    function edit(patch, label, focusResults = false) {
        const key = stateKey
        const before = {}
        for (const face of Object.keys(patch))
            before[face] = Object.assign({}, edits[face])
        writeEdits(key, patch, focusResults)
        pinhole.did(label, () => panel.writeEdits(key, before), () => panel.writeEdits(key, patch))
    }
    function assign(name) {
        if (picking === "photo") {
            const face = selection.key
            const last = !personView && rows.length === 1 && page === 0 && !nextPage
            closePicker(!last)
            if (last) {
                back(false)
                forceActiveFocus()
            }
            edit({ [face]: Object.assign({}, edits[face], { person: name }) }, qsTr("assigning a photo"), last)
            return
        }
        if (picking === "merge") {
            const from = anchor.name
            const patch = {}
            for (const face of Object.keys(edits)) {
                const previous = edits[face]
                if (previous.name === from || previous.person === from) {
                    patch[face] = Object.assign({}, previous)
                    if (previous.name === from) patch[face].name = name
                    if (previous.person === from) patch[face].person = name
                }
            }
            closePicker(false)
            back(false)
            forceActiveFocus()
            edit(patch, qsTr("merging people"), true)
            return
        }
        const job = request("assignment", { anchor: anchor.anchor })
        assignmentJob = job
        job.then(result => {
            if (assignmentJob !== job) return
            const patch = {}
            for (const face of JSON.parse(result.stdout).keys)
                patch[face] = Object.assign({}, edits[face], { person: name })
            closePicker(false)
            back(false)
            forceActiveFocus()
            if (Object.keys(patch).length) edit(patch, qsTr("assigning a group"), true)
            else load()
        }).catch(error => {
            if (assignmentJob !== job) return
            assignmentJob = null
            failure = qsTr("People could not load the face groups.")
            pinhole.warn(error)
        })
    }
    function move(delta) {
        if (!rows.length) return
        selected = Math.max(0, Math.min(rows.length - 1, selected + delta))
        grid.positionViewAtIndex(selected, GridView.Contain)
    }

    function tabKey(event) {
        return (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
    }
    Keys.onShortcutOverride: event => { if (tabKey(event)) event.accepted = true }
    Keys.onPressed: event => {
        if (!tabKey(event)) return
        const candidates = picking ? [picker.searchField, picker.choicesList, picker.createButton, picker.cancelButton]
            : [closeButton, ...tabs.children, ...groupingOptions.children, backButton, groupButton, assignButton, openButton, rejectButton, grid, previous, next]
        const controls = candidates.filter(item => item.visible && item.activeFocusOnTab && item.usable !== false)
        const reverse = event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier)
        const current = controls.indexOf(panel.Window.window.activeFocusItem)
        const index = current < 0 ? (reverse ? controls.length - 1 : 0)
            : (current + (reverse ? -1 : 1) + controls.length) % controls.length
        controls[index]?.forceActiveFocus(reverse ? Qt.BacktabFocusReason : Qt.TabFocusReason)
        event.accepted = true
    }

    Command { name: "people.back"; keys: ["Escape"]; keyLayer: "plugin"; when: panel.shown; onTriggered: panel.back() }
    Command { name: "people.enter"; keys: ["Return"]; keyLayer: "plugin"; when: panel.shown && grid.activeFocus; onTriggered: panel.enter(panel.selection) }
    Command { name: "people.left"; keys: ["Left"]; keyLayer: "plugin"; when: panel.shown && grid.activeFocus; onTriggered: panel.move(-1) }
    Command { name: "people.right"; keys: ["Right"]; keyLayer: "plugin"; when: panel.shown && grid.activeFocus; onTriggered: panel.move(1) }
    Command { name: "people.up"; keys: ["Up"]; keyLayer: "plugin"; when: panel.shown && grid.activeFocus; onTriggered: panel.move(-grid.columns) }
    Command { name: "people.down"; keys: ["Down"]; keyLayer: "plugin"; when: panel.shown && grid.activeFocus; onTriggered: panel.move(grid.columns) }
    Connections {
        target: pinhole
        function onDismissed() { panel.close() }
    }
    Connections {
        target: pinhole.library
        function onChanged() {
            if ((panel.shown || panel.returning) && (pinhole.library.key || pinhole.library.dir) !== panel.source)
                panel.close()
        }
    }
    Connections {
        target: pinhole.viewer
        function onOpenChanged() {
            if (!panel.returning) return
            panel.shown = !pinhole.viewer.open
            if (panel.shown) {
                panel.returning = false
                grid.forceActiveFocus()
            }
        }
    }

    Card {
        anchors.centerIn: parent
        width: Math.max(0, Math.min(880, parent.width - 24))
        height: Math.max(0, Math.min(690, parent.height - 24))
        Column {
            id: header
            visible: !panel.picking
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            spacing: 8
            Row {
                width: parent.width
                spacing: 8
                UiText {
                    width: parent.width - closeButton.width - parent.spacing
                    elide: Text.ElideRight
                    text: panel.anchor ? panel.anchor.name || qsTr("Suggested group") : qsTr("People")
                }
                ShellButton { id: closeButton; text: "✕"; onTapped: panel.close() }
            }
            Choices {
                id: tabs
                visible: !panel.anchor
                value: panel.tab
                options: [{ value: "people", text: qsTr("Named people") },
                          { value: "groups", text: qsTr("To review") }]
                onChosen: value => {
                    panel.tab = value
                    panel.page = 0
                    panel.rows = []
                    panel.load(false)
                }
            }
            Flow {
                id: groupingOptions
                visible: !panel.anchor && panel.tab === "groups"
                width: parent.width
                spacing: 4
                UiText { text: qsTr("Join groups"); height: separate.height; verticalAlignment: Text.AlignVCenter }
                ShellButton {
                    id: separate
                    objectName: "peopleGroupingOff"
                    text: qsTr("Off")
                    checkable: true
                    active: panel.grouping === "off"
                    onTapped: pinhole.settings.grouping = "off"
                }
                ShellButton {
                    objectName: "peopleGroupingCareful"
                    text: qsTr("Careful")
                    checkable: true
                    active: panel.grouping === "careful"
                    onTapped: pinhole.settings.grouping = "careful"
                }
                ShellButton {
                    objectName: "peopleGroupingWider"
                    text: qsTr("Wider")
                    checkable: true
                    active: panel.grouping === "wider"
                    onTapped: pinhole.settings.grouping = "wider"
                }
            }
            Flow {
                width: parent.width
                spacing: 8
                ShellButton { id: backButton; visible: !!panel.anchor; text: qsTr("Back to people"); hint: "Esc"; onTapped: panel.back() }
                ShellButton {
                    id: groupButton
                    objectName: "peopleAssignGroup"
                    visible: !!panel.anchor
                    usable: panel.personView ? panel.names.length > 1 : panel.rows.length > 0
                    text: panel.personView ? qsTr("Merge with a person") : qsTr("Assign group")
                    onTapped: panel.openPicker(panel.personView ? "merge" : "group", groupButton)
                }
                ShellButton {
                    id: assignButton
                    objectName: "peopleAssignPhoto"
                    visible: !!panel.anchor
                    usable: !!panel.selection
                    text: panel.personView ? qsTr("Move selected photo") : qsTr("Assign selected photo")
                    onTapped: panel.openPicker("photo", assignButton)
                }
                ShellButton {
                    id: openButton
                    objectName: "peopleOpen"
                    visible: !!panel.anchor
                    text: qsTr("Open photo")
                    usable: !!panel.selection
                    onTapped: panel.enter(panel.selection)
                }
                ShellButton {
                    id: rejectButton
                    visible: !!panel.anchor && !panel.personView
                    text: qsTr("Different person")
                    usable: !!panel.selection && panel.selection.key !== panel.anchor?.anchor
                    onTapped: {
                        const face = panel.anchor.anchor
                        const previous = panel.edits[face] || {}
                        panel.edit({ [face]: Object.assign({}, previous, {
                            rejected: (previous.rejected || []).concat([panel.selection.key])
                        }) }, qsTr("removing a face from a group"))
                    }
                }
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: panel.failure || panel.statusText()
                color: panel.failure ? theme.red : theme.foreground
            }
        }
        ScrollGrid {
            id: grid
            visible: !panel.picking
            objectName: "peopleGrid"
            readonly property int columns: Math.max(1, Math.floor(width / 154))
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            cellWidth: width / columns
            cellHeight: cellWidth
            model: panel.rows
            activeFocusOnTab: count > 0
            keyNavigationEnabled: false
            delegate: PeopleTile {
                required property int index
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight
                row: modelData
                group: !panel.anchor
                chosen: panel.selected === index
                label: group ? row.name || qsTr("Group · %1 photos").arg(row.count)
                             : pinhole.library.item(row.id).filename || qsTr("Photo %1").arg(index + 1)
                onSelected: {
                    panel.selected = index
                    if (group) panel.enter(row)
                    else grid.forceActiveFocus()
                }
            }
        }
        UiText {
            anchors.centerIn: grid
            width: Math.min(grid.width, 450)
            visible: !panel.picking && panel.status.phase === "ready" && grid.count === 0 && !panel.readJob
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: panel.anchor ? qsTr("This group has no visible matches.")
                : panel.tab === "people" ? qsTr("No named people yet. Start in To review.") : qsTr("No suggested groups on this page.")
        }
        Row {
            id: footer
            visible: !panel.picking
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 12
            spacing: 8
            ShellButton {
                id: previous
                text: qsTr("Previous")
                visible: panel.page > 0 || panel.nextPage
                usable: panel.page > 0 && !panel.readJob
                onTapped: { --panel.page; panel.load() }
            }
            ShellButton {
                id: next
                text: qsTr("Next")
                visible: panel.page > 0 || panel.nextPage
                usable: panel.nextPage && !panel.readJob
                onTapped: { ++panel.page; panel.load() }
            }
        }
        PeoplePicker {
            id: picker
            anchors.fill: parent
            anchors.margins: 12
            visible: !!panel.picking
            names: panel.names.filter(name => name !== panel.anchor?.name)
            busy: !!panel.assignmentJob
            allowCreate: panel.picking !== "merge"
            title: panel.failure || (panel.picking === "group" ? qsTr("Choose a person for this group")
                : panel.picking === "merge" ? qsTr("Choose a person to merge with")
                : qsTr("Choose a person for the selected photo"))
            onChosen: name => panel.assign(name)
            onCancelled: panel.closePicker()
        }
    }
}
