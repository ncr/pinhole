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
    property var rows: []
    property int page: 0
    property bool nextPage: false
    property int selected: 0
    property var status: ({ phase: "waiting" })
    property string failure: ""
    property var readJob: null
    readonly property var rejected: anchor ? edits[anchor.key]?.rejected || [] : []
    readonly property var visibleRows: rows.filter(row => !rejected.includes(row.key))
    readonly property var selection: visibleRows[selected]
    visible: shown
    color: Qt.rgba(0, 0, 0, 0.36)
    onClicked: close()

    function show() {
        if (shown) return
        source = pinhole.library.key || pinhole.library.dir
        stateKey = "people:" + source
        edits = pinhole.state[stateKey] || {}
        shown = true
        page = 0
        status = { phase: "waiting" }
        forceActiveFocus()
        load(false)
    }
    function close() {
        readJob?.cancel()
        readJob = null
        shown = false
        returning = false
        anchor = null
        rows = []
        failure = ""
    }
    function load(focusResults = true) {
        readJob?.cancel()
        const job = pinhole.run("pinhole-faces", [anchor ? "members" : "groups", "--cache", pinhole.cacheDir], {
            input: JSON.stringify({ source: source, anchor: anchor?.key, offset: page * 48 }),
            timeout: 10000
        })
        readJob = job
        job.then(result => {
            if (readJob !== job) return
            const response = JSON.parse(result.stdout)
            readJob = null
            failure = ""
            status = response.status
            nextPage = response.rows.length > 48
            const current = selection?.key
            const incoming = response.rows.slice(0, 48).filter(row => {
                const photo = pinhole.library.item(row.id)
                return photo.thumbPath === pinhole.library.dir + "/" + row.path
            })
            if (JSON.stringify(incoming) !== JSON.stringify(rows)) {
                rows = incoming
                selected = Math.max(0, visibleRows.findIndex(row => row.key === current))
                if (grid.activeFocus && visibleRows.length)
                    grid.positionViewAtIndex(selected, GridView.Contain)
            }
            if (shown && focusResults && visibleRows.length) grid.forceActiveFocus()
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
            return anchor ? qsTr("Review the suggested matches before naming the group.")
                          : qsTr("Face groups are suggestions. New photos are checked in the background.")
        default: return qsTr("Waiting for background face indexing…")
        }
    }
    Timer {
        interval: 3000
        repeat: true
        running: panel.shown
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
            nameInput.text = edits[row.key]?.name || ""
            page = 0
            rows = []
            nextPage = false
            load()
        }
    }
    function back() {
        if (!anchor) {
            close()
            return
        }
        anchor = null
        page = 0
        rows = []
        nextPage = false
        load()
    }
    function writeEdit(key, face, value) {
        const saved = Object.assign({}, pinhole.state[key] || {})
        if (value.name || value.rejected?.length) saved[face] = value
        else delete saved[face]
        pinhole.state[key] = saved
        if (stateKey === key) {
            edits = saved
            if (anchor?.key === face) nameInput.text = value.name || ""
            selected = Math.max(0, Math.min(selected, visibleRows.length - 1))
        }
    }
    function edit(value, label) {
        const key = stateKey
        const face = anchor.key
        const before = edits[face] || {}
        writeEdit(key, face, value)
        pinhole.did(label, () => writeEdit(key, face, before), () => writeEdit(key, face, value))
    }
    function move(delta) {
        if (!visibleRows.length) return
        selected = Math.max(0, Math.min(visibleRows.length - 1, selected + delta))
        grid.positionViewAtIndex(selected, GridView.Contain)
    }

    function tabKey(event) {
        return (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
    }
    Keys.onShortcutOverride: event => { if (tabKey(event)) event.accepted = true }
    Keys.onPressed: event => {
        if (!tabKey(event)) return
        const controls = [closeButton, backButton, openButton, rejectButton,
            nameInput.input, save, grid, previous, next].filter(item =>
            item.visible && item.activeFocusOnTab && item.usable !== false)
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
                    text: panel.anchor ? panel.edits[panel.anchor.key]?.name || qsTr("Suggested group") : qsTr("People")
                }
                ShellButton { id: closeButton; text: "✕"; onTapped: panel.close() }
            }
            Flow {
                width: parent.width
                spacing: 8
                ShellButton { id: backButton; visible: !!panel.anchor; text: qsTr("Back to people"); hint: "Esc"; onTapped: panel.back() }
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
                    visible: !!panel.anchor
                    text: qsTr("Different person")
                    usable: !!panel.selection && panel.selection.key !== panel.anchor?.key
                    onTapped: panel.edit(Object.assign({}, panel.edits[panel.anchor.key], {
                        rejected: panel.rejected.concat([panel.selection.key])
                    }), qsTr("removing a face from a group"))
                }
            }
            Row {
                visible: !!panel.anchor
                width: parent.width
                spacing: 8
                FieldInput {
                    id: nameInput
                    objectName: "peopleName"
                    width: Math.max(0, parent.width - save.width - parent.spacing)
                    height: save.height
                    placeholder: qsTr("Person’s name")
                    input.maximumLength: 100
                    onAccepted: save.press()
                }
                ShellButton {
                    id: save
                    text: qsTr("Save name")
                    usable: nameInput.text.trim() !== (panel.edits[panel.anchor?.key]?.name || "")
                    onTapped: panel.edit(Object.assign({}, panel.edits[panel.anchor.key], { name: nameInput.text.trim() }),
                                         qsTr("naming a person"))
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
            objectName: "peopleGrid"
            readonly property int columns: Math.max(1, Math.floor(width / 154))
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            cellWidth: width / columns
            cellHeight: cellWidth
            model: panel.visibleRows
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
                label: group ? panel.edits[row.key]?.name || qsTr("Group · %1 photos").arg(row.count)
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
            visible: panel.status.phase === "ready" && grid.count === 0 && !panel.readJob
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: panel.anchor ? qsTr("This group has no visible matches.") : qsTr("No suggested groups on this page.")
        }
        Row {
            id: footer
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
    }
}
